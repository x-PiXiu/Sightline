// null_persistence.h —— ep08 步骤 3：lite 模式持久化空实现（§5.7 双模式）
// 与 full 模式的 RealPersistenceAdapter 实现同一组端口，组合根二选一装配。
// 语义：不连库、不落盘、不阻塞对局；「审计不丢」在 lite 下兑现为「意图不丢」——只计数。
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

#include "application/ports.h"

namespace adapters {

class NullPersistence final : public sightline::application::IPersistencePorts {
public:
    void Ingest(std::span<const sightline::application::MatchEvent> events,
                sightline::application::Durability) override {
        events_ += events.size();
    }
    void Append(const sightline::application::AuditRecord&) override { ++audits_; }
    void OnKill(std::uint32_t) override { ++kills_; }
    void Rebuild() override { ++rebuilds_; }

    std::size_t Events() const { return events_; }
    std::size_t Audits() const { return audits_; }
    std::size_t Kills() const { return kills_; }

private:
    std::size_t events_ = 0, audits_ = 0, kills_ = 0, rebuilds_ = 0;
};

} // namespace adapters
