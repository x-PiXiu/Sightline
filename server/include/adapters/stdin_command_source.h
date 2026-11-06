// stdin_command_source.h —— ep08 步骤 3：命令输入适配器（A1：stdin 线程只入队）
// 编排器每帧开头 Drain() 消费，Lua / 配置永远只被主线程碰，单线程语义保住。
// 生命周期：线程捕获 shared_ptr<State>，析构置 stop 后 detach——关服时不会被阻塞的 getline 挂住。
#pragma once
#include <atomic>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "application/ports.h"

namespace adapters {

class StdinCommandSource final : public sightline::application::ICommandSource {
public:
    using Command = sightline::application::ConsoleCommand;

    StdinCommandSource() : st_(std::make_shared<State>()) {
        auto st = st_;
        th_ = std::thread([st] {
            std::string line;
            while (!st->stop.load() && std::getline(std::cin, line)) {
                if (auto c = Parse(line)) {
                    std::lock_guard<std::mutex> lk(st->mx);
                    st->q.push_back(*c);
                }
            }
        });
    }

    ~StdinCommandSource() override {
        st_->stop.store(true);
        if (th_.joinable()) th_.detach();
    }

    StdinCommandSource(const StdinCommandSource&) = delete;
    StdinCommandSource& operator=(const StdinCommandSource&) = delete;

    std::vector<Command> Drain() override {
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
        std::deque<Command> q;
        std::atomic<bool> stop{false};
    };
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
    std::thread th_;
};

} // namespace adapters
