// match_service 实现 —— M3：开始对局 + 成员离场；M4：实体暂存 + 快照组装；M3：权威推算（R12）
#include "match/match_service.h"
#include <algorithm>
#include <cmath>

namespace sightline::match {
namespace {
    // 最小合法性钳制（M3 版=和解完整性保险，非反作弊——完整校验与踢人 ep08 落地）：
    // 界 = 最大合法速度 × 单帧最大间隔。按"一条输入最多携带一个帧间隔的位移"设界，
    // 故意不用服务器测得的到达间隔——延迟滑块会让输入突发到达（间隔 1~5ms 但各含 ~16ms 位移），
    // 按到达间隔钳制会误伤诚实移动、刷爆客户端和解。
    constexpr float kMaxSpeedUuPerSec = 1000.f;   // uu/s（UE 厘米制；行走 450 / 冲刺 ~700，留裕量）
    constexpr float kMaxInputGapSec   = 0.1f;     // 100ms：帧率跌到 10fps 的单帧位移也放行
}

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
        if (E.playerId != playerId) continue;
        if (!E.bHasReport) {
            E.x = x; E.y = y; E.z = z;                       // 首报：权威位=上报位（本地生成点即真值）
            E.bHasReport = true;                             // 实体激活（此后入快照）
        } else {
            float dx = x - E.lx, dy = y - E.ly, dz = z - E.lz;   // 客户端声明的本拍位移
            // seq 间隙感知：丢帧时 dx 含多帧位移，bound 按实际间隔倍数放大（cap 3x 防刷）
            const std::uint32_t gap = (seq > E.ackedSeq) ? (seq - E.ackedSeq) : 1u;
            const float gapScale = std::min(static_cast<float>(gap), 3.0f);
            const float bound = kMaxSpeedUuPerSec * kMaxInputGapSec * gapScale;
            const float len2 = dx * dx + dy * dy + dz * dz;
            if (len2 > bound * bound) {                      // 超界钳回界上：保方向弃模长（传送黑客打不动权威位）
                const float k = bound / std::sqrt(len2);
                dx *= k; dy *= k; dz *= k;
            }
            E.x += dx; E.y += dy; E.z += dz;                 // 权威=上次权威+钳制增量（诚实路径与上报恒等）
        }
        E.lx = x; E.ly = y; E.lz = z;                        // 增量基准永远=客户端声明轨迹
        E.yaw = yaw; E.pitch = pitch;                        // 朝向无动力学：末次上报即权威
        E.flags = flags;
        E.ackedSeq = std::max(E.ackedSeq, seq);              // 确认序号只进不退（乱序防御 · R11）
        return;
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
