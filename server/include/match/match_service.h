// match_service.h —— 第 3 期对局用例（整洁架构内两层：与 LobbyService 并列）
// 依赖规则（D3-15）：只认 PlayerId 不认 fd——fd↔PlayerId 翻译归适配层 wireMatch；
//                   与 LobbyService 互不 include，名册由适配层以纯数据过桥；
//                   零 include net/*——可脱离网络纯单测
// M3 段：开始对局（名册+出生点）+ 成员离场
// M4 段：实体位置暂存（UpdateEntity）+ 每 tick 快照组装（TickSnapshot，17 的载荷源）
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sightline::match {

/** 对局名册项（纯数据；出生点序号 = 名册序，由 StartMatch 分配） */
struct RosterEntry {
    std::uint32_t playerId = 0;
    std::string   name;
    std::uint8_t  spawnIndex = 0;
};

/** 在局实体状态（直发版服务器只暂存不校验——"故意做错"，v3 契约规则 9） */
struct Entity {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;            // bit0=腾空
    bool bHasReport = false;           // 首次 16 上报前不入快照（位置未知的玩家不进世界）
};

/** 快照项（17 载荷的一行） */
struct EntitySnapshot {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;
};

/** 对局领域事件（纯数据；FrameWriter 翻译成 15/17/18 帧——v3 契约） */
struct MatchEvent {
    enum class Type { MatchStart, EntityLeave, Snapshot };
    Type type;
    std::uint32_t roomId = 0;
    std::vector<RosterEntry> roster;      // MatchStart：全员名册
    std::uint32_t leavePlayerId = 0;      // EntityLeave
    std::vector<EntitySnapshot> entities; // Snapshot：在局实体全量（按接收者滤自己）
};

class MatchService {
public:
    /** 开始对局：名册由适配层从 LobbyService 查询后过桥传入（校验房主/在房在适配层完成） */
    std::vector<MatchEvent> StartMatch(std::uint32_t roomId, std::vector<RosterEntry> roster);

    /** 成员离场（离开房间/断线共用）：在局则产出 EntityLeave；不在局（大厅离开/未知）返回空 */
    std::vector<MatchEvent> RemovePlayer(std::uint32_t playerId);

    /** 该玩家是否已在局中（对局期重复 START 的幂等判断由客户端切图状态兜底，此查询供日志/调试） */
    bool IsActive(std::uint32_t playerId) const;

    // —— M4：实体位置暂存与快照组装 ——
    /** 16 上报入库（只覆盖不校验；未知 pid 静默忽略——不在局者上报无效） */
    void UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                      float yaw, float pitch, std::uint8_t flags);

    /** 每帧驱动 tick 调用：在局则返回一条 Snapshot 事件（全员实体，FrameWriter 按接收者滤自己） */
    std::vector<MatchEvent> TickSnapshot();

private:
    std::vector<Entity> entities_;         // 在局实体（名册序；StartMatch 建，RemovePlayer 删）
    std::uint32_t roomId_ = 0;
    bool bActive_ = false;
};

} // namespace sightline::match
