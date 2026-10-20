// ClientConnection —— 一条客户端连接的生命周期托管（M2 主从 Reactor 形态）
// 关键点：
// 1. 构造只存参数；Channel 注册在 Start()——必须在归属 loop 线程调用（updateChannel 有线程断言）
//    主从分工：对象可在 main 构造（连接表/发送视角），IO（收/发/摘除）全归属 worker 线程
// 2. send() 任意线程可调：直接 write 快路径 + outBuf 兜底 + EPOLLOUT 动态挂摘（armWriting 经
//    queueInLoop 投递到归属线程动 epoll——epoll 操作永远不跨线程）
// 3. handleClose() 在归属线程执行完整收尾（摘 epoll + close fd + channel_.reset()）——
//    收尾后对象成为"无 Channel 裸对象"，任意线程析构安全（shared_ptr 多线程持有的最后一根稻草）
// 4. 读循环到 EAGAIN；切帧委托 adapters FrameCodec——net 层不知道什么是消息
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/buffer.h"
#include "adapters/frame_codec.h"
#include <unistd.h>
#include <cerrno>
#include <cstring>   // strerror

namespace net {

using common::network::EventLoop;
using common::network::InetAddress;
using common::network::Channel;

    class ClientConnection : public std::enable_shared_from_this<ClientConnection> {
    public:
        using Ptr = std::shared_ptr<ClientConnection>;
        using MessageCallback = std::function<void(const Ptr&, uint16_t msgid, const std::string& payload)>;
        using CloseCallback = std::function<void(const Ptr&)>;

        // 构造只存参数（Channel 稍后在归属 loop 线程 Start()——对象生命周期与 epoll 注册分家）
        ClientConnection(EventLoop* loop, int connfd, InetAddress peer)
            : loop_(loop), fd_(connfd), peerAddr_(std::move(peer)) {}

        ~ClientConnection() = default;

        // 在归属 loop 线程调用（main 构造后经 queueInLoop 投递过来）——Channel 注册 + 开读
        void Start() {
            if (channel_) return;                            // 防重复（防御）
            channel_ = std::make_unique<Channel>(loop_, fd_);
            channel_->setReadCallback([this] { handleRead(); });
            channel_->setCloseCallback([this] { handleClose(); });
            channel_->setWriteCallback([this] { handleWrite(); });
            channel_->enableReading();
        }

        void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
        void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

        // 任意线程可调（业务 main 发帧；内部保证 epoll 操作只发生在归属线程）
        void send(uint16_t msgid, const std::string& payload) {
            // bClosed_ 在收尾排队时即置位、::close 在其后的收尾任务里——此刻 fd 尚未关也即将关，
            // 对端已死，写只是给日志刷 EBADF 洪峰（档二压测实测 6204 行）——直接丢弃
            if (bClosed_.load(std::memory_order_relaxed)) return;
            std::string out;
            adapters::FrameCodec::encode(msgid, payload, out);
            std::lock_guard<std::mutex> lk(outMutex_);
            if (outBuf_.empty()) {
                const ssize_t n = ::write(fd_, out.data(), out.size());
                if (n > 0) {
                    if (static_cast<std::size_t>(n) == out.size()) return;      // 快路径：一次发完
                    outBuf_.append(out.data() + n, out.size() - static_cast<std::size_t>(n));
                } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR(std::string("ClientConnection: write 失败 errno=") + strerror(errno));
                    return;                                                       // 硬错丢弃（关闭由读路径负责）
                }                                                                 // EAGAIN：整帧入缓冲等 EPOLLOUT
            } else {
                outBuf_.append(out);                                              // 已有积压：顺序追加
            }
            armWritingLocked();
        }

        EventLoop* ownerLoop() const { return loop_; }
        const InetAddress& peerAddr() const { return peerAddr_; }
        int fd() const { return fd_; }                     // 连接表索引（fd 不随 Channel 收尾失效）

        // 断开收尾：归属线程执行——摘 epoll → 关 fd → channel_ 置空（此后任意线程析构安全）
        // ⚠ 立即收尾是禁区：本函数常在 Channel::handleEvent 回调链内被调（对端关闭/读错）——
        //   立即 reset = 在自己事件回调里释放自己（TSAN heap-use-after-free 实锤，handleEvent 尾部还要写成员）。
        //   修：queueInLoop 延迟到批次外执行（epoll 批次结束、回调栈弹干净后再动手）。
        void handleClose() {
            if (bClosed_.exchange(true)) return;            // 双关守卫（异步关与读路径关竞态；防 fd 复用二次 close）
            loop_->queueInLoop([self = shared_from_this()] {
                if (self->channel_) {
                    self->channel_->disableAll();
                    self->channel_->remove();
                    self->channel_.reset();
                }
                ::close(self->fd_);
                LOG_INFO("ClientConnection: 连接关闭 " + self->peerAddr_.toIpPort());
                if (self->closeCallback_) self->closeCallback_(self);
            });
        }

        // 任意线程调用的礼貌版：投归属线程执行收尾（KICKED forceClose 等 main 侧路径必须走这里——epoll 不跨线程）
        void closeAsync() {
            loop_->queueInLoop([self = shared_from_this()] { self->handleClose(); });
        }

    private:
        // 调用方须持 outMutex_：首次积压时挂 EPOLLOUT（经 queueInLoop 投归属线程——epoll 不跨线程）
        void armWritingLocked() {
            if (bWritingArmed_) return;
            bWritingArmed_ = true;
            loop_->queueInLoop([self = shared_from_this()] {
                if (self->channel_) self->channel_->enableWriting();   // 收尾竞态防御
            });
        }

        void handleWrite() {                             // 归属 loop 线程（EPOLLOUT 触发）
            bool bDrained = false;
            {
                std::lock_guard<std::mutex> lk(outMutex_);
                while (!outBuf_.empty()) {
                    const ssize_t n = ::write(fd_, outBuf_.data(), outBuf_.size());
                    if (n > 0) {
                        outBuf_.erase(0, static_cast<std::size_t>(n));
                        continue;
                    }
                    if (errno == EAGAIN || errno == EWOULDBLOCK) return;   // 内核缓冲满：等下次 EPOLLOUT
                    LOG_ERROR(std::string("ClientConnection: 写硬错 errno=") + strerror(errno));
                    outBuf_.clear();
                    break;
                }
                bDrained = true;
            }
            if (bDrained && bWritingArmed_) {            // 排空：摘 EPOLLOUT（LT 下不摘会被可写态疯狂触发）
                bWritingArmed_ = false;
                if (channel_) channel_->disableWriting();
            }
        }

        void handleRead() {
            char tmp[65536];
            while (true) {
                const ssize_t n = ::read(fd_, tmp, sizeof tmp);
                if (n > 0) {
                    recvBuf_.append(tmp, static_cast<std::size_t>(n));
                    // 切帧循环：半包留缓冲，下次 read 到齐再切
                    adapters::Frame frame;
                    while (adapters::FrameCodec::tryDecode(recvBuf_, frame)) {
                        if (messageCallback_) messageCallback_(shared_from_this(), frame.msgid, frame.payload);
                    }
                    continue;
                }
                if (n == 0) { handleClose(); return; }                       // 对端正常关闭
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;         // 读干净了
                LOG_ERROR(std::string("ClientConnection: read 错误 errno=") + strerror(errno));
                handleClose();
                return;
            }
        }

        EventLoop* loop_;                                // 归属 loop（主从：worker 的事件循环）
        int fd_;                                         // fd 直存（Channel 收尾后仍可安全读）
        std::unique_ptr<Channel> channel_;               // 归属线程创建/销毁；收尾后置空
        InetAddress peerAddr_;
        net::Buffer recvBuf_;
        std::mutex outMutex_;                            // 出站缓冲互斥（main 写 / 归属线程排空）
        std::string outBuf_;                             // 发送积压（EAGAIN 兜底）
        bool bWritingArmed_ = false;                     // EPOLLOUT 已挂标记
        std::atomic<bool> bClosed_{false};                // 关闭一次即终（双关守卫——防 fd 复用二次 close）
        MessageCallback messageCallback_;
        CloseCallback closeCallback_;
    };

} // namespace net
