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
#include "application/durability_policy.h"
#include "application/policies.h"
#include "application/ports.h"
#include "lobby/lobby.h"
#include "match/match_service.h"

namespace sightline::application {

struct OrchestratorDeps {
    std::function<std::uint64_t()> now;          // ADR-1 单钟：main.cpp:78 nowMs，唯一注入点
    IConfigSource&  config;
    ICombatParams&  combat;
    ICommandSource& commands;
    IWireSink&      wire;
    IMatchArchive&  archive;
    IAuditSink&     audit;
    ILeaderboard&   board;
    IConfigVersion& version;
    IDamagePolicy*  damage = nullptr;            // 步骤 4 接 Lua 策略
    IBotPolicy*     bot = nullptr;
    IPatchChannel*  patches = nullptr;          // 步骤 4 注入 LuaRuntime；步骤 3 为空
    match::MatchService&       match;
    IBotDriver&                botDriver;
    lobby::LobbyService&       lobby;            // Ended → SetRoomInMatch 过桥
};

class ApplicationOrchestrator {
public:
    explicit ApplicationOrchestrator(OrchestratorDeps d) : d_(std::move(d)) {
        ApplyWeaponTable();        // 启动即把配置表投喂给对局用例（默认表逐字段等于 ep07 常量）
    }

    void OnFrame() {
        if (stopped_) return;
        for (const auto& c : d_.commands.Drain()) Dispatch(c);
        const auto now = d_.now();
        auto botEvents   = d_.botDriver.Tick(now);   // 事件源 ②（原 main.cpp:238）
        auto matchEvents = d_.match.Tick(now);       // 事件源 ①（原 main.cpp:239）
        Fanout(botEvents);
        Fanout(matchEvents);
        for (const auto& e : matchEvents)
            if (e.type == MatchEvent::Type::Ended) d_.lobby.SetRoomInMatch(e.roomId, false);
    }

    // 事件源 ③：MatchWiring 20 号处理器经回调把 Fire 事件送到同一订阅点（B1 修复）
    void OnFireEvents(std::vector<MatchEvent> events) { Fanout(events); }

    // G4：配置变更是独立域事件，不混进 MatchEvent 流；编排器订阅后写审计 + 触发 30 号广播。
    void OnConfigChanged(const ConfigChanged& e) {
        ApplyWeaponTable();        // 数值类即时生效：下一发即按新表判定（§3.5 演示第一拍）
        d_.audit.Append(AuditRecord{e.version, e.note, "config_service", e.diffDigest, e.atUtcMs});
        d_.wire.BroadcastConfig(d_.version.Snapshot());
        SetNotice("配置生效 " + e.version + " · " + e.note);
    }

    // 停机：停 ingest（本编排器停止接帧）→ drain / join 由步骤 5 的持久化 worker 承担
    void Shutdown() { stopped_ = true; }

    // 用例层不打印日志：主循环读 Notice() 变化后由适配器落 LOG_INFO
    const std::string& Notice() const { return notice_; }

private:
    void SetNotice(std::string s) { notice_ = std::move(s); }

    // 配置表 → 对局用例的纯值快照（用例层不认识 ConfigService；只经三张只读视图）
    void ApplyWeaponTable() {
        std::array<match::WeaponParams, 4> table{};
        for (std::size_t i = 0; i < table.size(); ++i)
            table[i] = d_.combat.Weapon(static_cast<match::EWeaponType>(i));
        d_.match.SetWeaponTable(table, d_.version.Snapshot().weaponCount, d_.combat.MaxHp());
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
            SetNotice("version: " + std::string(d_.version.Version()));
            break;
        case ConsoleCommand::Kind::Help:
            SetNotice(c.arg.empty()
                ? "命令: reload [note] | rollback | version | help"
                : c.arg);
            break;
        }
    }

    void Fanout(std::span<const MatchEvent> es) {
        if (es.empty()) return;
        d_.wire.DeliverMatch(es);
        // 事件分级（§5.3）：终局/击杀=Must，其余 BestEffort；纯策略分类结果在步骤 5
        // 由 persistence worker 执行 WAL / 丢弃计数，此处只把决策与事件一并交付。
        Durability cls = Durability::BestEffort;
        for (const auto& e : es)
            if (e.type == MatchEvent::Type::Ended || e.type == MatchEvent::Type::Kill)
                cls = Durability::Must;
        const QueueState st{0, 1024, cls, SinkHealth{}};
        if (DurabilityPolicy::Classify(st) != QueueAction::DropAndCount)
            d_.archive.Ingest(es, cls);
        (void)d_.board;   // 步骤 5：Kill 事件 → 榜单增量
    }

    OrchestratorDeps d_;
    std::string notice_;
    bool stopped_ = false;
};

} // namespace sightline::application
