// serial_dispatcher.h —— ep09 步骤 3：ITickDispatcher 的 Serial 实现（主线程顺序调度）
// 整洁架构：orchestrator 只认 ITickDispatcher 端口；本适配器组合容器处理单元+bot 驱动——
// 与 worker_pool.h（PinnedWorker 实现）同一端口两形态（草案 §4.11 可插拔/ADR-20）。
// Serial=行为零变化路径：ProcessShard 逐 idx（drain cmdQ→dataQ→Tick）+ bot 全局驱动——
// 与重构前的 OnFrame 三路（drainEvents/botEvents/matchEvents）完全等价。
#pragma once
#include <cstdint>
#include <vector>

#include "adapters/bot_driver.h"
#include "adapters/lobby_wiring.h"     // LobbyService 经 lobby.h
#include "lobby/lobby.h"
#include "match/match_shards.h"
#include "application/ports.h"

namespace adapters {

class SerialDispatcher final : public sightline::application::ITickDispatcher {
public:
    SerialDispatcher(sightline::match::MatchShards& shards,
                     BotDriver& bot,
                     sightline::lobby::LobbyService& lobby)
        : shards_(shards), bot_(bot), lobby_(lobby) {}

    void Start() override {}       // Serial：无线程生命周期
    void Stop() override {}        // G11 阶段由 main/persistence 既有序承担

    std::vector<sightline::application::MatchEvent> OnFrameTick(std::uint64_t nowMs) override {
        std::vector<sightline::application::MatchEvent> out;
        for (std::size_t i = 0; i < shards_.shardCount(); ++i)
            shards_.ProcessShard(i, nowMs, out);            // drain cmdQ→dataQ→Tick 本桶房
        auto botEvents = bot_.Tick(nowMs, shards_, lobby_); // 人机驱动（主线程单线程铁律不变）
        out.insert(out.end(), std::make_move_iterator(botEvents.begin()),
                   std::make_move_iterator(botEvents.end()));
        return out;
    }

private:
    sightline::match::MatchShards& shards_;
    BotDriver& bot_;
    sightline::lobby::LobbyService& lobby_;
};

} // namespace adapters
