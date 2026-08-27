// adapters/tcp_connection.h —— 通用 TCP 连接（毕设缺失的连接层，本次新写）
//
// 本轮按整洁架构"机制与策略分离"补齐三块能力：
//   1. 优雅关闭：kDisconnecting 状态 + 输出排空后再断（策略：正常退出走排空，
//      踢人等异常走 forceClose 的 RST——两种策略共用同一套机制）
//   2. 高水位保护：输出缓冲超过水位时回调通告一次（机制），
//      "断开还是降级"由装配层注入的策略决定
//   3. 发送限速：内嵌令牌桶（机制），配额由组装层注入（策略，0=不限）；
//      令牌耗尽时借所属 loop 的时间轮定时重试
//
// 不变的边界：本类只懂字节，不懂协议与玩法（协议在 ProtocolCodec，玩法在 application/domain）

#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "adapters/buffer.h"
#include "net/event_loop.h"
#include "net/channel.h"
#include "net/socket.h"
#include "net/inet_address.h"
#include "logger/logger.h"

namespace sightline::adapters {

class TcpConnection;
using TcpConnectionPtr = std::shared_ptr<TcpConnection>;

// 令牌桶：按时间补充发送额度（毫秒粒度足够游戏服务器使用）
struct TokenBucket {
    size_t rate_bytes_per_sec = 0;   // 0 = 不限速
    size_t burst_bytes = 0;
    size_t available = 0;
    std::chrono::steady_clock::time_point last_refill = std::chrono::steady_clock::now();

    bool enabled() const { return rate_bytes_per_sec > 0; }

    size_t tryConsume(size_t want) {
        if (!enabled()) return want;
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refill).count();
        if (elapsed_ms > 0) {
            size_t refill =
                static_cast<size_t>(elapsed_ms) * rate_bytes_per_sec / 1000;
            available = std::min(burst_bytes, available + refill);
            last_refill = now;
        }
        size_t granted = std::min(want, available);
        available -= granted;
        return granted;
    }
};

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback        = std::function<void(const TcpConnectionPtr&, Buffer&)>;
    using CloseCallback          = std::function<void(const TcpConnectionPtr&)>;
    using HighWaterMarkCallback  = std::function<void(const TcpConnectionPtr&, size_t pending)>;
    using WriteCompleteCallback  = std::function<void(const TcpConnectionPtr&)>;

    enum class State { kConnected, kDisconnecting, kDisconnected };

    struct Options {
        size_t high_water_mark    = 4 * 1024 * 1024;   // 输出缓冲水位（慢客户端判定）
        size_t rate_bytes_per_sec = 0;                 // 发送限速（0 = 不限）
        size_t rate_burst_bytes   = 64 * 1024;         // 令牌桶突发容量
    };

    TcpConnection(common::network::EventLoop* loop, uint64_t id, int sockfd,
                  const common::network::InetAddress& peer_addr)
        : TcpConnection(loop, id, sockfd, peer_addr, Options()) {}

    TcpConnection(common::network::EventLoop* loop, uint64_t id, int sockfd,
                  const common::network::InetAddress& peer_addr, const Options& opts)
        : loop_(loop), id_(id), state_(State::kConnected),
          socket_(std::make_unique<common::network::Socket>(sockfd)),
          channel_(std::make_unique<common::network::Channel>(loop, sockfd)),
          peer_addr_(peer_addr), opts_(opts) {
        socket_->setTcpNoDelay(true);    // FPS 低延迟必开：关 Nagle
        bucket_.rate_bytes_per_sec = opts.rate_bytes_per_sec;
        bucket_.burst_bytes = opts.rate_burst_bytes;
        bucket_.available = opts.rate_burst_bytes;
        channel_->setReadCallback([this] { handleRead(); });
        channel_->setWriteCallback([this] { handleWrite(); });
        channel_->setCloseCallback([this] { handleClose(); });
        channel_->setErrorCallback([this] { handleError(); });
    }

    ~TcpConnection() = default;

    // 必须在所属 loop 线程调用（跨线程建立由外层 runInLoop 保证）
    void establish() {
        channel_->tie(shared_from_this());   // 事件处理期间保活
        channel_->enableReading();
    }

    uint64_t id() const { return id_; }
    bool connected() const { return state_ == State::kConnected; }
    const common::network::InetAddress& peerAddress() const { return peer_addr_; }
    size_t pendingOutput() const { return output_buffer_.readableBytes(); }

    void setMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }
    void setHighWaterMarkCallback(HighWaterMarkCallback cb) { high_water_cb_ = std::move(cb); }
    void setWriteCompleteCallback(WriteCompleteCallback cb) { write_complete_cb_ = std::move(cb); }

    // 发送（任意线程可调；非所属线程则投递）
    void send(const void* data, size_t len) {
        if (state_ == State::kDisconnected) return;
        if (loop_->isInLoopThread()) {
            sendInLoop(data, len);
        } else {
            std::string msg(static_cast<const char*>(data), len);
            loop_->queueInLoop([self = shared_from_this(), msg = std::move(msg)] {
                self->sendInLoop(msg.data(), msg.size());
            });
        }
    }
    void send(const std::string& s) { send(s.data(), s.size()); }

    // 优雅关闭：有积压则排空后再断（kDisconnecting）
    void shutdown() {
        loop_->runInLoop([self = shared_from_this()] { self->shutdownInLoop(); });
    }

    // 立即关闭（异常路径：高水位踢人等；Socket::forceClose 内部 RST）
    void forceClose() {
        loop_->runInLoop([self = shared_from_this()] {
            if (self->state_.exchange(State::kDisconnected) == State::kDisconnected) return;
            self->cleanup();
        });
    }

private:
    void sendInLoop(const void* data, size_t len) {
        if (state_ == State::kDisconnected) return;

        size_t grantable = bucket_.tryConsume(len);
        const char* p = static_cast<const char*>(data);
        size_t remaining = len;
        ssize_t nwrote = 0;

        // 无积压时尝试直写（只写令牌允许的部分）
        if (!channel_->isWriting() && output_buffer_.readableBytes() == 0 && grantable > 0) {
            nwrote = ::write(socket_->fd(), p, grantable);
            if (nwrote > 0) {
                remaining = len - static_cast<size_t>(nwrote);
            } else {
                nwrote = 0;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("TcpConnection::sendInLoop write error, conn=" + std::to_string(id_));
                }
            }
        }

        if (remaining > 0) {
            output_buffer_.append(p + nwrote, remaining);
            size_t pending = output_buffer_.readableBytes();

            // 高水位：慢客户端保护（只通告一次，排空后复位）
            if (pending > opts_.high_water_mark && !high_water_notified_) {
                high_water_notified_ = true;
                if (high_water_cb_) {
                    high_water_cb_(shared_from_this(), pending);
                } else {
                    LOG_WARNING("conn#" + std::to_string(id_) + " output high water: " +
                                std::to_string(pending) + " bytes");
                }
            }

            if (!channel_->isWriting()) channel_->enableWriting();

            // 限速导致的"socket 可写但没令牌"：借时间轮定时重试
            if (bucket_.enabled() && bucket_.available == 0 && !flush_timer_armed_) {
                armFlushRetry();
            }
        }
    }

    void armFlushRetry() {
        flush_timer_armed_ = true;
        loop_->runAfter(100, [self = shared_from_this()] {
            self->flush_timer_armed_ = false;
            if (self->state_ != State::kDisconnected && self->output_buffer_.readableBytes() > 0) {
                self->handleWrite();
            }
        });
    }

    void handleRead() {
        // ET：循环读到 EAGAIN；readv 返回 0 表示对端关闭
        while (true) {
            int saved_errno = 0;
            ssize_t n = input_buffer_.readFd(socket_->fd(), saved_errno);
            if (n > 0) continue;                        // 继续排干
            if (n == 0) { handleClose(); return; }      // 对端关闭
            if (saved_errno == EAGAIN || saved_errno == EWOULDBLOCK) break;   // 排干完毕
            if (saved_errno == EINTR) continue;
            handleError();
            return;
        }
        if (input_buffer_.readableBytes() > 0 && message_cb_) {
            message_cb_(shared_from_this(), input_buffer_);
        }
    }

    void handleWrite() {
        while (output_buffer_.readableBytes() > 0) {
            size_t granted = bucket_.tryConsume(output_buffer_.readableBytes());
            if (granted == 0) {
                // 令牌耗尽（socket 可能仍可写）：关写关注 + 定时重试
                channel_->disableWriting();
                if (!flush_timer_armed_) armFlushRetry();
                return;
            }
            ssize_t n = ::write(socket_->fd(), output_buffer_.peek(), granted);
            if (n > 0) {
                output_buffer_.retrieve(n);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 内核发送缓冲满
            if (errno == EINTR) continue;
            LOG_ERROR("TcpConnection::handleWrite error, conn=" + std::to_string(id_));
            break;
        }
        if (output_buffer_.readableBytes() == 0) {
            channel_->disableWriting();          // 发完立即取消关注（LT 写事件纪律）
            high_water_notified_ = false;
            if (write_complete_cb_) write_complete_cb_(shared_from_this());
            if (state_ == State::kDisconnecting) cleanup();   // 优雅关闭：排空即断
        }
    }

    void shutdownInLoop() {
        if (state_.exchange(State::kDisconnecting) == State::kDisconnected) return;
        if (output_buffer_.readableBytes() == 0 && !channel_->isWriting()) {
            cleanup();           // 无积压：直接进入关闭
        }
        // 有积压：等 handleWrite 排空后 cleanup
    }

    void handleClose() {
        if (state_.exchange(State::kDisconnected) == State::kDisconnected) return;
        cleanup();
    }

    void handleError() {
        int err = 0;
        socklen_t len = sizeof err;
        ::getsockopt(socket_->fd(), SOL_SOCKET, SO_ERROR, &err, &len);
        LOG_WARNING("TcpConnection error, conn=" + std::to_string(id_) +
                    ", errno=" + std::to_string(err));
        handleClose();
    }

    // 统一清理：摘事件 → 通知上层（上层 erase 掉 shared_ptr 后对象销毁，tie 守卫护送）
    void cleanup() {
        channel_->disableAll();
        channel_->remove();
        if (close_cb_) close_cb_(shared_from_this());
    }

    common::network::EventLoop* loop_;
    uint64_t id_;
    std::atomic<State> state_;
    std::unique_ptr<common::network::Socket> socket_;      // RAII：析构即优雅 close(fd)
    std::unique_ptr<common::network::Channel> channel_;
    common::network::InetAddress peer_addr_;
    Options opts_;
    TokenBucket bucket_;
    bool flush_timer_armed_ = false;                       // 仅所属 loop 线程访问
    bool high_water_notified_ = false;

    Buffer input_buffer_;
    Buffer output_buffer_;
    MessageCallback message_cb_;
    CloseCallback close_cb_;
    HighWaterMarkCallback high_water_cb_;
    WriteCompleteCallback write_complete_cb_;
};

} // namespace sightline::adapters
