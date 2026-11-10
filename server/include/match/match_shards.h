// match_shards.h —— ep09 多房分桶容器（用例层）：房→session 的唯一权威
// v1 = SerialDispatcher 形态（草案 §4.11）：单线程顺序 TickAll——Shard 结构/绑定哈希先立住，
//      步骤 3 的 worker 池换 ITickDispatcher 实现，本结构不变（可插拔的推迟决策）。
// 幂等（G2·Serial 简化形）：MatchSession::StartMatch 本就是全量复位（entities_.clear()+终局
//      三件套复位，2026-11-03 联调根因修复）——「不存在建 / 存在重置」与常驻单局时代行为零变化；
//      unique_ptr 替换形留给 worker 版（所有权不跨线程时才需要）。
#pragma once
#include "match/match_session.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sightline::match {

/** 开房共享依赖（构造注入·DIP）：session 创建时的只读共享件 + 初始配置闭包。
 *  geometry/damage 为 nullptr 时保持既有兼容路径（空表/内建策略——238 断言未注入行为）。 */
struct ShardDeps {
    const MapGeometry* geometry = nullptr;
    IDamagePolicy*     damage   = nullptr;
    std::function<GameConfig()> currentConfig;   // 新 session 初始规则快照（main 组装=ApplyConfig 同源）
    /** ep09 G1/G3：pid→(roomId,epoch) 快照查询（main 闭包=lobby.LocateFromSnapshot——
     *  用例层不见 LobbyService 类型，依赖经此端口）——Route 填路由 + Drain 校验在途判旧 */
    std::function<bool(std::uint32_t pid, std::uint32_t& roomId, std::uint32_t& epoch)> locate;
};

/** ep09 §4.4：跨线程 DTO（值语义·零指针零引用过界——ADR-9）。数据面/控制面二分（ADR-21）：
 *  对局输入走 dataQ FIFO 保序（RemovePlayer 不插队——防"退房后仍执行在途开火"，G1 配套）；
 *  生命周期控制走 cmdQ（先于数据面 drain）。roomId/epoch 由 Route 经 ShardDeps.locate 填——
 *  wiring 只投原始 pid（不查快照），路由与判旧统一在容器层。 */
struct MatchInput {
    enum class Kind : std::uint8_t { Move, Fire, Reload, SwitchWeapon, RemovePlayer, Rtt };
    Kind kind;
    std::uint32_t pid = 0;
    std::uint32_t roomId = 0;        // Route 填
    std::uint32_t epoch = 0;         // Route 填（drain 时对照快照判旧）
    std::uint64_t atMs = 0;          // 接收时刻（ADR-1 单钟·judgeAt 语义）
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    std::uint8_t flags = 0;
    std::uint32_t seq = 0;
    std::uint16_t rttMs = 0;
};

struct ShardCommand {
    enum class Kind : std::uint8_t { Patch };    // v1：StartMatch 直调（事件流归 wiring·V2 裁决）
    Kind kind;
    std::uint32_t roomId = 0;
    std::string source;                          // Patch 的 lua 源码（步骤 4）
};

class MatchShards {
public:
    // ===== Shard：桶=房容器+双邮箱（先声明——方法签名引用）=====
    struct Shard {
        std::unordered_map<std::uint32_t, std::unique_ptr<MatchSession>> rooms;
        // ep09 步骤 2：跨线程邮箱（草案 §4.4——Serial v1 主线程投递+同帧消费，结构先立）
        std::deque<MatchInput>   dataQ;              // 数据面 FIFO（Move newest-wins）
        std::deque<ShardCommand> cmdQ;               // 控制面（先于数据面 drain；v1 仅 Patch·步骤 4）
        std::mutex               qMutex;             // dataQ/cmdQ 共用一把（同临界区，无嵌套锁）
        std::atomic<std::uint64_t> dropCount{0};     // G8：入队/校验丢弃原子计数（横幅读数）
    };

    static constexpr std::size_t kDefaultShardCount = 4;   // P1 待批示默认值（worker 数=分桶数）

    explicit MatchShards(ShardDeps deps, std::size_t n = kDefaultShardCount)
        : deps_(std::move(deps)), shards_(n ? n : 1) {}

    /** 开房（确保存在+注入）：无则建（seed=roomId 派生·ADR-23 + Deps 注入），有则幂等返回。
     *  **不自动 StartMatch**——开局时机/名册/事件流归调用方（wiring 显式 StartMatch，
     *  与常驻单局时代语义完全同构；G2 幂等重置由 StartMatch 全量复位承担） */
    MatchSession& OpenRoom(std::uint32_t roomId) {
        auto& sh = shards_[roomId % shards_.size()];
        auto it = sh.rooms.find(roomId);
        if (it == sh.rooms.end()) {
            auto s = std::make_unique<MatchSession>(DeriveSeed(roomId));
            if (deps_.geometry) s->SetGeometry(*deps_.geometry);
            if (deps_.damage)   s->SetDamagePolicy(deps_.damage);
            if (deps_.currentConfig) s->SetConfig(deps_.currentConfig());
            it = sh.rooms.emplace(roomId, std::move(s)).first;
        }
        return *it->second;
    }

    /** 路由查（wiring/bot 用）——房间从未开局返回 nullptr */
    MatchSession* Find(std::uint32_t roomId) {
        auto& sh = shards_[roomId % shards_.size()];
        auto it = sh.rooms.find(roomId);
        return it == sh.rooms.end() ? nullptr : it->second.get();
    }

    /** 全 shard 顺序 Tick（Serial）——事件合流（每条自带 roomId，Fanout 按房分发零改动） */
    std::vector<MatchEvent> TickAll(std::uint64_t nowMs) {
        std::vector<MatchEvent> out;
        for (auto& sh : shards_)
            for (auto& [rid, s] : sh.rooms) {
                auto evs = s->Tick(nowMs);
                out.insert(out.end(), std::make_move_iterator(evs.begin()),
                           std::make_move_iterator(evs.end()));
            }
        return out;
    }

    /** 配置热重载传播（orchestrator ApplyConfig 调）——所有已建 session 逐个 SetConfig */
    void ApplyConfigAll(const GameConfig& c) {
        for (auto& sh : shards_)
            for (auto& [rid, s] : sh.rooms) s->SetConfig(c);
    }

    /** 启动期共享件注入（main 装配序：geometry 装载段/策略分支晚于容器声明——
     *  延迟到 setter；OpenRoom 全部发生在 wiring 运行期，晚于此两调，无竞态） */
    void SetSharedGeometry(const MapGeometry* g) { deps_.geometry = g; }
    void SetSharedDamage(IDamagePolicy* p) { deps_.damage = p; }

    // ===== ep09 步骤 2：输入路由与消费（Mailbox 主路径）=====
    static constexpr std::size_t kDataQCap = 4096;   // 背压上界（压测定档）

    /** 数据面投递：locate 解析路由（快照无此 pid=丢）→ 按 roomId 分 shard → dataQ。
     *  Move newest-wins：同 (pid,roomId) 的未消费 Move 被新值覆盖（丢旧不丢新·插值无感）。
     *  满则丢+计数（横幅可见）——返回 false=被丢（调用方日志）。 */
    bool Route(std::uint32_t pid, MatchInput in) {
        if (!deps_.locate) return false;
        if (!deps_.locate(pid, in.roomId, in.epoch)) return false;   // 不在房/人机外未知：在途判旧即丢
        in.pid = pid;
        auto& sh = shards_[in.roomId % shards_.size()];
        std::lock_guard lk(sh.qMutex);
        if constexpr (true) {
            if (in.kind == MatchInput::Kind::Move) {
                for (auto it = sh.dataQ.rbegin(); it != sh.dataQ.rend(); ++it)
                    if (it->kind == MatchInput::Kind::Move && it->pid == pid &&
                        it->roomId == in.roomId) { *it = std::move(in); return true; }   // newest-wins
            }
        }
        if (sh.dataQ.size() >= kDataQCap) { ++sh.dropCount; return false; }
        sh.dataQ.push_back(std::move(in));
        return true;
    }

    /** 控制面投递（cmdQ——先于数据面 drain；容量超限=系统异常级，横幅+计数） */
    bool RouteCommand(ShardCommand c) {
        auto& sh = shards_[c.roomId % shards_.size()];
        std::lock_guard lk(sh.qMutex);
        sh.cmdQ.push_back(std::move(c));
        return true;
    }

    /** 消费：cmdQ 先于 dataQ（控制面优先）→ epoch 在途判旧（对照最新快照——投递后快照变更即丢）
     *  → E8 契约：IsEnded 的 session 输入全丢 → 分派执行 → 事件流返回（并入 OnFrame 统一扇出）。
     *  Serial v1：主线程同帧投递+消费（队列瞬时存在）；worker 版本函数移入 worker loop。 */
    std::vector<MatchEvent> DrainAndApply(std::uint64_t nowMs) {
        std::vector<MatchEvent> out;
        for (auto& sh : shards_) {
            std::deque<MatchInput> inputs;
            std::deque<ShardCommand> cmds;
            {
                std::lock_guard lk(sh.qMutex);
                cmds.swap(sh.cmdQ);
                inputs.swap(sh.dataQ);
            }
            for (auto& c : cmds) applyCommand_(std::move(c), out);          // 控制面先于数据面
            for (auto& in : inputs) applyInput_(std::move(in), nowMs, out, sh);
        }
        return out;
    }

    /** G8：全 shard 丢弃计数聚合（横幅/审计读数——原子读，无锁） */
    std::uint64_t TotalDropped() const {
        std::uint64_t n = 0;
        for (const auto& sh : shards_) n += sh.dropCount.load(std::memory_order_relaxed);
        return n;
    }

    std::size_t ActiveRooms() const {
        std::size_t n = 0;
        for (const auto& sh : shards_) n += sh.rooms.size();
        return n;
    }

private:
    void applyCommand_(ShardCommand&& c, std::vector<MatchEvent>& out) {
        (void)c; (void)out;   // v1：cmdQ 仅 Patch 占位（步骤 4 接 LuaRuntime 聚合）——结构先立
    }

    void applyInput_(MatchInput&& in, std::uint64_t nowMs,
                     std::vector<MatchEvent>& out, Shard& sh) {
        // epoch 在途判旧（E5·drain 层对照快照——投递后快照变更即丢；Serial 同帧不变=恒过）
        if (deps_.locate) {
            std::uint32_t rid = 0, ep = 0;
            if (!deps_.locate(in.pid, rid, ep) || rid != in.roomId || ep != in.epoch) {
                ++sh.dropCount;
                return;
            }
        }
        auto* s = Find(in.roomId);
        if (!s) { ++sh.dropCount; return; }
        if (s->IsEnded()) { ++sh.dropCount; return; }   // E8：结算背景期输入全丢（快照继续·事件静默）
        switch (in.kind) {
        case MatchInput::Kind::Move: {
            auto evs = s->UpdateEntity(in.pid, in.x, in.y, in.z, in.yaw, in.pitch,
                                       in.flags, in.seq, in.atMs);
            out.insert(out.end(), std::make_move_iterator(evs.begin()),
                       std::make_move_iterator(evs.end()));
            break;
        }
        case MatchInput::Kind::Fire: {
            auto evs = s->Fire(in.pid, in.atMs);
            out.insert(out.end(), std::make_move_iterator(evs.begin()),
                       std::make_move_iterator(evs.end()));
            break;
        }
        case MatchInput::Kind::Reload:    (void)s->Reload(in.pid, in.atMs); break;
        case MatchInput::Kind::SwitchWeapon: (void)s->SwitchWeapon(in.pid, in.flags, in.atMs, nullptr); break;
        case MatchInput::Kind::RemovePlayer: {
            auto evs = s->RemovePlayer(in.pid);
            out.insert(out.end(), std::make_move_iterator(evs.begin()),
                       std::make_move_iterator(evs.end()));
            break;
        }
        case MatchInput::Kind::Rtt: s->SetShooterRtt(in.pid, in.rttMs); break;
        }
    }

private:
    /** ADR-23：seed=roomId 派生（乘法混淆防相邻房种子线性相关） */
    static std::uint32_t DeriveSeed(std::uint32_t roomId) {
        return roomId * 2654435761u ^ 0x9E3779B9u;
    }

    ShardDeps deps_;
    std::vector<Shard> shards_;
};

} // namespace sightline::match
