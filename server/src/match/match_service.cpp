// match_service 实现 —— M3：开始对局 + 成员离场；M4：实体暂存 + 快照组装
#include "match/match_service.h"

namespace sightline::match {

std::vector<MatchEvent> MatchService::StartMatch(std::uint32_t roomId, std::vector<RosterEntry> roster) {
    roomId_ = roomId;
    bActive_ = true;
    entities_.clear();
    for (std::size_t i = 0; i < roster.size(); ++i) {
        roster[i].spawnIndex = static_cast<std::uint8_t>(i);   // 出生点 = 名册序（v3 契约规则 7）
        Entity E;
        E.playerId = roster[i].playerId;                       // 位置先落原点——首个 16 上报覆盖
        entities_.push_back(E);
    }
    std::vector<MatchEvent> out;
    out.push_back({MatchEvent::Type::MatchStart, roomId_, std::move(roster), 0});
    return out;
}

std::vector<MatchEvent> MatchService::RemovePlayer(std::uint32_t playerId) {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;
    for (auto it = entities_.begin(); it != entities_.end(); ++it) {
        if (it->playerId == playerId) {
            entities_.erase(it);
            out.push_back({MatchEvent::Type::EntityLeave, roomId_, {}, playerId});
            break;
        }
    }
    return out;
}

bool MatchService::IsActive(std::uint32_t playerId) const {
    for (const Entity& E : entities_) if (E.playerId == playerId) return true;
    return false;
}

void MatchService::UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                                float yaw, float pitch, std::uint8_t flags, std::uint32_t seq) {
    for (Entity& E : entities_) {
        if (E.playerId == playerId) {
            E.x = x; E.y = y; E.z = z;
            E.yaw = yaw; E.pitch = pitch;
            E.flags = flags;
            E.ackedSeq = std::max(E.ackedSeq, seq);          // v4：确认序号只进不退（乱序防御）
            E.bHasReport = true;                             // 首次上报：实体激活（此后入快照）
            return;                                          // 只覆盖不校验（直发版原味）
        }
    }
    // 未知 pid：不在局者上报无效——静默忽略（服务器的"信任但只转发"不等于"来者不拒"）
}

std::vector<MatchEvent> MatchService::TickSnapshot() {
    std::vector<MatchEvent> out;
    if (!bActive_) return out;
    MatchEvent E{MatchEvent::Type::Snapshot, roomId_, {}, 0};
    E.entities.reserve(entities_.size());
    for (const Entity& Src : entities_) {
        if (!Src.bHasReport) continue;                       // 未上报者不入快照（避免 (0,0,0) 幻影出生）
        E.entities.push_back({Src.playerId, Src.x, Src.y, Src.z, Src.yaw, Src.pitch, Src.flags, Src.ackedSeq});
    }
    out.push_back(std::move(E));
    return out;
}

} // namespace sightline::match
