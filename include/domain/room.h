// domain/room.h —— 房间实体：一局对战的全部权威状态与规则
// 状态机：Waiting → Playing → Finished（Finished 房间复用回收时回到 Waiting）
// 本类是"服务器权威"的具象化：命中判定、移动采纳、伤害结算、胜负判定全部在此发生，
// 外层（application）只做编排与搬运，不碰规则。

#pragma once
#include <unordered_map>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <optional>
#include "domain/types.h"
#include "domain/player.h"
#include "domain/item.h"
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
    // ---- 道具规则（P2）----
    int item_heal_amount = 50;          // 血包回复量
    float item_pickup_radius = 150.f;   // 拾取半径（厘米，水平距离判定）
    double item_respawn_ms = 15000.0;   // 拾取后重生冷却（毫秒）
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
            started_at_ms_ = nowMs();   // D3 结算用：本局开战时刻
            return true;
        }
        return false;
    }

    /** 本局时长（秒，D3 结算采集用；从未开战 = 0） */
    int elapsedSec() const {
        return started_at_ms_ == 0.0
                   ? 0
                   : static_cast<int>((nowMs() - started_at_ms_) / 1000.0);
    }

    void finish() { state_ = RoomState::Finished; }

    // 重开一局：结算后复位状态并保留玩家（ kills/deaths/hp/位置全部重置），
    // 人够则立即再开战。业务缺口修复：此前 Finished 房间永久滞留，玩家只能断线重连
    bool rematch() {
        if (state_ != RoomState::Finished) return false;
        state_ = RoomState::Waiting;
        int index = 0;
        for (auto& [id, p] : players_) {
            p.kills = 0;
            p.deaths = 0;
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

    /** 结算快照：全员(账户,击杀,死亡)——MatchEnd 广播与落库的数据源 */
    struct ScoreRow { PlayerId pid; std::uint64_t account_id; int kills; int deaths; };
    std::vector<ScoreRow> scoreBoard() const {
        std::vector<ScoreRow> out;
        out.reserve(players_.size());
        for (const auto& [id, p] : players_)
            out.push_back({id, p.account_id, p.kills, p.deaths});
        return out;
    }
    std::uint64_t accountOf(PlayerId pid) const {
        auto it = players_.find(pid);
        return it != players_.end() ? it->second.account_id : 0;
    }

    // 除某人外的其他玩家（广播移动时排除自己，省流量）
    std::vector<PlayerId> othersOf(PlayerId pid) const {
        std::vector<PlayerId> ids;
        for (auto& [id, _] : players_) if (id != pid) ids.push_back(id);
        return ids;
    }

    // ---- 移动（客户端上报 → 服务器采纳的简化权威模型）----
    bool applyMove(PlayerId pid, const Vec3& pos, float yaw,
                   uint8_t flags = 0, float aim_pitch = 0.f) {
        Player* p = findPlayer(pid);
        if (!p || state_ != RoomState::Playing || !p->alive) return false;
        p->position = pos;    // MVP 不做速度校验；反外挂的速度/碰撞校验是它自然的进化位
        p->yaw = yaw;
        p->flags = flags;         // Phase1 动画同步状态（纯透传，不做判定）
        p->aim_pitch = aim_pitch;
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
        // ★ 中心 = 脚底位置 + 半高（p.position 是脚底，盒中心应在角色中部）
        std::vector<combat::TargetBox> boxes;
        for (auto& [pid, p] : players_) {
            if (pid == shooter_id || !p.alive) continue;
            Vec3 center = p.position;
            center.y += combat::TargetBox{}.half.y;   // 脚底 → 角色中部
            boxes.push_back({pid, center, combat::TargetBox{}.half});
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
            victim->deaths += 1;      // D3 战绩：被击杀数（结算面板/落库）
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

    // 出生点：围绕场地一圈均匀分布（场地约 40x40 米 = ±2000cm）
    Vec3 spawnPoint(int index) const {
        // radius=1800（18 米）——遗留修复：曾是 18（=18cm，重生挤在原点附近）
        static const float radius = 1800.f;
        float angle = (index % 8) * (3.14159265f * 2.f / 8.f);
        return {radius * std::cos(angle), 0.f, radius * std::sin(angle)};
    }

    // ---- 道具（P2）：服务端权威的布点/拾取/惰性重生 ----

    // 开局/重开时布点（幂等：清空旧道具重摆，NetID 重新计数）
    void spawnItems() {
        items_.clear();
        next_item_id_ = 1;
        auto add = [&](ItemTypeId t, float x, float z) {
            items_.push_back(Item{next_item_id_++, t, {x, 0.f, z}, false, 0.0});
        };
        // MVP 固定布点：十字对称——东西血包、南北弹药箱（对称即公平）
        add(ItemTypeId::HealthPack, -kItemFieldRadius_, 0.f);
        add(ItemTypeId::HealthPack,  kItemFieldRadius_, 0.f);
        add(ItemTypeId::AmmoBox,    0.f, -kItemFieldRadius_);
        add(ItemTypeId::AmmoBox,    0.f,  kItemFieldRadius_);
    }

    const std::vector<Item>& items() const { return items_; }

    // 惰性重生：冷却到点的道具重置为可拾取，返回重置列表（application 广播 ItemSpawn）
    // 由 applyMove 的调用方顺带触发——不需要独立定时器，也不需要 C2S_Pickup 消息
    std::vector<Item> tickItems(double now_ms) {
        std::vector<Item> respawned;
        for (auto& item : items_) {
            if (item.taken && now_ms - item.taken_at_ms >= rules_.item_respawn_ms) {
                item.taken = false;
                item.taken_at_ms = 0.0;
                respawned.push_back(item);
            }
        }
        return respawned;
    }

    // 拾取判定：水平距离 < 拾取半径 且未被拿走 → 标记 taken + 结算效果
    // 每次只捡一个（同帧踩到多个时取布点顺序靠前的，天然稀有）
    struct PickupResult {
        uint32_t net_id = 0;
        ItemTypeId type_id = ItemTypeId::HealthPack;
        PlayerId picker = 0;
        int picker_hp = 0;    // 拾取后血量（客户端据此回填权威值）
    };
    std::optional<PickupResult> tryPickup(PlayerId pid, const Vec3& pos, double now_ms) {
        if (state_ != RoomState::Playing) return std::nullopt;
        Player* p = findPlayer(pid);
        if (!p || !p->alive) return std::nullopt;

        const float r2 = rules_.item_pickup_radius * rules_.item_pickup_radius;
        for (auto& item : items_) {
            if (item.taken) continue;
            const float dx = pos.x - item.pos.x;
            const float dz = pos.z - item.pos.z;   // 只看水平距离（道具贴地，忽略身高差）
            if (dx * dx + dz * dz > r2) continue;

            item.taken = true;
            item.taken_at_ms = now_ms;
            if (item.type_id == ItemTypeId::HealthPack) {
                p->hp = std::min(rules_.max_hp, p->hp + rules_.item_heal_amount);
            }
            // AmmoBox：弹药是客户端本地演出（协议不校验），服务端只标记冷却
            return PickupResult{item.net_id, item.type_id, pid, p->hp};
        }
        return std::nullopt;
    }

private:
    RoomId id_ = 0;
    RoomState state_ = RoomState::Waiting;
    RoomRules rules_;
    std::unordered_map<PlayerId, Player> players_;

    // ---- 对局计时（D3 结算：本局时长）----
    double started_at_ms_ = 0.0;   // tryStart 时刻（单调钟毫秒）；0 = 从未开战

    static double nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // ---- 道具状态 ----
    static constexpr float kItemFieldRadius_ = 800.f;   // 布点半径：十字臂 8 米（出生圈 18 米之内）
    std::vector<Item> items_;
    uint32_t next_item_id_ = 1;
};

} // namespace sightline::domain
