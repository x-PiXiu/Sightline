// orchestrator.h —— ep08 步骤 3：应用编排器（用例层，零 IO / 零框架 / 零 net import）
// 职责：帧节律、三处事件扇出、命令消费、配置变更订阅（G4/G5/G8）。
// 组合根只做 new + 注入；main.cpp 不再出现 deliverMatch / Tick / ingest / drain。
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "application/config_views.h"
#include "application/account_ports.h"   // ep09 步骤 5：AccountResult/IAccountResultSource
#include "application/patch_aggregator.h"
#include "application/durability_policy.h"
#include "application/ports.h"
#include "lobby/lobby.h"
#include "match/match_session.h"
#include "match/match_shards.h"
#include "match/policies.h"        // v1.6：端口已迁 match/（消费方拥有——见 policies.h 头注）

namespace sightline::application {

struct OrchestratorDeps {
    std::function<std::uint64_t()> now;          // ADR-1 单钟：main.cpp:78 nowMs，唯一注入点
    IConfigSource&  config;
    ICombatParams&  combat;
    IBotTuning&     tuning;              // v1.6 收口③：bot 段（ApplyConfig 用）
    IMatchRules&    rules;               // v1.6 收口③：deathmatch/rewind（此前零消费者）
    ICommandSource& commands;
    IWireSink&      wire;
    IMatchArchive&  archive;
    IAuditSink&     audit;
    ILeaderboard&   board;
    IConfigVersion& version;
    IPatchChannel*  patches = nullptr;          // 步骤 4 注入 LuaRuntime；步骤 3 为空
    ITickDispatcher&           tick;             // ep09 步骤 3：Serial/Worker 可插拔调度（§4.11）
    match::MatchShards&        shards;           // 配置传播/指标面（Tick 节拍已归 tick）
    // ===== ep09 步骤 5（G9/E9）：账号域回流（persistence resultQ→主线程→wiring 编码定向 send）=====
    IAccountResultSource* authResults = nullptr;          // full=RealPersistence；lite/null=账号域降级
    std::function<void(const std::vector<AccountResult>&)> onAuthResults;   // 延迟接线（SetAuthResultsSink——wiring 构造后）

    lobby::LobbyService&       lobby;            // Ended → SetRoomInMatch 过桥
};

class ApplicationOrchestrator {
public:
    explicit ApplicationOrchestrator(OrchestratorDeps d) : d_(std::move(d)) {
        ApplyConfig();              // 启动即把完整规则快照投喂对局用例（默认=ep07 常量）
    }

    void OnFrame() {
        if (stopped_) return;
        for (const auto& c : d_.commands.Drain()) Dispatch(c);
        const auto now = d_.now();
        auto events = d_.tick.OnFrameTick(now);      // ep09 步骤 3：drain cmdQ/dataQ+bot+Tick 统一收（§4.11）
        Fanout(events);
        for (const auto& e : events)
            if (e.type == MatchEvent::Type::Ended) d_.lobby.SetRoomInMatch(e.roomId, false);
            else if (e.type == MatchEvent::Type::PatchResult)
                patchAgg_.OnPatchResult(e.patchOk, now);   // ep09 G5：VM 回执聚合（两阶段收口）
        patchAgg_.OnFrameTick(now);                  // G5：500ms 超时检查
        // ep09 步骤 5：账号域回流（AccountResult→wiring 编码定向 send 33/35——域隔离独立管道）
        if (d_.authResults && d_.onAuthResults) {
            auto authResults = d_.authResults->DrainAuthResults();   // 账号域回流（与对局域隔离）
            if (!authResults.empty()) d_.onAuthResults(authResults);
        }
    }

    // 事件源 ③：MatchWiring 的 Fire/StartMatch/RemovePlayer 处理器经回调送同一订阅点（B1 修复）。
    // v1.6 收口⑥：MatchStart 在此触发 30 号广播——「开局先 15 号后 30 号」的时序归用例层
    // （此前由 FrameWriter 内部在 MatchStart 编码后自播=第二数据入口，已收敛为端口单一入口）。
    void OnWiringEvents(std::vector<MatchEvent> events) {
        // v1.6 收口⑥的 30 号随行已并入 Fanout（MatchStart 检测统一）——Serial/Worker 两路同源
        Fanout(events);                                        // 15 号先出（DeliverMatch）→ 30 号随后
    }

    // G4：配置变更是独立域事件，不混进 MatchEvent 流；编排器订阅后写审计 + 触发 30 号广播。
    void OnConfigChanged(const ConfigChanged& e) {
        ApplyConfig();         // 数值类即时生效：下一发即按新表判定（§3.5 演示第一拍）
        d_.audit.Append(AuditRecord{e.version, e.note, "config_service", e.diffDigest, e.atUtcMs});
        d_.wire.BroadcastConfig(d_.version.Snapshot());
        SetNotice("配置生效 " + e.version + " · " + e.note);
    }

    // 停机：停 ingest（本编排器停止接帧）→ drain / join 由步骤 5 的持久化 worker 承担
    void Shutdown() { stopped_ = true; }

    // 用例层不打印日志：主循环读 Notice() 变化后由适配器落 LOG_INFO
    const std::string& Notice() const { return notice_; }

private:
    // ===== ep09 步骤 4（G5·ADR-13）：patch 两阶段聚合器（主线程单写者）=====
    struct AggSink final : PatchAggregator::Sink {
        explicit AggSink(ApplicationOrchestrator& o) : o_(o) {}
        void BroadcastPatch(std::size_t idx, const std::string& name,
                            const std::string& source) override {
            o_.d_.shards.RouteCommandTo(idx, [&] {
                match::ShardCommand c;
                c.kind = match::ShardCommand::Kind::Patch;
                c.roomId = 0;                          // 路由专用（RouteCommandTo 直指定 idx）
                c.patchName = name;
                c.source = source;
                return c;
            }());
        }
        void Notice(const std::string& msg) override { o_.SetNotice(msg); }
        std::string LastGoodSource(const std::string& name) override {
            auto* ch = o_.d_.shards.PatchChannelAt(0);  // 恢复点取任一通道（加载初值/上次成功版）
            return ch ? ch->LastGoodSource(name) : std::string();
        }
        std::size_t ShardCount() const override { return o_.d_.shards.shardCount(); }
        void AuditPatch(const std::string& name, bool ok, const std::string& detail,
                        std::uint64_t atMs) override {
            // R10：patch 收口审计留痕（AuditWal/MySQL——与 ConfigChanged 同一审计链）
            // 时钟：main.nowMs=system 墙钟（ADR-1 注释）——聚合器收口时刻透传
            o_.d_.audit.Append(sightline::application::AuditRecord{
                "patch:" + name, ok ? "成功 " + detail : "失败 " + detail,
                "patch_aggregator", "", atMs});
        }
        ApplicationOrchestrator& o_;
    };

    void SetNotice(std::string s) { notice_ = std::move(s); }

    // 完整规则快照 → 对局用例（用例层不认识 ConfigService；经五张只读视图组装 GameConfig）。
    // v1.6 收口③：deathmatch/rewind/bot 三组随行——此前只推武器表，改 respawn_ms 不生效。
    void ApplyConfig() {
        // ep09：组装逻辑提为 ComposeGameConfig 单源（config_views.h）——MatchShards 开房注入同源
        d_.shards.ApplyConfigVia(ComposeGameConfig(d_.combat, d_.tuning, d_.rules, d_.version));   // ep09：Serial 直调/Worker cmdQ
    }

    void Dispatch(const ConsoleCommand& c) {
        switch (c.kind) {
        case ConsoleCommand::Kind::Reload: {
            const auto r = d_.config.Reload(c.arg);
            SetNotice(r.ok ? std::string("reload 成功 ") + r.versionOrError
                           : std::string("reload 拒收（保留旧表）: ") + r.versionOrError);
            break;
        }
        case ConsoleCommand::Kind::Rollback:
            d_.config.Rollback();
            SetNotice("rollback → 内存回退上一版（与磁盘分叉）");
            break;
        case ConsoleCommand::Kind::Version:
            SetNotice("version: " + d_.version.Version());   // v1.6：按值返回
            break;
        case ConsoleCommand::Kind::Patch: {
            // 步骤 4：公式/决策面热修（IPatchChannel）。source 空=运行时自读 scriptDir（IO 在适配器）。
            // patch 审计行随步骤 5 AuditWal 落库（lite 下 LOG 行已带时间戳记录——双钟分立：
            // 这里不拿 steady 的 d_.now() 充当 UTC 审计时钟）。
            // ep09 G5：patch 广播聚合（两阶段收口）——HAS_LUA off 由聚合器拒收
            (void)patchAgg_.Begin(c.arg, d_.now());   // ep09 G5：广播聚合——收口在 PatchResult 事件回流
            break;
        }
        case ConsoleCommand::Kind::Help:
            SetNotice(c.arg.empty()
                ? "命令: reload [note] | rollback | version | patch <name> | help"
                : c.arg);
            break;
        }
    }

    void Fanout(std::span<const MatchEvent> es) {
        if (es.empty()) return;
        d_.wire.DeliverMatch(es);
        bool bStart = false;
        for (const auto& e : es)
            if (e.type == MatchEvent::Type::MatchStart) bStart = true;
        if (bStart) d_.wire.BroadcastConfig(d_.version.Snapshot());   // v1.6 收口⑥：15 号后 30 号（两路统一）
        // §5.3 分级（v1.6 收口⑤）：终局/击杀=Must，其余 BestEffort——
        // 决策归纯策略（DurabilityPolicy），执行归 sink（按 QueueAction 真分派，不再
        // 「算出 SpillToWal 却只当 !=DropAndCount 用」的假分派）；状态由执行者自报（State()）。
        Durability cls = Durability::BestEffort;
        for (const auto& e : es) {
            if (e.type == MatchEvent::Type::Ended || e.type == MatchEvent::Type::Kill)
                cls = Durability::Must;
            if (e.type == MatchEvent::Type::Kill)
                d_.board.OnKill(e.killKiller);   // 榜单真接线（此前 (void)d_.board 悬空）
        }
        QueueState st = d_.archive.State();      // 执行者自报（lite：无界+恒健康）
        st.cls = cls;
        switch (DurabilityPolicy::Classify(st)) {
        case QueueAction::Enqueue:
            d_.archive.Ingest(es, cls);
            break;
        case QueueAction::SpillToWal:
            // lite 无 WAL 执行者（步骤 5 装）：Must 事件降级到计数执行者——
            // 真实 WAL 落地时此分支改交 AuditWal；降级路径必须在此显式可见（不许静默）。
            d_.archive.Ingest(es, cls);
            break;
        case QueueAction::DropAndCount:
            break;                               // BestEffort 满：丢（lite 无队列走不到）
        case QueueAction::Defer:
            d_.archive.Ingest(es, cls);          // 步骤 5：延迟重试语义
            break;
        }
    }

    AggSink aggSink_{*this};
    PatchAggregator patchAgg_{aggSink_};
    OrchestratorDeps d_;
    std::string notice_;
    bool stopped_ = false;
};

} // namespace sightline::application
