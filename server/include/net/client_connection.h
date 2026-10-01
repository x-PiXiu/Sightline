// ClientConnection —— 一条客户端连接的生命周期托管
// 关键点：
// 1. enable_shared_from_this——事件回调触发时对象必须活着（生命周期由外部 map 持有）
// 2. 全部操作跑在所属 EventLoop 线程（单线程版 = 主线程）
// 3. 读循环到 EAGAIN（ET 模式必需；connfd 已由 accept4 设为非阻塞）
// 4. 消息切帧委托给 adapters 的 FrameCodec——本类只管字节（net 层不知道什么是消息）
#pragma once
#include <functional>
#include <memory>
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

        // 仅由 main（连接表持有者）调用：构造后必须存入 map，否则回调触发时对象已亡
        ClientConnection(EventLoop* loop, int connfd, InetAddress peer)
            : loop_(loop),
              channel_(new Channel(loop, connfd)),
              peerAddr_(std::move(peer)) {
            channel_->setReadCallback([this] { handleRead(); });
            channel_->setCloseCallback([this] { handleClose(); });
            channel_->enableReading();
        }

        ~ClientConnection() = default;

        void setMessageCallback(MessageCallback cb) { messageCallback_ = std::move(cb); }
        void setCloseCallback(CloseCallback cb) { closeCallback_ = std::move(cb); }

        // 发送一条完整帧（第 1 期：心跳包 <100B，直接 write；SendBuffer+EPOLLOUT 为演进项）
        void send(uint16_t msgid, const std::string& payload) {
            std::string out;
            adapters::FrameCodec::encode(msgid, payload, out);
            ssize_t n = ::write(channel_->fd(), out.data(), out.size());
            if (n < 0) {
                LOG_ERROR(std::string("ClientConnection: write 失败 errno=") + strerror(errno));
            } else if (static_cast<std::size_t>(n) < out.size()) {
                // 部分写：第 1 期直接丢弃残余并告警（心跳包极小，实测不应发生）
                LOG_ERROR("ClientConnection: 部分写 " + std::to_string(n) + "/" +
                          std::to_string(out.size()) + " —— 出现即触发 SendBuffer 演进项");
            }
        }

        const InetAddress& peerAddr() const { return peerAddr_; }
        int fd() const { return channel_->fd(); }   // 连接表索引用

        // 断开：禁事件 → 从 epoll 摘除 → 关 fd → 通知持有者清 map
        void handleClose() {
            channel_->disableAll();
            channel_->remove();
            ::close(channel_->fd());
            LOG_INFO("ClientConnection: 连接关闭 " + peerAddr_.toIpPort());
            if (closeCallback_) closeCallback_(shared_from_this());
        }

    private:
        void handleRead() {
            char tmp[65536];
            while (true) {
                ssize_t n = ::read(channel_->fd(), tmp, sizeof tmp);
                if (n > 0) {
                    recvBuf_.append(tmp, static_cast<std::size_t>(n));
                    // 切帧循环：缓冲里可能积了多个完整帧
                    // 半包留在缓冲（FrameCodec 返回 false 时不消费），下次 read 到齐再切
                    adapters::Frame frame;
                    while (adapters::FrameCodec::tryDecode(recvBuf_, frame)) {
                        if (messageCallback_) messageCallback_(shared_from_this(), frame.msgid, frame.payload);
                    }
                    continue;
                }
                if (n == 0) { handleClose(); return; }                       // 对端正常关闭
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;         // 读干净了（ET 必须）
                LOG_ERROR(std::string("ClientConnection: read 错误 errno=") + strerror(errno));
                handleClose();
                return;
            }
        }

        EventLoop* loop_;
        std::unique_ptr<Channel> channel_;
        InetAddress peerAddr_;
        net::Buffer recvBuf_;
        MessageCallback messageCallback_;
        CloseCallback closeCallback_;
    };

} // namespace net
