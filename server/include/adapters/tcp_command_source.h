// tcp_command_source.h —— ep08 步骤 9 前置：运维命令 TCP 口（与 stdout 日志流分离）
// 动机：前台 Run 窗口心跳日志刷屏，stdin 输入被日志淹没——命令从独立端口进：
//   nc 127.0.0.1 <port> 敲命令（reload/rollback/version/patch），新终端/远程皆可。
// A1 纪律：读线程只入队（复用 StdinCommandSource 的锁队列），消费归编排器主循环。
// header-only（POSIX 阻塞式·独立线程·进程退出即亡）；port=0 表示关闭。
#pragma once
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapters/stdin_command_source.h"
#include "logger/logger.h"

namespace adapters {

class TcpCommandListener {
public:
    TcpCommandListener(StdinCommandSource& sink, unsigned port) : sink_(sink) {
        if (port == 0) return;                                  // 0=显式关闭
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { LOG_WARNING("[命令口] socket 创建失败——stdin 仍可用"); return; }
        int on = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd_, 4) < 0) {
            LOG_WARNING("[命令口] 监听 :" + std::to_string(port) +
                        " 失败（端口占用？）——stdin 仍可用");
            ::close(fd_);
            fd_ = -1;
            return;
        }
        th_ = std::thread([this, port, &sink] {
            LOG_INFO("[命令口] 就绪 ::" + std::to_string(port) +
                     " —— nc 127.0.0.1 " + std::to_string(port) +
                     " 后敲命令（reload [note] / rollback / version / patch <name> / help）");
            while (!stop_.load()) {
                const int c = ::accept(fd_, nullptr, nullptr);
                if (c < 0) break;
                std::thread([c, this] { Serve(c, *this); }).detach();
            }
        });
    }

    ~TcpCommandListener() {
        stop_.store(true);
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);   // 唤醒阻塞的 accept（Linux：shutdown 监听套接字 → accept 返回错误）
            ::close(fd_);
        }
        // ep09 关服修复（与 stdin 线程同类别·2026-11-12 实测钉死）：detach→join——原 detach 让
        // accept 线程活过宿主（读已析构的 stop_/fd_=UAF·exit 期隐患）；shutdown 唤醒 accept →
        // c<0 break → 线程在对象存活期内退出，join 立即返回。
        if (th_.joinable()) th_.join();
        // 已知边界（记录不处理）：每连接 Serve 线程仍 detach（阻塞于 recv(c)，c 按值捕获）——
        //   关服时其不再触碰 self 直到客户端断开；根治须 Serve 线程注册表逐个 join（压测期清单）。
    }

    /** 命令回执回写（2026-11-08 拍摄观感修正）：编排器处理的响应此前只落服务器日志——
     *  nc 窗口敲完命令无回显=观感"没反应"。主循环 Notice 变化时调 Broadcast，
     *  回执发到所有活跃命令连接（命令与响应同窗） */
    void Broadcast(const std::string& text) {
        std::lock_guard<std::mutex> lk(clisMx_);
        for (int c : clis_) {
            std::string out = text + "\n";
            ssize_t ignored = ::send(c, out.data(), out.size(), MSG_NOSIGNAL);
            (void)ignored;
        }
    }

private:
    static void Serve(int c, TcpCommandListener& self) {
        {
            std::lock_guard<std::mutex> lk(self.clisMx_);
            self.clis_.push_back(c);
        }
        std::string buf;
        char tmp[512];
        ssize_t n;
        while ((n = ::recv(c, tmp, sizeof(tmp), 0)) > 0) {
            buf.append(tmp, static_cast<size_t>(n));
            size_t p;
            while ((p = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, p);
                buf.erase(0, p + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) self.sink_.Enqueue(line);
            }
        }
        {
            std::lock_guard<std::mutex> lk(self.clisMx_);
            self.clis_.erase(std::remove(self.clis_.begin(), self.clis_.end(), c), self.clis_.end());
        }
        ::close(c);
    }

    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::thread th_;
    std::mutex clisMx_;
    std::vector<int> clis_;
    StdinCommandSource& sink_;
};

} // namespace adapters
