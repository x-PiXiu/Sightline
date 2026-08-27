// application/session_service.h —— 会话用例：登录身份、心跳保活（惰性扫描）、超时踢人
// 只依赖 domain + ports（IGameChannel / ITimerScheduler），可被 Fake 双测。
//
// 心跳设计（惰性扫描式）：每会话只记 last_active 时间戳，单个 runEvery 扫描器
// 周期巡检超时——替代早期"每消息 cancel+add 重排"（3 万连接档每秒数万次
// 锁+堆操作压主循环）。用低频操作实现低频语义。

#pragma once
#include <chrono>
#include <unordered_map>
#include <string>
#include <functional>
#include "application/dto.h"
#include "application/ports/i_game_channel.h"
#include "application/ports/i_timer_scheduler.h"
#include "domain/types.h"

namespace sightline::app {

class SessionService {
public:
    struct Config {
        int heartbeat_timeout_ms = 8000;   // 超过此时长无任何消息即踢出
        int scan_interval_ms = 1000;       // 心跳扫描周期（踢人精度粒度）
    };

    // on_player_gone：玩家彻底离开（断线或被踢）时回调，由 main 接到 RoomService
    SessionService(IGameChannel& channel, ITimerScheduler& timers,
                   std::function<void(PlayerId)> on_player_gone)
        : SessionService(channel, timers, std::move(on_player_gone), Config()) {}

    SessionService(IGameChannel& channel, ITimerScheduler& timers,
                   std::function<void(PlayerId)> on_player_gone,
                   const Config& config)
        : channel_(channel), timers_(timers),
          on_player_gone_(std::move(on_player_gone)), config_(config) {
        // 唯一的心跳定时器：周期扫描（替代每会话一个重排定时器）
        timers_.runEvery(config_.scan_interval_ms, [this] { scanHeartbeats(); });
    }

    // ---- 连接生命周期（由 adapters 的连接回调驱动）----

    void onConnected(uint64_t conn_id) {
        // 连接建立时还没有身份；等待 Login
    }

    void onDisconnected(uint64_t conn_id) {
        auto it = conn_to_player_.find(conn_id);
        if (it == conn_to_player_.end()) return;
        PlayerId pid = it->second;
        conn_to_player_.erase(it);
        dropPlayer(pid);
    }

    // ---- 命令处理 ----

    void handleLogin(uint64_t conn_id, const LoginCommand& cmd) {
        PlayerId pid = next_player_id_++;
        PlayerSession s;
        s.conn_id = conn_id;
        s.name = cmd.name.empty() ? ("Player" + std::to_string(pid)) : cmd.name;
        s.last_active = std::chrono::steady_clock::now();
        sessions_[pid] = s;
        conn_to_player_[conn_id] = pid;
        channel_.sendTo(pid, LoginAckEvent{pid});
    }

    void handlePing(PlayerId pid, const PingCommand& cmd) {
        if (!has(pid)) return;
        channel_.sendTo(pid, PongEvent{cmd.client_time});
    }

    // 任意消息都会喂狗：只打时间戳，零定时器操作
    void onActivity(PlayerId pid) {
        auto it = sessions_.find(pid);
        if (it != sessions_.end()) it->second.last_active = std::chrono::steady_clock::now();
    }

    // ---- 查询 ----
    bool has(PlayerId pid) const { return sessions_.count(pid) > 0; }
    const std::string& name(PlayerId pid) const {
        static const std::string kEmpty;
        auto it = sessions_.find(pid);
        return it == sessions_.end() ? kEmpty : it->second.name;
    }
    PlayerId playerIdOf(uint64_t conn_id) const {
        auto it = conn_to_player_.find(conn_id);
        return it == conn_to_player_.end() ? 0 : it->second;
    }
    // 反查：连接 id ←→ 玩家 id 的映射真源在本服务（适配层发送时反查）
    uint64_t connIdOf(PlayerId pid) const {
        auto it = sessions_.find(pid);
        return it == sessions_.end() ? 0 : it->second.conn_id;
    }

private:
    struct PlayerSession {
        uint64_t conn_id = 0;
        std::string name;
        std::chrono::steady_clock::time_point last_active;
    };

    // 单定时器巡检：超时会话 → 通告 + 断开（随后的 onDisconnected 完成清理与联动）
    void scanHeartbeats() {
        auto now = std::chrono::steady_clock::now();
        for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
            auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.last_active).count();
            if (idle < config_.heartbeat_timeout_ms) continue;
            channel_.sendTo(it->first, KickEvent{1});   // 1 = 心跳超时
            channel_.close(it->first);
        }
    }

    void dropPlayer(PlayerId pid) {
        auto it = sessions_.find(pid);
        if (it == sessions_.end()) return;
        sessions_.erase(it);
        if (on_player_gone_) on_player_gone_(pid);
    }

    IGameChannel& channel_;
    ITimerScheduler& timers_;
    std::function<void(PlayerId)> on_player_gone_;
    Config config_;

    std::unordered_map<PlayerId, PlayerSession> sessions_;
    std::unordered_map<uint64_t, PlayerId> conn_to_player_;
    PlayerId next_player_id_ = 1;
};

} // namespace sightline::app
