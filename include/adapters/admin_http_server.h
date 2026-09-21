// admin_http_server.h —— GM 管理面板 HTTP 服务（服务端 03 文档）
//
// 独立阻塞线程：accept → 读 HTTP 请求 → 极简解析 → 回调 handler → 写响应 → 关闭。
// 不复用游戏 Reactor：管理请求低频且不可信（解析崩了不能连累游戏 EventLoop）；
// 单线程串行处理足够（运营面板就一两个浏览器在用）。
//
// 线程契约：handler 在 HTTP 线程被调用；凡触碰游戏状态的路由必须经
// AdminApi 的 queueInLoop 跳回主 loop（整洁架构：适配层自理，不外泄锁）。

#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <cstdlib>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace sightline::adapters {

struct HttpRequest
{
    std::string method;        // "GET" / "POST"
    std::string path;          // "/api/stats"（不含 query string）
    std::string body;          // POST body（GET 为空）
    std::string authorization; // Authorization 头原值（"Bearer xxx"），无则空
};

struct HttpResponse
{
    int         status       = 200;
    std::string content_type = "application/json";
    std::string body;
};

class AdminHttpServer
{
public:
    using Handler = std::function<HttpResponse(const HttpRequest&)>;

    void start(int port, Handler handler)
    {
        handler_ = std::move(handler);
        port_    = port;
        running_ = true;
        worker_  = std::thread([this] { run(); });
    }

    void stop()
    {
        running_ = false;
        // shutdown 才能唤醒阻塞在 accept() 的 worker——close() 对进行中的 accept 无效，
        // worker 会永远挂在 inet_csk_accept，主线程 join 死等（进程退不干净的元凶）
        if (listen_fd_ >= 0) { ::shutdown(listen_fd_, SHUT_RDWR); }
        if (worker_.joinable()) worker_.join();          // worker 若在 recv() 中，最多等 5s 超时
        if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    }

private:
    void run()
    {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port        = htons(static_cast<uint16_t>(port_));
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return;
        if (::listen(listen_fd_, 5) != 0) return;

        while (running_)
        {
            int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) continue;
            // 收发超时：半开连接（TCP 建立后不发数据/不收响应）不许挂死 worker——
            // 否则 stop() 的 join 永远等不到，进程退不干净
            timeval tv{5, 0};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            handleClient(client);          // 串行处理：完一个再收下一个
            ::close(client);
        }
    }

    void handleClient(int fd)
    {
        // ---- 读请求头：直到 \r\n\r\n 或上限（管理请求都小，8KB 封顶防恶意撑内存）----
        std::string raw;
        char buf[4096];
        size_t header_end = std::string::npos;
        while (raw.size() < 8192)
        {
            int n = static_cast<int>(::recv(fd, buf, sizeof(buf), 0));
            if (n <= 0) return;
            raw.append(buf, n);
            header_end = raw.find("\r\n\r\n");
            if (header_end != std::string::npos) break;
        }
        if (header_end == std::string::npos) return;

        // ---- 解析请求行：METHOD SP PATH SP VERSION ----
        const std::string line = raw.substr(0, raw.find("\r\n"));
        const size_t sp1 = line.find(' ');
        const size_t sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        HttpRequest req;
        req.method = line.substr(0, sp1);
        req.path   = line.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t q = req.path.find('?');
        if (q != std::string::npos) req.path.resize(q);   // 面板不用 query，剥掉防注入路由

        // ---- 头部字段：只关心 Content-Length / Authorization（大小写不敏感找值）----
        int content_length = 0;
        size_t pos = 0;
        while ((pos = raw.find("\r\n", pos)) != std::string::npos && pos + 2 < header_end)
        {
            const size_t key_begin = pos + 2;
            const size_t colon     = raw.find(':', key_begin);
            if (colon == std::string::npos || colon > header_end) break;
            const std::string key = lower(raw.substr(key_begin, colon - key_begin));
            size_t val_begin = colon + 1;
            while (val_begin < header_end && raw[val_begin] == ' ') ++val_begin;
            const std::string val = raw.substr(val_begin, raw.find("\r\n", val_begin) - val_begin);
            if (key == "content-length")  content_length = std::atoi(val.c_str());
            if (key == "authorization")   req.authorization = val;
            pos = val_begin;
        }
        if (content_length > 64 * 1024) return;   // body 上限：管理 API 没有大载荷

        // ---- 读 body（可能分片到达，补读齐 Content-Length）----
        if (req.method == "POST" && content_length > 0)
        {
            req.body = raw.substr(header_end + 4);
            while (req.body.size() < static_cast<size_t>(content_length))
            {
                int n = static_cast<int>(::recv(fd, buf, sizeof(buf), 0));
                if (n <= 0) break;
                req.body.append(buf, n);
            }
        }

        // ---- 执行 + 回写 ----
        HttpResponse resp;
        try { resp = handler_(req); }
        catch (...) { resp = {500, "application/json", "{\"error\":\"internal\"}"}; }
        const char* status_text = resp.status == 200 ? "OK"
                                : resp.status == 401 ? "Unauthorized"
                                : resp.status == 404 ? "Not Found" : "Error";
        const std::string out =
            "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text + "\r\n"
            "Content-Type: " + resp.content_type + "\r\n"
            "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + resp.body;
        ::send(fd, out.c_str(), static_cast<int>(out.size()), 0);
    }

    static std::string lower(std::string s)
    {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
        return s;
    }

    int               listen_fd_ = -1;
    std::thread       worker_;
    std::atomic<bool> running_{false};
    int               port_ = 8080;
    Handler           handler_;
};

} // namespace sightline::adapters
