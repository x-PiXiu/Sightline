// adapters/acceptor.h —— 监听与接入：accept 循环排干到 EAGAIN（配合读事件 ET 模式）

#pragma once
#include <functional>
#include <memory>
#include <unistd.h>
#include "net/event_loop.h"
#include "net/channel.h"
#include "net/socket.h"
#include "net/inet_address.h"
#include "logger/logger.h"

namespace sightline::adapters {

class Acceptor {
public:
    using NewConnectionCallback = std::function<void(int connfd, const common::network::InetAddress& peer)>;

    Acceptor(common::network::EventLoop* loop, const common::network::InetAddress& listen_addr)
        : loop_(loop),
          listen_socket_(std::make_unique<common::network::Socket>()),
          channel_(std::make_unique<common::network::Channel>(loop, -1)),
          listen_addr_(listen_addr) {
        listen_socket_->applySocketOptions(/*reuse_addr=*/true, /*reuse_port=*/false,
                                           /*tcp_no_delay=*/false, /*keep_alive=*/false);
        // Reactor 必须非阻塞：accept 排干循环依赖 EAGAIN 而非阻塞
        listen_socket_->setNonBlocking(true);
        listen_socket_->bindAddress(listen_addr_);
        // Channel 先占位（fd 未知），listen() 成功后再绑定真实 fd
    }

    ~Acceptor() = default;

    void setNewConnectionCallback(NewConnectionCallback cb) { new_conn_cb_ = std::move(cb); }

    void listen() {
        listen_socket_->listen();
        // 用已监听的 fd 重建 Channel（Channel 构造后 fd 不可变，这里它尚未注册过事件，
        // 直接重新创建是安全的）
        channel_ = std::make_unique<common::network::Channel>(loop_, listen_socket_->fd());
        channel_->setReadCallback([this] { handleRead(); });
        channel_->enableReading();    // 读事件含 EPOLLET：必须排干 accept
    }

private:
    void handleRead() {
        // ET：循环 accept 直到 EAGAIN/EMFILE
        while (true) {
            common::network::InetAddress peer_addr;
            int connfd = listen_socket_->accept(&peer_addr);
            if (connfd >= 0) {
                if (new_conn_cb_) new_conn_cb_(connfd, peer_addr);
                else ::close(connfd);
            } else {
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;    // 排干完毕
                if (errno == EMFILE || errno == ENFILE) {
                    // fd 耗尽：不 break 会热循环，直接退出（生产做法是 idle fd 腾挪）
                    // LOG via logger
                    common::logger::Logger::getInstance().error(
                        "Acceptor: fd exhausted", __FILE__, __LINE__);
                    break;
                }
                if (errno == EINTR) continue;
                break;
            }
        }
    }

    common::network::EventLoop* loop_;
    std::unique_ptr<common::network::Socket> listen_socket_;
    std::unique_ptr<common::network::Channel> channel_;
    common::network::InetAddress listen_addr_;
    NewConnectionCallback new_conn_cb_;
};

} // namespace sightline::adapters
