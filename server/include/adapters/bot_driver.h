// bot_driver.h —— ep07 人机驱动适配器（整洁架构接口适配器·与 match_wiring 平级的"另一个输入源"）
// 铁律（ep07 设计草案 §3.1）：人机的移动与开火必须经与真人完全相同的入口
// （MatchSession::UpdateEntity / MatchSession::Fire）——不得直写 Entity、不得绕四道门；
// 人机超速照样被钳制 → ep09"人机不作弊"是结构性保证。
// 并发模型：主线程单线程驱动（main 33ms tick 统一调 Tick）——与业务串行铁律一致，零锁。
// 自愈（❗12）：Tick 内自检——房间被解散（真人数归零）→ AbortMatch + 人机清场。
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "application/config_views.h"   // v1.6 G2：IBotTuning 视图（bot 段热更即时生效）
#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/bot_brain.h"
#include "match/match_session.h"
#include "match/match_shards.h"
#include "match/nav_grid.h"
#include "match/policies.h"              // v1.6 收口②：IBotPolicy（决策策略端口）
#include "match/perception.h"

namespace adapters {

/** 移动目标出界钳制（2026-11-04 用户局实锤 Bot2 撤退到 (3177,−3918)——走出烘焙地板上千米）：
 *  FSM 四态的 moveTarget 都可能落在栅格外（TakeCover 撤退方向无界 / Engage 追击出界敌 /
 *  Search 的 lastKnown 在界外）——统一在适配器钳回活动区（kPlayAreaHalfUu=域常量单源）。
 *  分工：本函数只管"FSM 意图目标不越图"（寻路输入卫生）；权威位边界执法在
 *  MatchSession::UpdateEntity（域规则·对全体实体含真人生效）——两层不互替。 */
inline sightline::match::Vec2 ClampMoveTarget(const sightline::match::NavGrid& g,
                                              sightline::match::Vec2 p) {
    // 钳到栅格界（=地板+建格余量，随地图推导）——FSM 意图目标不越过可寻路范围；
    // 权威位边界执法在域层 UpdateEntity（活动区，对全体实体生效）——两层不互替。
    const float maxX = g.originX + static_cast<float>(g.w) * g.cellCm;
    const float maxY = g.originY + static_cast<float>(g.h) * g.cellCm;
    return {std::clamp(p.x, g.originX, maxX), std::clamp(p.y, g.originY, maxY)};
}

class BotDriver {
public:
    /** ep09 步骤 3：分桶化——byRoom_ → byShard_（每 shard 一张房表）——worker 只碰自己的桶
     *  （OnBotAdded/RemoveBot/Tick 全经 cmdQ/归属 worker；跨 worker 零共享=零竞态）。
     *  shardCount=1 即 Serial 单桶（行为与旧 byRoom_ 全等）。 */
    explicit BotDriver(std::size_t shardCount = 1)
        : byShard_(shardCount ? shardCount : 1),
          waypointsByShard_(byShard_.size()),
          botPolicyByShard_(byShard_.size(), nullptr) {}

    /** 26 号：席位登记（FSM 初始态；进对局前的房内待命） */
    void OnBotAdded(std::uint32_t roomId, std::uint32_t pid, const std::string& name,
                    std::uint8_t difficulty) {
        byShard_[roomId % byShard_.size()][roomId].push_back(Bot{pid, name, difficulty});
        LOG_INFO("[人机] 席位登记 房=" + std::to_string(roomId) + " pid=" + std::to_string(pid) +
                 " 名=" + name + " 难度=" + std::to_string(difficulty));
    }

    /** 27 号：移除席位 */
    void RemoveBot(std::uint32_t roomId, std::uint32_t pid) {
        auto& byRoom = byShardAt_(roomId);
        auto rit = byRoom.find(roomId);
        if (rit == byRoom.end()) return;
        auto& list = rit->second;
        for (auto it = list.begin(); it != list.end(); ++it)
            if (it->pid == pid) { list.erase(it); break; }
        if (list.empty()) byRoom.erase(roomId);
    }

    bool OwnsBot(std::uint32_t pid) const {
        for (const auto& byRoom : byShard_) for (const auto& [_, list] : byRoom)
            for (const auto& b : list)
                if (b.pid == pid) return true;
        return false;
    }

    /** 开局（14 号处理后调用）：建导航栅格 + 人机立即首报出生点（防 (0,0,0) 幻影·❗2）。
     *  出生点序读用例在 StartMatch 分配的 Entity::spawnIx（名册序=出生点序的唯一真相源）——适配层
     *  不重推规则：2026-11-03 联调实锤，14 号处理器的名册是值传递副本（spawnIndex 全 0），按它取点
     *  = 所有人机叠在房主出生点（客户端胶囊互挤+零距互射） */
    void OnMatchStart(std::uint32_t roomId, const sightline::match::MapGeometry& geo,
                      sightline::match::MatchSession& match, std::uint64_t nowMs) {
        auto it = byShardAt_(roomId).find(roomId);
        if (it == byShardAt_(roomId).end()) return;
        // nav：Prebuild 优先（main 装配期）——未 Prebuild（测试/兜底路径）惰性构建（布尔竞态=幂等双建无害）
        if (!navBuilt_) {
            nav_ = sightline::match::BuildNavGrid(geo.occluders);
            navBuilt_ = true;
        }
        // 航点 per-shard 构建（归属线程单写者——测试无 Prebuild 时兜底；生产=Prebuild 已填同内容）
        auto& waypoints = waypointsByShard_[roomId % waypointsByShard_.size()];
        waypoints.clear();
        if (!geo.patrolPoints.empty()) {
            for (const auto& pt : geo.patrolPoints) waypoints.push_back({pt.x, pt.y});
        } else if (!geo.spawnPoints.empty()) {
            for (const auto& sp : geo.spawnPoints) waypoints.push_back({sp.x, sp.y});
        } else {
            waypoints = {{300.f, -300.f}, {-300.f, 300.f}, {300.f, 300.f}, {-300.f, -300.f}};
        }
        if (!geo.spawnPoints.empty() && match.Entities().size() > geo.spawnPoints.size())
            LOG_WARNING("[人机] 出生点不足：本局 " + std::to_string(match.Entities().size()) +
                        " 人 > 地图出生点 " + std::to_string(geo.spawnPoints.size()) +
                        " 个——超出者叠在末位出生点（关卡补 PlayerStart Spawn_N 后重烘焙）");
        for (auto& b : it->second) {
            const sightline::match::Entity* E = match.EntityById(b.pid);
            if (!E) continue;                                // 不在本局名册：不首报
            b.homeSpawnIx = E->spawnIx;
            b.st = sightline::match::BotState::Patrol;
            b.mem = sightline::match::BotMemory{};
            b.path.reset();
            b.pathI = 0;
            b.wp = b.pid % waypoints.size();                 // 航点错开（相邻 pid 走不同航点）
            b.seq = 0;
            b.lastFireReqMs = 0;
            b.lastTickMs = nowMs;
            b.hp = sightline::match::kMaxHp;
            b.wasDead = false;                               // 上一局死亡沿不跨局
            // 首报位=实体权威锚（StartMatch 已做活动区钳制且 spawnX 同步——单一真相源，不再直读
            // 合同表：合同出生点在界外时锚已被钳入，双源必漂移=2026-11-04 03:20 局数据冲突教训）
            const float sx = E->spawnX, sy = E->spawnY, syaw = E->yaw;
            const float sz = E->spawnZ != 0.f ? E->spawnZ : 90.f;   // 无合同兜底（旧路径 z=90）
            match.UpdateEntity(b.pid, sx, sy, sz, syaw, 0.f, 0, ++b.seq, nowMs);
            b.x = sx; b.y = sy; b.z = sz; b.yaw = syaw;
            LOG_INFO("[人机] 首报出生点 pid=" + std::to_string(b.pid) + " 序=" +
                     std::to_string(E->spawnIx) + " @(" + std::to_string(sx) + "," + std::to_string(sy) + ")");
        }
    }

    /** 结束/中止：FSM 与记忆重置（席位保留——"再来一局"直接可用） */
    void OnMatchEnd(std::uint32_t roomId) {
        auto it = byShardAt_(roomId).find(roomId);
        if (it == byShardAt_(roomId).end()) return;
        for (auto& b : it->second) {
            b.st = sightline::match::BotState::Patrol;
            b.mem = sightline::match::BotMemory{};
            b.path.reset();
        }
    }

    void ClearRoom(std::uint32_t roomId) { byShardAt_(roomId).erase(roomId); }

    /** 主循环 33ms tick（唯一驱动点·主线程）：自愈检查 + 逐人机 感知→决策→行动。
     *  返回人机开火产出的领域事件（21 判定 / 23 击杀）——"同真人入口"不止同进还要同出：真人的 Fire
     *  事件经 FrameWriter 广播，人机的也必须走同一输出端口（2026-11-03 联调：此前返回值被丢——
     *  人机打人只扣血不广播，受害客户端不知道自己已死 → 服务器冻结尸体位、客户端照走 → 和解回拉） */
    /** ep09 多房化：match 单例参数 → MatchShards 容器——byRoom_ 本就按房组织，逐房 Find
     *  路由到各自 session 驱动（房内单线程语义由调用方保证：Serial=主线程顺序；worker 版=归属 worker）。
     *  终局静默从「全局 break」（单房）改「本房 continue」（多房——其它房的人机照跑）。 */
    std::vector<sightline::match::MatchEvent> Tick(std::uint64_t nowMs, sightline::match::MatchShards& shards,
                                                   sightline::lobby::LobbyService& lobby) {
        std::vector<sightline::match::MatchEvent> out;
        // 自愈（❗12）：房间已解散（最后真人离开）→ 清场+中止本房对局（Find 到才 abort——
        // 多房下 Find(roomId) 即"本房局"，不存在单例时代的跨房误杀问题）
        for (auto& byRoom : byShard_)
        for (auto it = byRoom.begin(); it != byRoom.end();) {
            if (!lobby.HasRoom(it->first)) {
                auto* s = shards.Find(it->first);
                if (s) s->AbortMatch();
                LOG_INFO("[人机] 房间已解散 → " + std::string(s ? "AbortMatch + " : "") +
                         "清场 房=" + std::to_string(it->first));
                it = byRoom.erase(it);
            } else {
                ++it;
            }
        }
        if (!navBuilt_ || waypointsByShard_.empty()) return out;
        for (auto& byRoom : byShard_)
        for (auto& [roomId, list] : byRoom) {
            auto* session = shards.Find(roomId);
            if (!session || session->IsEnded()) continue;    // 未开局/终局：本房 bot 静默（快照保留期）
            for (auto& b : list) TickOne(b, nowMs, *session, waypointsByShard_[roomId % waypointsByShard_.size()], botPolicyByShard_[roomId % botPolicyByShard_.size()], out);
        }
        return out;
    }

    // v1.6 收口②/G2：组合根注入——配置视图（bot 段热更即时）与决策策略（步骤 4 换 Lua 实现同口）。
    void SetBotTuning(const sightline::application::IBotTuning* t) { tuning_ = t; }
    /** ep09 步骤 4：per-shard 决策策略（每 shard 独立 VM 的 bot_ai——lua_State 非线程安全解） */
    void SetShardBotPolicy(std::size_t idx, sightline::match::IBotPolicy* p) {
        if (idx < botPolicyByShard_.size()) botPolicyByShard_[idx] = p;
    }
    void SetBotPolicy(sightline::match::IBotPolicy* p) {          // 兼容：全槽同策略（builtin）
        for (auto& slot : botPolicyByShard_) slot = p;
    }

private:
    struct Bot {
        std::uint32_t pid = 0;
        std::string name;
        std::uint8_t diff = 1;
        sightline::match::BotState st = sightline::match::BotState::Patrol;
        sightline::match::BotMemory mem;
        std::size_t wp = 0;                                  // 航点游标（航点表=驱动器级 waypoints_·每局重建）
        std::optional<std::vector<sightline::match::Vec3>> path;
        std::size_t pathI = 0;
        std::uint64_t lastRepathMs = 0;
        std::uint64_t lastTickMs = 0;
        std::uint32_t seq = 0;
        std::uint64_t lastFireReqMs = 0;                 // 上次扣扳机时刻（fireIntervalMs 节奏门控）
        std::uint8_t homeSpawnIx = 0;
        float x = 0.f, y = 0.f, z = 90.f, yaw = 0.f;
        float hp = 100.f;
        bool wasDead = false;
    };

    // v1.6 注入位：配置视图（G2·中档基准热更）+ 决策策略（收口②·步骤 4 换 Lua 同口）。
    // 组合根注入，进程期存活；nullptr=内建路径（测试/未注入零变化）。
    const sightline::application::IBotTuning* tuning_ = nullptr;


    // G2（v1.6）：中档（默认 diff=1）=配置表 bot 段（IBotTuning 视图·热更即时生效）；
    // 0/2 档保留内置绝对档位（难度档位进表=后续演进，ep08 范围外）。
    sightline::match::BotTuning TuneFor(std::uint8_t diff) const {
        sightline::match::BotTuning t;
        if (diff == 0) {                                     // 易：反应慢·误差大·枪稀
            t.reactMs = 400.f; t.errStartDeg = 6.f; t.errEndDeg = 3.f; t.errDecayDegPerS = 1.5f;
            t.fireIntervalMs = 600.f;
        } else if (diff >= 2) {                              // 难：反应快·误差小·枪密
            t.reactMs = 120.f; t.errStartDeg = 2.f; t.errEndDeg = 0.5f; t.errDecayDegPerS = 5.f;
            t.fireIntervalMs = 250.f;
        } else {
            return tuning_ ? tuning_->Get() : sightline::match::BotTuning{};   // 中=配置表
        }
        return t;
    }

    void TickOne(Bot& b, std::uint64_t nowMs, sightline::match::MatchSession& match,
                 const std::vector<sightline::match::Vec2>& waypoints,
                 sightline::match::IBotPolicy* botPolicy,
                 std::vector<sightline::match::MatchEvent>& out) {
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
        // 巡逻目标=驱动器航点表（此前读的是从未填充的每人机空表 → patrolTarget 落默认 (0,0)：人机全奔场心扎堆）
        if (!waypoints.empty()) w.patrolTarget = waypoints[b.wp % waypoints.size()];

        // ── 行动·移动（栅格寻路兜底：LOS 直行 / 否则 A* 路径逐段走）──
        // 收口②：决策走策略端口（未注入=内建 Decide 原路径——238 断言零变化；注入后
        // Builtin=Sanitize(Decide)，步骤 4 换 Lua 实现同口）
        sightline::match::BotIntent intent = botPolicy
            ? botPolicy->Decide(w, b.st, b.mem, nowMs, t)
            : sightline::match::Decide(w, b.st, b.mem, nowMs, t);   // 非常量：moveTarget 出界钳制就地改写
        if (intent.hasMove) {
            intent.moveTarget = ClampMoveTarget(nav_, intent.moveTarget);   // 意图目标钳制（见函数注释；权威边界在域层 UpdateEntity）
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
            if (!waypoints.empty()) {
                const auto& wp = waypoints[b.wp % waypoints.size()];
                const float dx = b.x - wp.x, dy = b.y - wp.y;
                if (dx * dx + dy * dy < 60.f * 60.f) ++b.wp;
            }
        }

        // ── 行动·转身（转身上限——禁甩头）──
        const float maxTurn = t.turnRateDegS * dtMs / 1000.f;
        const float want = intent.aimYawDeg - b.yaw;
        b.yaw += std::clamp(sightline::match::detail::NormDeg(want), -maxTurn, maxTurn);

        // ── 行动·开火（同真人入口：Fire 内四道门说了算；产出事件回传主循环走同一输出端口）──
        // 节奏门控（用户令：射击频率降低）：意图每拍都有，扣扳机按 fireIntervalMs 间隔——
        // 中难度 400ms=2.5 发/s（原为射速门允许的 ~10 发/s 连压）
        if (intent.bFire && nowMs - b.lastFireReqMs >= t.fireIntervalMs) {
            b.lastFireReqMs = nowMs;
            for (auto& ev : match.Fire(b.pid, nowMs)) {
                // 被武器门拦下的射击（射速/换弹）：真人路径同样不广播（D5-03 静默丢）——人机 30Hz 决策
                // 每拍都请求，拒绝事件回传只会刷"[射击拒绝]"日志，就地丢弃
                if (ev.type == sightline::match::MatchEvent::Type::FireResult && !ev.fire.bAccepted) continue;
                out.push_back(std::move(ev));
            }
        }

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

    // ===== ep09 步骤 3：per-shard 分桶（worker 版只碰自己的桶——byShard_[idx] 单写者）=====
    std::vector<std::map<std::uint32_t, std::vector<Bot>>> byShard_;
    std::map<std::uint32_t, std::vector<Bot>>& byShardAt_(std::uint32_t roomId) {
        return byShard_[roomId % byShard_.size()];
    }

public:
    /** worker 版 per-shard 驱动（E3 直调·事件入调用方 out——PublishToMerge 由 worker loop 做）。
     *  只遍历 byShard_[idx]——跨 worker 零共享。自愈同样只管本桶（别的桶别的 worker 自愈）。 */
    void TickShard(std::uint64_t nowMs, sightline::match::MatchShards& shards,
                   sightline::lobby::LobbyService& lobby,
                   std::size_t idx, std::vector<sightline::match::MatchEvent>& out) {
        if (idx >= byShard_.size()) return;
        auto& byRoom = byShard_[idx];
        for (auto it = byRoom.begin(); it != byRoom.end();) {
            if (!lobby.HasRoom(it->first)) {
                auto* s = shards.Find(it->first);
                if (s) s->AbortMatch();
                LOG_INFO("[人机] 房间已解散 → " + std::string(s ? "AbortMatch + " : "") +
                         "清场 房=" + std::to_string(it->first));
                it = byRoom.erase(it);
            } else {
                ++it;
            }
        }
        if (!navBuilt_ || waypointsByShard_.empty()) return;
        for (auto& [roomId, list] : byRoom) {
            auto* session = shards.Find(roomId);
            if (!session || session->IsEnded()) continue;    // 未开局/终局：本房 bot 静默
            for (auto& b : list) TickOne(b, nowMs, *session, waypointsByShard_[roomId % waypointsByShard_.size()], botPolicyByShard_[roomId % botPolicyByShard_.size()], out);
        }
    }

    /** 启动期预构建（G6 竞态根除：nav/waypoints 只读共享——写只发生在 main 装配期、worker 起线程前） */
    void PrebuildNav(const sightline::match::MapGeometry& geo) {
        nav_ = sightline::match::BuildNavGrid(geo.occluders);
        navBuilt_ = true;
        waypointsByShard_.assign(byShard_.size(), {});
        for (auto& wps : waypointsByShard_) {
            if (!geo.patrolPoints.empty()) {
                for (const auto& pt : geo.patrolPoints) wps.push_back({pt.x, pt.y});
            } else if (!geo.spawnPoints.empty()) {
                for (const auto& sp : geo.spawnPoints) wps.push_back({sp.x, sp.y});
            } else {
                wps = {{300.f, -300.f}, {-300.f, 300.f}, {300.f, 300.f}, {-300.f, -300.f}};
            }
        }
    }
    sightline::match::NavGrid nav_;
    bool navBuilt_ = false;
    std::vector<std::vector<sightline::match::Vec2>> waypointsByShard_;   // per-shard（同 shard 单写者）
    std::vector<sightline::match::IBotPolicy*> botPolicyByShard_;   // per-shard（同 shard 单写者·声明序在 byShard_ 后——初始化列表依赖其 size）
};

} // namespace adapters
