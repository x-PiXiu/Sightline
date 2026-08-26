// application/session_service.h —— 会话用例：登录身份、心跳保活、超时踢人
// 只依赖 domain + ports（IGameChannel / ITimerScheduler），可被 Fake 双测。

#pragma once
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
    };

    // on_player_gone：玩家彻底离开（断线或被踢）时回调，由 main 接到 RoomService
    SessionService(IGameChannel& channel, ITimerScheduler& timers,
                   std::function<void(PlayerId)> on_player_gone)
        : SessionService(channel, timers, std::move(on_player_gone), Config()) {}

    SessionService(IGameChannel& channel, ITimerScheduler& timers,
                   std::function<void(PlayerId)> on_player_gone,
                   const Config& config)
        : channel_(channel), timers_(timers),
          on_player_gone_(std::move(on_player_gone)), config_(config) {}

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
        sessions_[pid] = s;
        conn_to_player_[conn_id] = pid;
        scheduleHeartbeat(pid);
        channel_.sendTo(pid, LoginAckEvent{pid});
    }

    void handlePing(PlayerId pid, const PingCommand& cmd) {
        if (!has(pid)) return;
        channel_.sendTo(pid, PongEvent{cmd.client_time});
    }

    // 任意消息都会喂狗（GameServer 分发时统一调用）
    void onActivity(PlayerId pid) {
        if (!has(pid)) return;
        scheduleHeartbeat(pid);
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
        uint64_t heartbeat_timer = 0;
    };

    void scheduleHeartbeat(PlayerId pid) {
        auto it = sessions_.find(pid);
        if (it == sessions_.end()) return;
        if (it->second.heartbeat_timer) timers_.cancel(it->second.heartbeat_timer);
        it->second.heartbeat_timer = timers_.runAfter(config_.heartbeat_timeout_ms, [this, pid] {
            auto s = sessions_.find(pid);
            if (s == sessions_.end()) return;
            s->second.heartbeat_timer = 0;
            channel_.sendTo(pid, KickEvent{1});          // 1 = 心跳超时
            channel_.close(pid);                          // 触发 onClose → onDisconnected → dropPlayer
        });
    }

    void dropPlayer(PlayerId pid) {
        auto it = sessions_.find(pid);
        if (it == sessions_.end()) return;
        if (it->second.heartbeat_timer) timers_.cancel(it->second.heartbeat_timer);
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
