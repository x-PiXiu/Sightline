// bot_driver.h —— ep07 人机驱动适配器（整洁架构接口适配器·与 match_wiring 平级的"另一个输入源"）
// 铁律（ep07 设计草案 §3.1）：人机的移动与开火必须经与真人完全相同的入口
// （MatchService::UpdateEntity / MatchService::Fire）——不得直写 Entity、不得绕四道门；
// 人机超速照样被钳制 → ep09"人机不作弊"是结构性保证。
// 并发模型：主线程单线程驱动（main 33ms tick 统一调 Tick）——与业务串行铁律一致，零锁。
// 自愈（❗12）：Tick 内自检——房间被解散（真人数归零）→ AbortMatch + 人机清场。
#pragma once
#include <cmath>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/bot_brain.h"
#include "match/match_service.h"
#include "match/nav_grid.h"
#include "match/perception.h"

namespace adapters {

class BotDriver {
public:
    /** 26 号：席位登记（FSM 初始态；进对局前的房内待命） */
    void OnBotAdded(std::uint32_t roomId, std::uint32_t pid, const std::string& name,
                    std::uint8_t difficulty) {
        byRoom_[roomId].push_back(Bot{pid, name, difficulty});
        LOG_INFO("[人机] 席位登记 房=" + std::to_string(roomId) + " pid=" + std::to_string(pid) +
                 " 名=" + name + " 难度=" + std::to_string(difficulty));
    }

    /** 27 号：移除席位 */
    void RemoveBot(std::uint32_t pid) {
        for (auto& [roomId, list] : byRoom_)
            for (std::size_t i = 0; i < list.size(); ++i)
                if (list[i].pid == pid) {
                    LOG_INFO("[人机] 席位移除 pid=" + std::to_string(pid));
                    list.erase(list.begin() + static_cast<long>(i));
                    return;
                }
    }

    bool OwnsBot(std::uint32_t pid) const {
        for (const auto& [_, list] : byRoom_)
            for (const auto& b : list)
                if (b.pid == pid) return true;
        return false;
    }

    /** 开局（14 号处理后调用）：建导航栅格 + 人机立即首报出生点（防 (0,0,0) 幻影·❗2） */
    void OnMatchStart(std::uint32_t roomId, const std::vector<sightline::match::RosterEntry>& roster,
                      const sightline::match::MapGeometry& geo, sightline::match::MatchService& match,
                      std::uint64_t nowMs) {
        auto it = byRoom_.find(roomId);
        if (it == byRoom_.end()) return;
        nav_ = sightline::match::BuildNavGrid(geo.occluders);
        navBuilt_ = true;
        // 巡逻航点：合同 patrolPoints 优先，缺省退化为出生点轮转
        for (const auto& r : roster) {
            for (auto& b : it->second) {
                if (b.pid != r.playerId) continue;
                b.homeSpawnIx = r.spawnIndex;
                b.st = sightline::match::BotState::Patrol;
                b.mem = sightline::match::BotMemory{};
                b.path.reset();
                b.wp = 0;
                b.seq = 0;
                b.lastTickMs = nowMs;
                b.hp = sightline::match::kMaxHp;
                // 立即首报：出生点 = 名册序 → geometry.spawnPoints[序]（同源 Spawn_{n+1}）
                float sx = 0.f, sy = 0.f, sz = 90.f, syaw = 0.f;
                if (!geo.spawnPoints.empty()) {
                    const auto& sp = geo.spawnPoints[std::min<std::size_t>(r.spawnIndex, geo.spawnPoints.size() - 1)];
                    sx = sp.x; sy = sp.y; sz = sp.z; syaw = sp.yawDeg;
                }
                match.UpdateEntity(b.pid, sx, sy, sz, syaw, 0.f, 0, ++b.seq, nowMs);
                b.x = sx; b.y = sy; b.z = sz; b.yaw = syaw;
                LOG_INFO("[人机] 首报出生点 pid=" + std::to_string(b.pid) + " @(" +
                         std::to_string(sx) + "," + std::to_string(sy) + ")");
            }
        }
        // 航点表：patrolPoints 优先，否则 spawnPoints 世界位轮转，再否则场心环
        if (!geo.patrolPoints.empty()) {
            for (const auto& p : geo.patrolPoints) waypoints_.push_back({p.x, p.y});
        } else if (!geo.spawnPoints.empty()) {
            for (const auto& s : geo.spawnPoints) waypoints_.push_back({s.x, s.y});
        } else {
            waypoints_ = {{300.f, -300.f}, {-300.f, 300.f}, {300.f, 300.f}, {-300.f, -300.f}};
        }
        for (auto& b : it->second) b.wp = b.pid % waypoints_.size();
    }

    /** 结束/中止：FSM 与记忆重置（席位保留——"再来一局"直接可用） */
    void OnMatchEnd(std::uint32_t roomId) {
        auto it = byRoom_.find(roomId);
        if (it == byRoom_.end()) return;
        for (auto& b : it->second) {
            b.st = sightline::match::BotState::Patrol;
            b.mem = sightline::match::BotMemory{};
            b.path.reset();
        }
    }

    void ClearRoom(std::uint32_t roomId) { byRoom_.erase(roomId); }

    /** 主循环 33ms tick（唯一驱动点·主线程）：自愈检查 + 逐人机 感知→决策→行动 */
    void Tick(std::uint64_t nowMs, sightline::match::MatchService& match,
              sightline::lobby::LobbyService& lobby) {
        // 自愈（❗12）：房间已解散（最后真人离开）→ 终止对局 + 清场（ lobby 已自行解散房间）
        for (auto it = byRoom_.begin(); it != byRoom_.end();) {
            if (!lobby.HasRoom(it->first)) {
                match.AbortMatch();
                LOG_INFO("[人机] 房间已解散 → AbortMatch + 清场 房=" + std::to_string(it->first));
                it = byRoom_.erase(it);
            } else {
                ++it;
            }
        }
        if (!navBuilt_) return;
        for (auto& [roomId, list] : byRoom_) {
            if (match.IsEnded()) break;                      // 终局：本帧人机全部静默（快照保留期）
            for (auto& b : list) TickOne(b, nowMs, match);
        }
    }

private:
    struct Bot {
        std::uint32_t pid = 0;
        std::string name;
        std::uint8_t diff = 1;
        sightline::match::BotState st = sightline::match::BotState::Patrol;
        sightline::match::BotMemory mem;
        std::vector<sightline::match::Vec2> waypoints;
        std::size_t wp = 0;
        std::optional<std::vector<sightline::match::Vec3>> path;
        std::size_t pathI = 0;
        std::uint64_t lastRepathMs = 0;
        std::uint64_t lastTickMs = 0;
        std::uint32_t seq = 0;
        std::uint8_t homeSpawnIx = 0;
        float x = 0.f, y = 0.f, z = 90.f, yaw = 0.f;
        float hp = 100.f;
        bool wasDead = false;
    };

    static sightline::match::BotTuning TuneFor(std::uint8_t diff) {
        sightline::match::BotTuning t;
        if (diff == 0) {                                     // 易：反应慢·误差大
            t.reactMs = 400.f; t.errStartDeg = 6.f; t.errEndDeg = 3.f; t.errDecayDegPerS = 1.5f;
        } else if (diff >= 2) {                              // 难：反应快·误差小
            t.reactMs = 120.f; t.errStartDeg = 2.f; t.errEndDeg = 0.5f; t.errDecayDegPerS = 5.f;
        }
        return t;
    }

    void TickOne(Bot& b, std::uint64_t nowMs, sightline::match::MatchService& match) {
        const sightline::match::Entity* E = match.EntityById(b.pid);
        if (!E || !E->bHasReport) { b.lastTickMs = nowMs; return; }   // 未在局/未激活：静默
        const float dtMs = b.lastTickMs ? std::min<float>(float(nowMs - b.lastTickMs), 100.f) : 33.f;
        b.lastTickMs = nowMs;
        // 同步权威位（服务器权威=唯一真相；驱动器只是"输入生成器"）
        b.x = E->x; b.y = E->y; b.z = E->z; b.yaw = E->yaw; b.hp = E->weapon.hp;
        // 死亡/重生沿：重生前人机静默；复活瞬回 Patrol
        if (E->bDead) { b.wasDead = true; return; }
        if (b.wasDead) { b.wasDead = false; b.st = sightline::match::BotState::Patrol; b.mem = {}; b.path.reset(); }
        if (match.IsEnded()) return;
        const sightline::match::BotTuning t = TuneFor(b.diff);

        // ── 感知：最近可见敌（视线=CastRay 同源；位置采样=now−reactMs·人类反应具象）──
        std::optional<sightline::match::SeenEnemy> visible;
        const auto sampleAt = nowMs - static_cast<std::uint64_t>(t.reactMs);
        for (const auto& O : match.Entities()) {
            if (O.playerId == b.pid || !O.bHasReport || O.bDead) continue;
            const float dx = O.x - b.x, dy = O.y - b.y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > 2200.f) continue;                     // 感知半径（硬上限·可视外不感知）
            const auto sampled = sightline::match::SampleHistoryAt(O.history, O.histCount, O.histHead,
                                                                   sampleAt);
            const float tx = sampled ? sampled->x : O.x, ty = sampled ? sampled->y : O.y;
            if (!sightline::match::CanSee(match.Geometry().occluders,
                                          sightline::match::Vec3{b.x, b.y, b.z}, b.yaw,
                                          sightline::match::Vec3{tx, ty, b.z + 60.f}))
                continue;                                    // 胸口高度视线判定（所见=所及）
            const float ax = tx - b.x, ay = ty - b.y;
            const float sd = sampled ? std::sqrt(ax * ax + ay * ay) : dist;
            if (!visible || sd < visible->dist)
                visible = sightline::match::SeenEnemy{O.playerId, tx, ty, sd};
        }

        // ── 决策（纯函数）：视图组装 ──
        sightline::match::BotWorldView w;
        w.self = {b.x, b.y, b.z, b.yaw, b.hp, E->weapon.magUsed,
                  nowMs < E->weapon.reloadUntilMs};
        w.visible = visible;
        if (b.mem.hasLastKnown) w.lastKnown = sightline::match::Vec3{b.mem.lastKnown.x, b.mem.lastKnown.y, 0.f};
        if (!b.waypoints.empty()) w.patrolTarget = b.waypoints[b.wp % b.waypoints.size()];
        const sightline::match::BotIntent intent =
            sightline::match::Decide(w, b.st, b.mem, nowMs, t);

        // ── 行动·移动（栅格寻路兜底：LOS 直行 / 否则 A* 路径逐段走）──
        if (intent.hasMove) {
            const bool direct = sightline::match::GridLOS(
                nav_, nav_.CellIx(b.x), nav_.CellIy(b.y), nav_.CellIx(intent.moveTarget.x),
                nav_.CellIy(intent.moveTarget.y));
            if (direct) {
                StepLinear(b, intent.moveTarget, dtMs, t);
            } else {
                const bool needRepath =
                    !b.path || b.pathI >= b.path->size() ||
                    nowMs - b.lastRepathMs > 800.f ||
                    std::fabs(b.path->back().x - intent.moveTarget.x) > 1.f ||
                    std::fabs(b.path->back().y - intent.moveTarget.y) > 1.f;
                if (needRepath) {
                    b.path = sightline::match::FindPath(nav_, b.x, b.y, intent.moveTarget.x,
                                                        intent.moveTarget.y);
                    b.pathI = 0;
                    b.lastRepathMs = nowMs;
                }
                if (b.path && b.pathI < b.path->size()) {
                    StepLinear(b, {(*b.path)[b.pathI].x, (*b.path)[b.pathI].y}, dtMs, t);
                    const float dx = b.x - (*b.path)[b.pathI].x, dy = b.y - (*b.path)[b.pathI].y;
                    if (dx * dx + dy * dy < 40.f * 40.f) ++b.pathI;
                }
            }
            // 航点推进（巡逻环）
            if (!b.waypoints.empty()) {
                const auto& wp = b.waypoints[b.wp % b.waypoints.size()];
                const float dx = b.x - wp.x, dy = b.y - wp.y;
                if (dx * dx + dy * dy < 60.f * 60.f) ++b.wp;
            }
        }

        // ── 行动·转身（转身上限——禁甩头）──
        const float maxTurn = t.turnRateDegS * dtMs / 1000.f;
        const float want = intent.aimYawDeg - b.yaw;
        b.yaw += std::clamp(sightline::match::detail::NormDeg(want), -maxTurn, maxTurn);

        // ── 行动·开火（同真人入口：Fire 内四道门说了算）──
        if (intent.bFire) match.Fire(b.pid, nowMs);

        // ── 上报（同 19 号入口：权威推算+钳制+历史环一条不落）──
        match.UpdateEntity(b.pid, b.x, b.y, b.z, b.yaw, 0.f, 0, ++b.seq, nowMs);
    }

    void StepLinear(Bot& b, const sightline::match::Vec2& to, float dtMs,
                    const sightline::match::BotTuning& t) {
        const float dx = to.x - b.x, dy = to.y - b.y;
        const float len = std::sqrt(dx * dx + dy * dy);
        if (len < 1e-3f) return;
        const float step = std::min(len, t.moveSpeedCmS * dtMs / 1000.f);
        b.x += dx / len * step;
        b.y += dy / len * step;
    }

    std::unordered_map<std::uint32_t, std::vector<Bot>> byRoom_;
    sightline::match::NavGrid nav_;
    bool navBuilt_ = false;
    std::vector<sightline::match::Vec2> waypoints_;
};

} // namespace adapters
