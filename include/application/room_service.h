// application/room_service.h —— 房间用例：匹配进房、开战、移动广播、
// 服务器权威开火判定、重生调度、胜负广播、离场处理。
// 规则全部委托 domain::Room，本类只做编排（取房间→调实体→翻译结果为事件→选收件人广播）。

#pragma once
#include <unordered_map>
#include <vector>
#include "application/dto.h"
#include "application/ports/i_game_channel.h"
#include "application/ports/i_timer_scheduler.h"
#include "domain/room.h"
#include "domain/player.h"

namespace sightline::app {

class RoomService {
public:
    struct Config {
        domain::RoomRules room_rules;    // 每间房的对战规则
        int respawn_delay_ms = 3000;     // 阵亡后重生倒计时
    };

    RoomService(IGameChannel& channel, ITimerScheduler& timers)
        : RoomService(channel, timers, Config()) {}

    RoomService(IGameChannel& channel, ITimerScheduler& timers, const Config& config)
        : channel_(channel), timers_(timers), config_(config) {}

    // ---- 进房/匹配：找一间能进的 WAITING 房，否则开新房；人齐自动开战 ----
    void handleJoin(PlayerId pid, const std::string& name) {
        domain::Room* room = findJoinableRoom();
        if (!room) {
            rooms_[next_room_id_] = domain::Room(next_room_id_, config_.room_rules);
            room = &rooms_[next_room_id_];
            ++next_room_id_;
        }
        domain::Player p;
        p.id = pid;
        p.name = name;
        if (!room->addPlayer(p)) return;      // 满员竞态，忽略（MVP 客户端可重试）

        player_room_[pid] = room->id();
        if (room->tryStart()) {
            channel_.sendToAll(room->playerIds(), RoomStartEvent{room->playerIds()});
        }
    }

    // ---- 移动：采纳 + 转播给房间其他人 ----
    void handleMove(PlayerId pid, const MoveCommand& cmd) {
        domain::Room* room = roomOf(pid);
        if (!room) return;
        if (!room->applyMove(pid, cmd.pos, cmd.yaw)) return;
        channel_.sendToAll(room->othersOf(pid),
                           MoveEvent{pid, cmd.pos, cmd.yaw});
    }

    // ---- 开火：domain 权威判定 → 结果广播；击杀触发重生/胜负调度 ----
    void handleFire(PlayerId pid, const FireCommand& cmd) {
        domain::Room* room = roomOf(pid);
        if (!room) return;
        auto r = room->applyFire(pid, cmd.origin, cmd.dir);
        if (!r.valid) return;

        // 命中与未中都广播（客户端用 miss 画弹道/弹孔）
        channel_.sendToAll(room->playerIds(),
            HitEvent{r.killer, r.victim, r.damage, r.victim_hp, r.victim_dead});

        if (r.victim_dead) {
            PlayerId victim = r.victim;
            scheduleRespawn(room->id(), victim);
        }
        if (r.game_over) {
            channel_.sendToAll(room->playerIds(), GameOverEvent{r.winner});
        }
    }

    // ---- 玩家彻底离开（断线/被踢），由 SessionService 回调进来 ----
    void handlePlayerGone(PlayerId pid) {
        domain::Room* room = roomOf(pid);
        player_room_.erase(pid);
        if (!room) return;
        auto out = room->removePlayer(pid);
        channel_.sendToAll(room->playerIds(), PlayerLeftEvent{pid});
        if (out.game_over) {
            channel_.sendToAll(room->playerIds(), GameOverEvent{out.winner});
        }
        if (room->playerCount() == 0) rooms_.erase(room->id());   // 空房即回收
    }

    // ---- 供查询 ----
    size_t roomCount() const { return rooms_.size(); }

private:
    domain::Room* roomOf(PlayerId pid) {
        auto it = player_room_.find(pid);
        if (it == player_room_.end()) return nullptr;
        auto rit = rooms_.find(it->second);
        return rit == rooms_.end() ? nullptr : &rit->second;
    }

    domain::Room* findJoinableRoom() {
        for (auto& [id, room] : rooms_) {
            if (room.canJoin()) return &room;
        }
        return nullptr;
    }

    void scheduleRespawn(domain::RoomId room_id, PlayerId pid) {
        timers_.runAfter(config_.respawn_delay_ms, [this, room_id, pid] {
            auto it = rooms_.find(room_id);
            if (it == rooms_.end()) return;
            auto& room = it->second;
            if (room.respawn(pid)) {
                const domain::Player* p = room.findPlayer(pid);
                channel_.sendToAll(room.playerIds(),
                                   RespawnEvent{pid, p->position, p->hp});
            }
        });
    }

    // 结算后的房间由"最后一个玩家离开"自然回收（handlePlayerGone 空房即删）

    IGameChannel& channel_;
    ITimerScheduler& timers_;
    Config config_;

    std::unordered_map<domain::RoomId, domain::Room> rooms_;
    std::unordered_map<PlayerId, domain::RoomId> player_room_;
    domain::RoomId next_room_id_ = 1;
};

} // namespace sightline::app
