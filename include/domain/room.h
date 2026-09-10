// domain/room.h —— 房间实体：一局对战的全部权威状态与规则
// 状态机：Waiting → Playing → Finished（Finished 房间复用回收时回到 Waiting）
// 本类是"服务器权威"的具象化：移动采纳、命中判定、伤害结算、胜负判定全部在此发生，
// 外层（application）只做编排与搬运，不碰规则。

#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include "domain/types.h"
#include "domain/player.h"
#include "domain/combat/hitscan.h"
#include "domain/combat/damage.h"

namespace sightline::domain {

enum class RoomState {
    Waiting,    // 等人，人齐即开
    Playing,    // 对战中
    Finished    // 已分胜负
};

struct RoomRules {
    int min_players = 2;
    int max_players = 4;
    int max_hp = 100;
    int damage_per_hit = 25;    // 4 枪击杀（含击杀节奏的最简数值）
    int kills_to_win = 3;
    float max_fire_range = 10000.f;   // 射程上限(客户端坐标系=UE厘米,100m;超出判 miss)
};

class Room {
public:
    Room() = default;
    explicit Room(RoomId id, const RoomRules& rules) : id_(id), rules_(rules) {}

    // ---- 生命周期 ----
    RoomId id() const { return id_; }
    RoomState state() const { return state_; }
    const RoomRules& rules() const { return rules_; }
    int playerCount() const { return static_cast<int>(players_.size()); }

    bool canJoin() const {
        return state_ == RoomState::Waiting && playerCount() < rules_.max_players;
    }

    // 返回分配到的出生点索引（用于出生点计算）
    bool addPlayer(const Player& p) {
        if (!canJoin()) return false;
        players_[p.id] = p;
        players_[p.id].position = spawnPoint(static_cast<int>(players_.size()) - 1);
        players_[p.id].resetHp(rules_.max_hp);
        return true;
    }

    // 移除玩家；若对战中人已不足，判对局结束（剩下的唯一玩家获胜）
    struct LeaveOutcome {
        bool was_playing = false;
        bool game_over = false;
        PlayerId winner = 0;    // game_over 时的胜者（可能为 0 = 平局）
    };
    LeaveOutcome removePlayer(PlayerId pid) {
        LeaveOutcome out;
        out.was_playing = (state_ == RoomState::Playing);
        players_.erase(pid);
        if (out.was_playing && playerCount() == 1 && state_ == RoomState::Playing) {
            out.game_over = true;
            out.winner = players_.begin()->first;
            finish();
        } else if (out.was_playing && playerCount() == 0) {
            out.game_over = true;   // 全走光，无胜者
            finish();
        }
        return out;
    }

    // 人齐开战；返回是否真的开始了
    bool tryStart() {
        if (state_ == RoomState::Waiting && playerCount() >= rules_.min_players) {
            state_ = RoomState::Playing;
            return true;
        }
        return false;
    }

    void finish() { state_ = RoomState::Finished; }

    // 重开一局：结算后复位状态并保留玩家（ kills/hp/位置全部重置），
    // 人够则立即再开战。业务缺口修复：此前 Finished 房间永久滞留，玩家只能断线重连
    bool rematch() {
        if (state_ != RoomState::Finished) return false;
        state_ = RoomState::Waiting;
        int index = 0;
        for (auto& [id, p] : players_) {
            p.kills = 0;
            p.resetHp(rules_.max_hp);
            p.alive = true;
            p.position = spawnPoint(index++);
        }
        return tryStart();
    }

    // ---- 查询 ----
    const Player* findPlayer(PlayerId pid) const {
        auto it = players_.find(pid);
        return it == players_.end() ? nullptr : &it->second;
    }
    Player* findPlayer(PlayerId pid) {
        auto it = players_.find(pid);
        return it == players_.end() ? nullptr : &it->second;
    }

    std::vector<PlayerId> playerIds() const {
        std::vector<PlayerId> ids;
        ids.reserve(players_.size());
        for (auto& [id, _] : players_) ids.push_back(id);
        return ids;
    }

    // 除某人外的其他玩家（广播移动时排除自己，省流量）
    std::vector<PlayerId> othersOf(PlayerId pid) const {
        std::vector<PlayerId> ids;
        for (auto& [id, _] : players_) if (id != pid) ids.push_back(id);
        return ids;
    }

    // ---- 移动（客户端上报 → 服务器采纳的简化权威模型）----
    bool applyMove(PlayerId pid, const Vec3& pos, float yaw) {
        Player* p = findPlayer(pid);
        if (!p || state_ != RoomState::Playing || !p->alive) return false;
        p->position = pos;    // MVP 不做速度校验；反外挂的速度/碰撞校验是它自然的进化位
        p->yaw = yaw;
        return true;
    }

    // ---- 开火（服务器权威 hitscan 判定）----
    struct FireResult {
        bool valid = false;         // 射手/状态是否合法
        bool hit = false;
        PlayerId victim = 0;
        int damage = 0;
        int victim_hp = 0;
        bool victim_dead = false;
        PlayerId killer = 0;
        bool game_over = false;
        PlayerId winner = 0;
        // 诊断字段（判定瞬间的几何快照,适配层记录用）
        Vec3 target_pos;            // 候选目标位置(2人房即对手;判定时服务器所见)
        int  target_count = 0;      // 候选数量
    };

    FireResult applyFire(PlayerId shooter_id, const Vec3& origin, const Vec3& dir) {
        FireResult r;
        Player* shooter = findPlayer(shooter_id);
        if (!shooter || state_ != RoomState::Playing || !shooter->alive) return r;
        r.valid = true;
        r.killer = shooter_id;

        // 把所有存活敌人抽象为 AABB 候选
        std::vector<combat::TargetBox> boxes;
        for (auto& [pid, p] : players_) {
            if (pid == shooter_id || !p.alive) continue;
            boxes.push_back({pid, p.position, combat::TargetBox{}.half});
        }
        r.target_count = static_cast<int>(boxes.size());
        if (!boxes.empty()) r.target_pos = boxes[0].center;

        combat::RayHit hit = combat::pickNearestTarget(origin, dir, boxes);
        if (!hit.hit || hit.distance > rules_.max_fire_range) return r;   // miss

        Player* victim = findPlayer(hit.target);
        if (!victim) return r;

        r.hit = true;
        r.damage = rules_.damage_per_hit;
        auto outcome = combat::applyDamage(victim->hp, r.damage);
        victim->hp = outcome.new_hp;
        r.victim = hit.target;
        r.victim_hp = outcome.new_hp;
        r.victim_dead = outcome.dead;

        if (outcome.dead) {
            victim->alive = false;
            shooter->kills += 1;
            if (shooter->kills >= rules_.kills_to_win) {
                r.game_over = true;
                r.winner = shooter_id;
                finish();
            }
        }
        return r;
    }

    // ---- 重生（倒计时到点后由 application 调用）----
    bool respawn(PlayerId pid) {
        Player* p = findPlayer(pid);
        if (!p || state_ != RoomState::Playing) return false;
        p->resetHp(rules_.max_hp);
        p->position = spawnPoint(static_cast<int>(pid) % 8);   // 简单散开的出生点
        return true;
    }

    // 出生点：围绕场地一圈均匀分布（MVP 场地 40x40）
    Vec3 spawnPoint(int index) const {
        static const float radius = 18.f;
        float angle = (index % 8) * (3.14159265f * 2.f / 8.f);
        return {radius * std::cos(angle), 0.f, radius * std::sin(angle)};
    }

private:
    RoomId id_ = 0;
    RoomState state_ = RoomState::Waiting;
    RoomRules rules_;
    std::unordered_map<PlayerId, Player> players_;
};

} // namespace sightline::domain
