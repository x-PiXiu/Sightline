// ports.h —— ep08 步骤 2：出站 / 入站端口（用例层所有，适配器实现）
// 纪律：本目录不得 include 适配器目录、网络库、协议字节层或任何第三方驱动头。
// §11.6 自检命令第 1 / 2 / 4 条负责钉住这条依赖方向。
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "application/config_views.h"
#include "match/match_service.h"

namespace sightline::application {

using MatchEvent = sightline::match::MatchEvent;

enum class Durability : std::uint8_t { Must, BestEffort, Projection };
struct SinkHealth { bool mysql = false, mongo = false, redis = false; };
enum class QueueAction : std::uint8_t { Enqueue, DropAndCount, SpillToWal, Defer };
struct DegradeDecision { bool continueMatch = true; bool warn = false; };
// QueueState 住本头（v1.6 收口⑤）：IMatchArchive::State() 按值返回它——执行者自报队列状态，
// 编排器不再硬编码假状态（此前 depth=0/health={} 的假分类是要灭的）。
struct QueueState {
    std::size_t depth = 0;
    std::size_t capacity = 0;   // 0 表示不设上限（lite/调试用）
    Durability cls = Durability::BestEffort;
    SinkHealth health{};
};

struct IMatchArchive {
    virtual ~IMatchArchive() = default;
    virtual void Ingest(std::span<const MatchEvent> events, Durability d) = 0;
    // 执行者自报状态（v1.6）：分类决策基于真实队列/健康位，不许编排器代填。
    virtual QueueState State() const = 0;
};

struct IAuditSink {
    virtual ~IAuditSink() = default;
    virtual void Append(const AuditRecord&) = 0;
};

struct ILeaderboard {
    virtual ~ILeaderboard() = default;
    virtual void OnKill(std::uint32_t killerPid) = 0;
    virtual void Rebuild() = 0;   // G7：persistence 内部协调，非 adapter 直调
};

// §5.7：full / lite 两个持久化适配器实现同一组端口，组合根二选一装配。
struct IPersistencePorts : IMatchArchive, IAuditSink, ILeaderboard {};

struct IWireSink {
    virtual ~IWireSink() = default;
    virtual void DeliverMatch(std::span<const MatchEvent>) = 0;   // 现有 FrameWriter::deliverMatch
    // 步骤 6：30 号 S2C_CONFIG_VERSION 广播。默认 no-op 让步骤 3 的编排器能先编译，
    // 契约落地时由 WireSinkAdapter 覆盖——用例层不因此依赖协议字节层。
    virtual void BroadcastConfig(const ConfigWireSnapshot&) {}
};

struct ICommandSource {
    virtual ~ICommandSource() = default;
    virtual std::vector<ConsoleCommand> Drain() = 0;   // stdin 线程只入队（A1）
};

struct IBanStore {   // §14-P1：ep08 只建通道，ep09 防作弊期消费
    virtual ~IBanStore() = default;
    virtual void Ban(std::string_view name, std::uint32_t ttlSec) = 0;
    virtual bool IsBanned(std::string_view name) const = 0;
};

struct IPatchChannel {
    virtual ~IPatchChannel() = default;
    virtual PatchResult Patch(std::string_view name, std::string_view source) = 0;
};

// 整洁架构修正（v1.5 实勘发现）：设计草案 §10.1.3 的 OrchestratorDeps 原写
// `adapters::BotDriver&` —— 会让用例层 import 适配器，违反 §11.6 第 2 条自检。
// 改用本端口；adapters::BotDriver 在步骤 3 实现它（组合根注入具体实现）。
struct IBotDriver {
    virtual ~IBotDriver() = default;
    virtual std::vector<MatchEvent> Tick(std::uint64_t nowMs) = 0;
};

} // namespace sightline::application
