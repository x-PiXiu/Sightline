// bot_driver_port.h —— ep08 步骤 3：把既有 BotDriver 适配成用例层 IBotDriver 端口
// 整洁架构：OrchestratorDeps 只认 IBotDriver，不 import adapters::BotDriver（v1.5 实勘修正）。
// 本适配器在构造时绑定 match / lobby，Tick(now) 转发给既有三参驱动，事件由编排器统一扇出。
#pragma once
#include <cstdint>
#include <vector>

#include "adapters/bot_driver.h"
#include "application/ports.h"
#include "lobby/lobby.h"
#include "match/match_session.h"
#include "match/match_shards.h"

namespace adapters {

class BotDriverPort final : public sightline::application::IBotDriver {
public:
    BotDriverPort(BotDriver& driver,
                  sightline::match::MatchShards& shards,
                  sightline::lobby::LobbyService& lobby)
        : driver_(driver), shards_(shards), lobby_(lobby) {}

    std::vector<sightline::application::MatchEvent> Tick(std::uint64_t nowMs) override {
        return driver_.Tick(nowMs, shards_, lobby_);
    }

private:
    BotDriver& driver_;
    sightline::match::MatchShards& shards_;
    sightline::lobby::LobbyService& lobby_;
};

} // namespace adapters
