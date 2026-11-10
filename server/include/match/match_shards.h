// match_shards.h —— ep09 多房分桶容器（用例层）：房→session 的唯一权威
// v3（步骤 3 刀 A）：cmdQ 全控制面化（StartMatch/ApplyConfig/AddBot/RemoveBot）——
//     worker 下主线程零直写 shard 数据（OpenRoom/StartMatch/SetConfig/bot 登记全走 cmdQ）；
//     ProcessShard(idx) = per-shard 处理单元（drain cmdQ→dataQ→Tick 本桶房）——
//     Serial 顺序调用全部 / worker 池各调自己（调度归适配器，处理归容器——整洁架构落点）。
// 幂等（G2·Serial 简化形）：MatchSession::StartMatch 本就是全量复位——「不存在建 / 存在重置」；
//     worker 版 cmdQ 串行执行天然免竞态（同一 shard 的 cmd/data 严格单线程）。
#pragma once
#include "match/match_session.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sightline::match {

/** ep09 步骤 3：bot 登记端口（消费方拥有——match_shards 本地·ISP，同 IDamagePolicy 先例）。
 *  cmdQ 执行体经此登记/开局通知 bot——worker 下调用发生在归属 worker 线程（byRoom_ 单写者）。 */
struct IBotControl {
    virtual ~IBotControl() = default;
    virtual void RegisterBot(std::uint32_t roomId, std::uint32_t pid,
                             const std::string& name, std::uint8_t difficulty) = 0;
    virtual void UnregisterBot(std::uint32_t pid) = 0;
    virtual void OnMatchStarted(std::uint32_t roomId, const MapGeometry& geo,
                                MatchSession& session, std::uint64_t nowMs) = 0;
};

/** 开房共享依赖（构造注入·DIP）：session 创建时的只读共享件 + 初始配置闭包 + bot 控制口。
 *  geometry/damage 为 nullptr 时保持既有兼容路径（空表/内建策略——238 断言未注入行为）。 */
struct ShardDeps {
    const MapGeometry* geometry = nullptr;
    IDamagePolicy*     damage   = nullptr;
    IBotControl*       bot      = nullptr;   // cmdQ 执行体调（AddBot/RemoveBot/StartMatch 随行）
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

/** 控制面命令（ADR-21）：StartMatch/ApplyConfig/AddBot/RemoveBot——worker 版下主线程
 *  对 shard 状态的全部写入都经此队列（单 worker 内串行执行=零锁语义）。 */
struct ShardCommand {
    enum class Kind : std::uint8_t { Patch, StartMatch, ApplyConfig, AddBot, RemoveBot };
    Kind kind;
    std::uint32_t roomId = 0;
    std::string source;                          // Patch 的 lua 源码（步骤 4）
    std::vector<RosterEntry> roster;             // StartMatch 名册
    std::optional<GameConfig> config;            // ApplyConfig 快照（值过界）
    std::uint32_t pid = 0;                       // AddBot/RemoveBot
    std::string name;                            // AddBot
    std::uint8_t difficulty = 1;                 // AddBot
};

class MatchShards {
public:
    // ===== Shard：桶=房容器+双邮箱（先声明——方法签名引用）=====
    struct Shard {
        std::unordered_map<std::uint32_t, std::unique_ptr<MatchSession>> rooms;
        // ep09 步骤 2：跨线程邮箱（草案 §4.4——Serial 主线程投递+同帧消费；worker 版跨线程）
        std::deque<MatchInput>   dataQ;              // 数据面 FIFO（Move newest-wins）
        std::deque<ShardCommand> cmdQ;               // 控制面（先于数据面 drain）
        std::mutex               qMutex;             // dataQ/cmdQ 共用一把（同临界区，无嵌套锁）
        std::atomic<std::uint64_t> dropCount{0};     // G8：入队/校验丢弃原子计数（横幅读数）
        // ep09 步骤 3：出向合流（worker 产物→主线程收；Serial 直返不经此）
        std::mutex               outMutex;
        std::vector<MatchEvent>  outbox;
    };

    enum class Mode : std::uint8_t { Serial, Worker };
    static constexpr std::size_t kDefaultShardCount = 4;   // P1 待批示默认值（worker 数=分桶数）

    explicit MatchShards(ShardDeps deps, std::size_t n = kDefaultShardCount,
                         Mode mode = Mode::Serial)
        : deps_(std::move(deps)), shards_(n ? n : 1), mode_(mode) {}

    /** 开房（确保存在+注入）：无则建（seed=roomId 派生·ADR-23 + Deps 注入），有则幂等返回。
     *  **Serial 直调专用**——worker 版经 cmdQ（RouteCommand StartMatch）。
     *  **不自动 StartMatch**——开局时机/名册/事件流归调用方。 */
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

    /** 路由查——只许「归属线程」调（worker 版=归属 worker；Serial=主线程） */
    MatchSession* Find(std::uint32_t roomId) {
        auto& sh = shards_[roomId % shards_.size()];
        auto it = sh.rooms.find(roomId);
        return it == sh.rooms.end() ? nullptr : it->second.get();
    }

    // ===== ep09 步骤 3：控制面统一入口（wiring 调——容器内按 mode 分派）=====

    /** 开局：Serial=同帧执行（事件返回给 wiring 走既有 sink——行为零变化）；Worker=cmdQ（事件从合流出）。 */
    std::vector<MatchEvent> StartMatchVia(std::uint32_t roomId, std::vector<RosterEntry> roster) {
        ShardCommand c;
        c.kind = ShardCommand::Kind::StartMatch;
        c.roomId = roomId;
        c.roster = std::move(roster);
        if (mode_ == Mode::Serial) {
            std::vector<MatchEvent> out;
            applyCommand_(std::move(c), out, 0);
            return out;
        }
        RouteCommand(std::move(c));
        return {};
    }

    /** 配置热重载传播：Serial=直调全部；Worker=每 shard 一条 cmd（worker 执行 SetConfig——
     *  主线程不直写 weaponTable_）。 */
    void ApplyConfigVia(const GameConfig& c) {
        if (mode_ == Mode::Serial) {
            for (auto& sh : shards_)
                for (auto& [rid, s] : sh.rooms) s->SetConfig(c);
            return;
        }
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            ShardCommand cmd;
            cmd.kind = ShardCommand::Kind::ApplyConfig;
            cmd.config = c;
            RouteCommandTo(i, std::move(cmd));
        }
    }

    /** bot 登记（26/27 号）：Serial=同帧执行；Worker=cmdQ（归属 worker 内登记 byRoom_——单写者） */
    void RegisterBotVia(std::uint32_t roomId, std::uint32_t pid,
                        const std::string& name, std::uint8_t difficulty) {
        ShardCommand c;
        c.kind = ShardCommand::Kind::AddBot;
        c.roomId = roomId;
        c.pid = pid;
        c.name = name;
        c.difficulty = difficulty;
        if (mode_ == Mode::Serial) { std::vector<MatchEvent> out; applyCommand_(std::move(c), out, 0); }
        else RouteCommand(std::move(c));
    }
    void UnregisterBotVia(std::uint32_t pid) {
        // pid→roomId 反查经 locate（快照）——查不到=已不在房（RemoveBot 的 lobby 侧已清）
        std::uint32_t rid = 0, ep = 0;
        ShardCommand c;
        c.kind = ShardCommand::Kind::RemoveBot;
        c.pid = pid;
        if (deps_.locate && deps_.locate(pid, rid, ep)) {
            c.roomId = rid;
            if (mode_ == Mode::Serial) { std::vector<MatchEvent> out; applyCommand_(std::move(c), out, 0); }
            else RouteCommand(std::move(c));
        } else if (mode_ == Mode::Serial && deps_.bot) {
            deps_.bot->UnregisterBot(pid);   // 快照已清（离房先于移除）——直清兜底
        }
    }

    /** 启动期共享件注入（main 装配序：geometry 装载段/策略分支晚于容器声明——
     *  延迟到 setter；OpenRoom 全部发生在 cmdQ 执行期，晚于此两调，无竞态） */
    void SetSharedGeometry(const MapGeometry* g) { deps_.geometry = g; }
    void SetSharedDamage(IDamagePolicy* p) { deps_.damage = p; }

    // ===== ep09 步骤 2：输入路由（Mailbox 主路径）=====
    static constexpr std::size_t kDataQCap = 4096;   // 背压上界（压测定档）

    /** 数据面投递：locate 解析路由（快照无此 pid=丢）→ 按 roomId 分 shard → dataQ。
     *  Move newest-wins：同 (pid,roomId) 的未消费 Move 被新值覆盖（丢旧不丢新·插值无感）。
     *  满则丢+计数（横幅可见）——返回 false=被丢（调用方日志）。
     *  TODO(worker v2)：尾扫持锁 O(队列长)——worker 版换 pid→槽位索引（草案 R2）。 */
    bool Route(std::uint32_t pid, MatchInput in) {
        if (!deps_.locate) return false;
        if (!deps_.locate(pid, in.roomId, in.epoch)) return false;   // 不在房/人机外未知：在途判旧即丢
        in.pid = pid;
        auto& sh = shards_[in.roomId % shards_.size()];
        std::lock_guard lk(sh.qMutex);
        if (in.kind == MatchInput::Kind::Move) {
            for (auto it = sh.dataQ.rbegin(); it != sh.dataQ.rend(); ++it)
                if (it->kind == MatchInput::Kind::Move && it->pid == pid &&
                    it->roomId == in.roomId) { *it = std::move(in); return true; }   // newest-wins
        }
        if (sh.dataQ.size() >= kDataQCap) { ++sh.dropCount; return false; }
        sh.dataQ.push_back(std::move(in));
        return true;
    }

    /** 控制面投递（按 roomId 分 shard）——容量超限=系统异常级，横幅+计数 */
    bool RouteCommand(ShardCommand c) {
        auto& sh = shards_[c.roomId % shards_.size()];
        std::lock_guard lk(sh.qMutex);
        sh.cmdQ.push_back(std::move(c));
        return true;
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

    Mode mode() const { return mode_; }
    std::size_t shardCount() const { return shards_.size(); }

    // ===== ep09 步骤 3：per-shard 处理单元（Serial 与 worker 的公共 body）=====
    /** drain cmdQ（控制面先于数据面）→ drain dataQ（epoch 判旧/E8 门）→ Tick 本桶房。
     *  Serial：主线程逐 idx 调（TickAll）；Worker：归属 worker 的 loop 独调（事件入 shard.outbox）。 */
    void ProcessShard(std::size_t idx, std::uint64_t nowMs, std::vector<MatchEvent>& out) {
        auto& sh = shards_[idx];
        std::deque<MatchInput> inputs;
        std::deque<ShardCommand> cmds;
        {
            std::lock_guard lk(sh.qMutex);
            cmds.swap(sh.cmdQ);
            inputs.swap(sh.dataQ);
        }
        for (auto& c : cmds) applyCommand_(std::move(c), out, idx);     // 控制面先于数据面
        for (auto& in : inputs) applyInput_(std::move(in), nowMs, out, sh);
        for (auto& [rid, s] : sh.rooms) {
            auto evs = s->Tick(nowMs);
            out.insert(out.end(), std::make_move_iterator(evs.begin()),
                       std::make_move_iterator(evs.end()));
        }
    }

    /** Serial 兼容壳（测试/直调方）：全部 shard 的 ProcessShard 合并——
     *  worker 版由 ITickDispatcher（serial/worker_dispatcher）接管节拍，本壳不进 worker 路径 */
    std::vector<MatchEvent> DrainAndApply(std::uint64_t nowMs) {
        std::vector<MatchEvent> out;
        for (std::size_t i = 0; i < shards_.size(); ++i) ProcessShard(i, nowMs, out);
        return out;
    }

    /** worker 产物入合流（worker loop 调——锁瞬时） */
    void PublishToMerge(std::size_t idx, std::vector<MatchEvent>&& evs) {
        auto& sh = shards_[idx];
        if (evs.empty()) return;
        std::lock_guard lk(sh.outMutex);
        sh.outbox.insert(sh.outbox.end(), std::make_move_iterator(evs.begin()),
                         std::make_move_iterator(evs.end()));
    }

    /** 合流区收割（主线程 OnFrame 调——worker 版 OnFrameTick 的 body） */
    std::vector<MatchEvent> DrainMerge() {
        std::vector<MatchEvent> out;
        for (auto& sh : shards_) {
            std::vector<MatchEvent> local;
            {
                std::lock_guard lk(sh.outMutex);
                local.swap(sh.outbox);
            }
            out.insert(out.end(), std::make_move_iterator(local.begin()),
                       std::make_move_iterator(local.end()));
        }
        return out;
    }

private:
    void applyCommand_(ShardCommand&& c, std::vector<MatchEvent>& out, std::size_t idx) {
        switch (c.kind) {
        case ShardCommand::Kind::StartMatch: {
            // G2 幂等：OpenRoom（无则建+注入）+StartMatch（全量复位）——worker 下此执行在归属
            // worker 线程（rooms map 单写者）；15 号名册事件进 out（Serial 归 wiring sink/Worker 归合流）。
            auto& s = OpenRoom(c.roomId);
            auto evs = s.StartMatch(c.roomId, std::move(c.roster));
            out.insert(out.end(), std::make_move_iterator(evs.begin()),
                       std::make_move_iterator(evs.end()));
            if (deps_.bot) {
                auto* session = Find(c.roomId);
                if (session) deps_.bot->OnMatchStarted(c.roomId, s.Geometry(), *session, 0);
            }
            break;
        }
        case ShardCommand::Kind::ApplyConfig: {
            if (!c.config) break;
            auto& sh = shards_[idx];
            for (auto& [rid, s] : sh.rooms) s->SetConfig(*c.config);   // 本 shard 全房（归属线程内）
            break;
        }
        case ShardCommand::Kind::AddBot:
            if (deps_.bot) deps_.bot->RegisterBot(c.roomId, c.pid, c.name, c.difficulty);
            break;
        case ShardCommand::Kind::RemoveBot:
            if (deps_.bot) deps_.bot->UnregisterBot(c.pid);
            break;
        case ShardCommand::Kind::Patch:
            break;   // 步骤 4：LuaRuntime 聚合接线（G5 两阶段）
        }
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

    /** ADR-23：seed=roomId 派生（乘法混淆防相邻房种子线性相关） */
    static std::uint32_t DeriveSeed(std::uint32_t roomId) {
        return roomId * 2654435761u ^ 0x9E3779B9u;
    }

    bool RouteCommandTo(std::size_t idx, ShardCommand c) {
        auto& sh = shards_[idx];
        std::lock_guard lk(sh.qMutex);
        sh.cmdQ.push_back(std::move(c));
        return true;
    }

    ShardDeps deps_;
    std::vector<Shard> shards_;
    Mode mode_;
};

} // namespace sightline::match
