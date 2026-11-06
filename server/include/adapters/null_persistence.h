// null_persistence.h —— ep08 步骤 3：lite 模式持久化空实现（§5.7 双模式）
// 与 full 模式的 RealPersistenceAdapter 实现同一组端口，组合根二选一装配。
// 语义：不连库、不落盘、不阻塞对局；「审计不丢」在 lite 下兑现为「意图不丢」——只计数。
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "adapters/audit_wal.h"   // lite 审计落盘（§5.7.2——inline Append 调用需完整类型）
#include "application/ports.h"

namespace adapters {

class NullPersistence final : public sightline::application::IPersistencePorts {
public:
    // wal 非空：Append 额外写 WAL 文件（lite 模式的审计不丢）；空=纯计数（旧行为）
    explicit NullPersistence(AuditWal* wal = nullptr) : wal_(wal) {}
    void Ingest(std::span<const sightline::application::MatchEvent> events,
                sightline::application::Durability) override {
        events_ += events.size();
    }
    void Append(const sightline::application::AuditRecord& r) override {
        ++audits_;
        if (wal_) wal_->AppendAudit(r);      // §5.7.2：lite 审计落 WAL（full 启动回放补写）
    }
    void OnKill(std::uint32_t) override { ++kills_; }
    void Rebuild() override { ++rebuilds_; }
    // v1.6 收口⑤：执行者自报状态。lite 的 sink=内存计数器——恒健康、无队列（capacity=0
    // 无界）：分类恒 Enqueue，SpillToWal/DropAndCount 无假动作可走（两分支留给步骤 5 真队列）。
    // 「审计不丢」在 lite 兑现为意图计数 + LOG 行（文件头语义不变）。
    sightline::application::QueueState State() const override {
        using sightline::application::Durability;
        using sightline::application::QueueState;
        using sightline::application::SinkHealth;
        return QueueState{/*depth=*/0, /*capacity=*/0, Durability::BestEffort,
                          SinkHealth{/*mysql=*/true, /*mongo=*/true, /*redis=*/true}};
    }

    std::size_t Events() const { return events_; }
    std::size_t Audits() const { return audits_; }
    std::size_t Kills() const { return kills_; }

private:
    AuditWal* wal_ = nullptr;
    std::size_t events_ = 0, audits_ = 0, kills_ = 0, rebuilds_ = 0;
};

} // namespace adapters
