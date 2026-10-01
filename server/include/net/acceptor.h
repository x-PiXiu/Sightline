// Acceptor —— 监听 socket 的封装：只做一件事（accept 循环 + 回调新连接）
// 它不知道连接是什么、给谁用——参照毕设 http_server 的 listen_channel_ 模式
#pragma once
#include <functional>
#include <memory>
#include "common/network/event_loop.h"
#include "common/network/inet_address.h"
#include "common/network/socket.h"
#include "common/network/channel.h"
#include <fcntl.h>
#include <cstring>   // strerror

namespace net {

using common::network::EventLoop;
using common::network::InetAddress;
using common::network::Socket;
using common::network::Channel;

    class Acceptor {
    public:
        using NewConnectionCallback = std::function<void(int connfd, const InetAddress& peer)>;

        Acceptor(EventLoop* loop, const InetAddress& listen_addr)
            : loop_(loop),
              socket_(new Socket()),
              channel_(new Channel(loop, socket_->fd())),
              idle_fd_(::open("/dev/null", O_RDONLY | O_CLOEXEC)) {
            socket_->setReuseAddr(true);
            socket_->bindAddress(listen_addr);
            socket_->listen();
            channel_->setReadCallback([this] { handleAccept(); });
            channel_->enableReading();
        }

        ~Acceptor() {
            channel_->disableAll();
            channel_->remove();
            if (idle_fd_ >= 0) ::close(idle_fd_);
        }

        void setNewConnectionCallback(NewConnectionCallback cb) { callback_ = std::move(cb); }

    private:
        // accept 循环：一次性清空就绪连接（非阻塞 fd，读空返回 EAGAIN）
        void handleAccept() {
            InetAddress peer;
            while (true) {
                int connfd = socket_->accept(&peer);
                if (connfd >= 0) {
                    if (callback_) callback_(connfd, peer);
                    else ::close(connfd);
                    continue;
                }
                // EAGAIN / EWOULDBLOCK：没有更多就绪连接
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                // EMFILE：fd 耗尽——用预留 idle_fd 抢占再释放，避免惊群死循环
                if (errno == EMFILE) {
                    ::close(idle_fd_);
                    idle_fd_ = ::accept(socket_->fd(), nullptr, nullptr);
                    ::close(idle_fd_);
                    idle_fd_ = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
                    LOG_ERROR("Acceptor: fd 耗尽（EMFILE），拒绝新连接");
                    break;
                }
                LOG_ERROR(std::string("Acceptor: accept 错误 errno=") + strerror(errno));
                break;
            }
        }

        EventLoop* loop_;
        std::unique_ptr<Socket> socket_;
        std::unique_ptr<Channel> channel_;
        NewConnectionCallback callback_;
        int idle_fd_ = -1;   // EMFILE 防护预留（毕设没有的防御——交接批注第 8 条）
    };

} // namespace net
