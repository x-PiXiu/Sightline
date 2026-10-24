// match_service.h —— 第 3 期对局用例（整洁架构内两层：与 LobbyService 并列）
// 依赖规则（D3-15）：只认 PlayerId 不认 fd——fd↔PlayerId 翻译归适配层 wireMatch；
//                   与 LobbyService 互不 include，名册由适配层以纯数据过桥；
//                   零 include net/*——可脱离网络纯单测
// M3 段：开始对局（名册+出生点）+ 成员离场
// M4 段：实体位置暂存（UpdateEntity）+ 每 tick 快照组装（TickSnapshot，17 的载荷源）
// M5 段：开火裁决（Fire——四道门→hitscan 求交→结算扣血；规则全在 fire_judge.h 纯决策头，
//        用例只编排；时间以 nowMs 参数注入、几何以 SetGeometry 装配注入——域不持钟不读文件）
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "match/fire_judge.h"   // 实体层纯决策（WeaponState/几何合同/求交）——依赖向内

namespace sightline::match {

/** 对局名册项（纯数据；出生点序号 = 名册序，由 StartMatch 分配） */
struct RosterEntry {
    std::uint32_t playerId = 0;
    std::string   name;
    std::uint8_t  spawnIndex = 0;
};

/** 在局实体状态。v4（M3 · R12 位置权威收回）：x/y/z = 服务器权威推算位
 *  （=上次权威位 + 钳制后增量，首报=上报位）——不再是"转发上报值"的朴素版记账
 *  M5：weapon = 武器状态（hp/弹匣/换弹/射速——开火四道门的 ②③④ 作用对象） */
struct Entity {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;   // 权威位（快照推的就是它；诚实路径与上报值恒等，钳制后分叉）
    float yaw = 0.f, pitch = 0.f;      // 朝向无动力学模型：末次上报即权威
    std::uint8_t flags = 0;            // bit0=腾空
    std::uint32_t ackedSeq = 0;        // 已确认的最新输入序号（v4 · R11）
    bool bHasReport = false;           // 首次上报前不入快照（位置未知的玩家不进世界）
    float lx = 0.f, ly = 0.f, lz = 0.f; // 上次上报位（增量基准——权威增量=本次上报-上次上报）
    WeaponState weapon;                // M5：开火状态（MatchStart 时满血满匣）
};

/** 快照项（17 载荷的一行） */
struct EntitySnapshot {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;
    std::uint32_t ackedSeq = 0;        // v4：17 号每实体 25B→29B（契约破例 D4-01）
};

/** 对局领域事件（纯数据；FrameWriter 翻译成 15/17/18/21 帧）
 *  M5：FireResult = 21 号载荷源（全房间广播含射手自己——一条消息三方共用：Hitmarker/tracer/红闪） */
struct FireResultData {
    std::uint32_t shooterPid = 0;
    bool bHit = false;
    std::uint32_t victimPid = 0;
    float hitX = 0.f, hitY = 0.f, hitZ = 0.f;  // 未命中=射线终点（墙面或 Lmax 端点，tracer 停墙）
    std::uint8_t victimHp = 0;                 // 命中后余血（ep07 接血条/死亡判定，契约一次定稿）
};

struct MatchEvent {
    enum class Type { MatchStart, EntityLeave, Snapshot, FireResult };
    Type type;
    std::uint32_t roomId = 0;
    std::vector<RosterEntry> roster;      // MatchStart：全员名册
    std::uint32_t leavePlayerId = 0;      // EntityLeave
    std::vector<EntitySnapshot> entities; // Snapshot：在局实体全量（按接收者滤自己）
    FireResultData fire;                  // FireResult
};

class MatchService {
public:
    /** 开始对局：名册由适配层从 LobbyService 查询后过桥传入（校验房主/在房在适配层完成） */
    std::vector<MatchEvent> StartMatch(std::uint32_t roomId, std::vector<RosterEntry> roster);

    /** 成员离场（离开房间/断线共用）：在局则产出 EntityLeave；不在局（大厅离开/未知）返回空 */
    std::vector<MatchEvent> RemovePlayer(std::uint32_t playerId);

    /** 该玩家是否已在局中（对局期重复 START 的幂等判断由客户端切图状态兜底，此查询供日志/调试） */
    bool IsActive(std::uint32_t playerId) const;

    // —— M4：实体位置暂存与快照组装；M3：权威推算 ——
    /** 19 号输入入库（v4 · R12 权威收回）：最小合法性钳制（本拍位移超界钳回——保和解完整性的
     *  最小版，完整反作弊 ep08）→ 服务器按输入自推权威位（上次权威+钳制增量）→ 存权威位+ackedSeq；
     *  未知 pid 静默忽略（不在局者上报无效）。16 号对局期由适配层拒收（R10），不进此函数 */
    void UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                      float yaw, float pitch, std::uint8_t flags, std::uint32_t seq = 0);

    /** 每帧驱动 tick 调用：在局则返回一条 Snapshot 事件（全员实体，FrameWriter 按接收者滤自己） */
    std::vector<MatchEvent> TickSnapshot();

    // —— M5：开火裁决（D5 系列；几何合同=构建期烘焙注入，拒绝运行时上报）——
    /** 装配注入：main 启动时经 MapGeometryLoader 读几何合同后调用（缺失=空表，兼容无图环境） */
    void SetGeometry(const MapGeometry& geometry);

    /** 20 号开火裁决（唯一入口）。①在对局（未在局/未上报=丢）→ ②③④ 武器门（fire_judge 纯函数，
     *  任一不过静默丢——D5-03 无回执，ep08 起违规可见）→ ⑤ hitscan 求交（先遮挡原语后实体圆柱，
     *  取最近 t；judgeAtMs 预留=ep06 回溯只换判定时刻）→ ⑥ 扣血钳 0 → ⑦ 扣弹记时 → ⑧ FireResult
     *  全房间广播事件。nowMs 由适配层注入（steady 时钟，域不持钟） */
    std::vector<MatchEvent> Fire(std::uint32_t shooterPid, std::uint64_t nowMs);

private:
    std::vector<Entity> entities_;         // 在局实体（名册序；StartMatch 建，RemovePlayer 删）
    std::uint32_t roomId_ = 0;
    bool bActive_ = false;
    MapGeometry geometry_;                 // 几何合同（构建期烘焙注入；缺省=空遮挡+默认胶囊 40/90）
};

} // namespace sightline::match
