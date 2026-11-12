// stdin_command_source.h —— ep08 步骤 3：命令输入适配器（A1：stdin 只入队，Drain 归主循环）
// 编排器每帧开头 Drain() 消费，Lua / 配置永远只被主线程碰，单线程语义保住。
// 生命周期（ep09 关服修复·2026-11-12 实测钉死）：**去线程化**——原 getline(stdin) 线程
//   detach 后活过宿主：阻塞读持有 cin 流锁，exit 期静态析构 cin 需同一把锁 → futex 死锁
//   → 进程挂死（对照实验：stdin=EOF 退 0 / stdin=阻塞挂死·wchan=futex_wait+pipe_read）→
//   外部关会话 SIGHUP → 退出码 129=128+1。修复：Drain() 内 poll(fd0,0)+::read 非阻塞收集
//   ——无线程即无锁竞争，exit 安全；stdin 读取这个"细节"归主循环驱动（A1 纪律的字面兑现）。
#pragma once
#include <poll.h>
#include <unistd.h>

#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "application/ports.h"

namespace adapters {

class StdinCommandSource final : public sightline::application::ICommandSource {
public:
    using Command = sightline::application::ConsoleCommand;

    StdinCommandSource() : st_(std::make_shared<State>()) {}

    // 多源入队口（ep08 步骤 9：TCP 命令口等）——A1 纪律：非主线程只入队，Drain 归主循环
    void Enqueue(const std::string& line) {
        auto st = st_;
        if (auto c = Parse(line)) {
            std::lock_guard<std::mutex> lk(st->mx);
            st->q.push_back(*c);
        }
    }

    ~StdinCommandSource() override = default;   // 无线程——exit 期无锁竞争（原 detach 方案的死锁根因已除）

    StdinCommandSource(const StdinCommandSource&) = delete;
    StdinCommandSource& operator=(const StdinCommandSource&) = delete;

    std::vector<Command> Drain() override {
        CollectStdin_();                        // 每帧非阻塞收集（poll 0 超时——主循环驱动）
        std::vector<Command> out;
        std::lock_guard<std::mutex> lk(st_->mx);
        while (!st_->q.empty()) { out.push_back(std::move(st_->q.front())); st_->q.pop_front(); }
        return out;
    }

    // 单测可复用：把一行文本解析为命令（reload [note] / rollback / version / help）
    static std::optional<Command> Parse(const std::string& raw) {
        std::string line = Trim(raw);
        if (line.empty()) return std::nullopt;
        std::string head = line;
        std::string rest;
        if (const auto sp = line.find_first_of(" \t"); sp != std::string::npos) {
            head = line.substr(0, sp);
            rest = Trim(line.substr(sp + 1));
        }
        if (head == "reload")   return Command{Command::Kind::Reload, Unquote(rest)};
        if (head == "rollback") return Command{Command::Kind::Rollback, {}};
        if (head == "version")  return Command{Command::Kind::Version, {}};
        if (head == "help")     return Command{Command::Kind::Help, {}};
        if (head == "patch")    return Command{Command::Kind::Patch, rest};   // patch <name>：脚本名，源码由运行时自读 scriptDir
        return Command{Command::Kind::Help, "未知命令: " + head};
    }

private:
    struct State {
        std::mutex mx;
        std::deque<Command> q;                 // TCP 线程 Enqueue 与主线程 Drain 的共享点（锁保护）
    };

    /** 每帧非阻塞收集 fd0（canonical 终端：整行回车后才 POLLIN；EOF/POLLHUP 置位后永不再查） */
    void CollectStdin_() {
        if (bEof_) return;
        pollfd pfd{0, POLLIN, 0};
        const int r = ::poll(&pfd, 1, 0);       // 0 超时=不阻塞主循环
        if (r <= 0) {
            if (r == 0) return;
            if (pfd.revents & (POLLHUP | POLLERR)) { FlushLine_(); bEof_ = true; }
            return;
        }
        char buf[512];
        const ssize_t n = ::read(0, buf, sizeof(buf));
        if (n <= 0) { FlushLine_(); bEof_ = true; return; }   // EOF/错误：尾行冲出后停止收集
        inBuf_.append(buf, static_cast<std::size_t>(n));
        std::size_t pos;
        while ((pos = inBuf_.find('\n')) != std::string::npos) {
            std::string line = inBuf_.substr(0, pos);
            inBuf_.erase(0, pos + 1);
            if (auto c = Parse(line)) {
                std::lock_guard<std::mutex> lk(st_->mx);
                st_->q.push_back(*c);
            }
        }
    }
    void FlushLine_() {                         // EOF 时无换行的尾行（重定向文件最后一行）
        if (inBuf_.empty()) return;
        if (auto c = Parse(inBuf_)) {
            std::lock_guard<std::mutex> lk(st_->mx);
            st_->q.push_back(*c);
        }
        inBuf_.clear();
    }
    static std::string Trim(std::string s) {
        const auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return {};
        const auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }
    static std::string Unquote(std::string s) {
        if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') ||
                              (s.front() == '\'' && s.back() == '\'')))
            return s.substr(1, s.size() - 2);
        return s;
    }

    std::shared_ptr<State> st_;
    std::string inBuf_;                         // 跨帧行缓冲（半行残留）
    bool bEof_ = false;                         // EOF 后永久停查（防每帧空读）
};

} // namespace adapters
