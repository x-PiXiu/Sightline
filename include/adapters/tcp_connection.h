// adapters/tcp_connection.h —— 通用 TCP 连接（毕设缺失的连接层，本次新写）
// 关键设计：
// 1. ET 读：循环 readv 到 EAGAIN（websocket 版毕设里修过的坑，此处原生正确）
// 2. 写优化：先尝试直写，剩余进输出缓冲并关注 EPOLLOUT，发完即取消关注
// 3. 生命周期：enable_shared_from_this + Channel::tie()，事件处理期间连接不会被销毁
// 本类只懂字节，不懂协议与玩法（协议在 ProtocolCodec，玩法在 application/domain）

#pragma once
#include <atomic>
#include <cstdio>
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

class TcpConnection : public std::enable_shared_from_this<TcpConnection> {
public:
    using MessageCallback = std::function<void(const TcpConnectionPtr&, Buffer&)>;
    using CloseCallback  = std::function<void(const TcpConnectionPtr&)>;

    enum class State { kConnected, kDisconnected };

    TcpConnection(common::network::EventLoop* loop, uint64_t id, int sockfd,
                  const common::network::InetAddress& peer_addr)
        : loop_(loop), id_(id), state_(State::kConnected),
          socket_(std::make_unique<common::network::Socket>(sockfd)),
          channel_(std::make_unique<common::network::Channel>(loop, sockfd)),
          peer_addr_(peer_addr) {
        // FPS 低延迟必开：关 Nagle
        socket_->setTcpNoDelay(true);
        channel_->setReadCallback([this] { handleRead(); });
        channel_->setWriteCallback([this] { handleWrite(); });
        channel_->setCloseCallback([this] { handleClose(); });
        channel_->setErrorCallback([this] { handleError(); });
    }

    ~TcpConnection() = default;

    void establish() {
        // tie：事件处理期间保活（防回调中销毁自身导致 Channel 悬空）
        channel_->tie(shared_from_this());
        channel_->enableReading();
    }

    uint64_t id() const { return id_; }
    bool connected() const { return state_ == State::kConnected; }
    const common::network::InetAddress& peerAddress() const { return peer_addr_; }

    void setMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

    // 发送（可在任意线程调用；非 IO 线程则投递到 IO 线程）
    void send(const void* data, size_t len) {
        if (state_ != State::kConnected) return;
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

    // 主动关闭（心跳踢人）
    void shutdown() {
        if (state_.exchange(State::kDisconnected) == State::kConnected) {
            loop_->queueInLoop([self = shared_from_this()] { self->cleanup(); });
        }
    }

private:
    void sendInLoop(const void* data, size_t len) {
        if (state_ != State::kConnected) return;
        ssize_t nwrote = 0;
        size_t remaining = len;

        // 输出缓冲为空才尝试直写（避免乱序）
        if (!channel_->isWriting() && output_buffer_.readableBytes() == 0) {
            nwrote = ::write(socket_->fd(), data, len);
            if (nwrote >= 0) {
                remaining = len - nwrote;
            } else {
                nwrote = 0;
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    LOG_ERROR("TcpConnection::sendInLoop write error, conn=" + std::to_string(id_));
                }
            }
        }

        if (remaining > 0) {
            output_buffer_.append(static_cast<const char*>(data) + nwrote, remaining);
            if (!channel_->isWriting()) channel_->enableWriting();   // 关注可写，等 EPOLLOUT
        }
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
        if (!channel_->isWriting()) return;
        while (output_buffer_.readableBytes() > 0) {
            ssize_t n = ::write(socket_->fd(), output_buffer_.peek(),
                                output_buffer_.readableBytes());
            if (n > 0) {
                output_buffer_.retrieve(n);
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;   // 内核发送缓冲满，下次再来
            if (errno == EINTR) continue;
            LOG_ERROR("TcpConnection::handleWrite error, conn=" + std::to_string(id_));
            break;
        }
        if (output_buffer_.readableBytes() == 0) {
            channel_->disableWriting();    // 发完立即取消关注（LT 模式正确姿势）
        }
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

    // 统一清理：摘事件 → 通知上层（上层 erase 掉 shared_ptr 后对象销毁，Channel 由 tie 守卫护送）
    void cleanup() {
        channel_->disableAll();
        channel_->remove();
        if (close_cb_) close_cb_(shared_from_this());
    }

    common::network::EventLoop* loop_;
    uint64_t id_;
    std::atomic<State> state_;
    std::unique_ptr<common::network::Socket> socket_;      // RAII：析构即 close(fd)
    std::unique_ptr<common::network::Channel> channel_;
    common::network::InetAddress peer_addr_;
    Buffer input_buffer_;
    Buffer output_buffer_;
    MessageCallback message_cb_;
    CloseCallback close_cb_;
};

} // namespace sightline::adapters
