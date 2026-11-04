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
#include <random>
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
    // —— ep06 回溯：权威位时间线（服务器接收时刻·单钟 ADR-1）——
    HistorySample history[kEntityHistoryCap]; // 环形（32 槽×1/30s ≈ 1.07s 窗口 > 回溯上限 1000ms）
    std::uint8_t histCount = 0;        // 有效条数（≤kEntityHistoryCap）
    std::uint8_t histHead = 0;         // 下一写槽（环形覆盖最老）
    std::uint16_t shooterRtt = 0;      // 该玩家上报的 RTT（22 号·1Hz；0=未上报 → 不回溯=ep05 行为）
    std::uint64_t lastRecvMs = 0;      // 上一条 19 号到达时刻（钳制时间版：卡顿后恢复的长间隔诚实位移放行）
    // —— ep07 死亡/重生状态机 ——
    bool bDead = false;                // hp≤0 置位（开火门0=存活门；命中候选过滤；快照 flags bit1）
    std::uint64_t respawnAtMs = 0;     // 死亡→重生时刻（Tick 到期处理）
    std::uint64_t invulnUntilMs = 0;   // 重生无敌截止（受击不扣血）
    std::uint64_t bornAtMs = 0;        // 本次出生时刻（命中候选过滤 judgeAt≥bornAt——❗8）
    bool rebasePending = false;        // 重生握手：忽略在途上报，出生点附近首条重定基准（❗9）
    float spawnX = 0.f, spawnY = 0.f, spawnZ = 90.f;  // 最近一次落点（rebase 判定锚）
    std::uint8_t spawnIx = 0;          // 出生点序（24 号回传——客户端 Spawn_{n+1} 同源落位）
    std::uint32_t kills = 0, deaths = 0;  // 比分（结算 25 号载荷源）
};

/** 快照项（17 载荷的一行） */
struct EntitySnapshot {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;
    std::uint32_t ackedSeq = 0;        // v4：17 号每实体 25B→29B（契约破例 D4-01）
};

/** 21 号载荷源 + 领域裁定（2026-11-04 审计 C 级收编：域事件只载裁定与 wire 字段，
 *  判定链诊断一律进 FireDiagnostics——域事件回归"纯裁定"职责） */
struct FireResultData {
    std::uint32_t shooterPid = 0;
    bool bHit = false;
    bool bAccepted = true;                     // false=被武器门拦下（②③④ 静默丢 D5-03——驱动器丢事件过滤）
    std::uint8_t gate = 0;                     // FireGate 枚举值（0=Ok 1=换弹 2=弹匣 3=射速 4=死亡/终局）
    std::uint32_t victimPid = 0;
    float hitX = 0.f, hitY = 0.f, hitZ = 0.f;  // 未命中=射线终点（墙面或 Lmax 端点，tracer 停墙）
    std::uint8_t victimHp = 0;                 // 命中后余血（ep07 接血条/死亡判定，契约一次定稿）
};

/** 判定链诊断快照（2026-10-28 用户令"判定链全参数可读" + 2026-11-03 miss 最近候选 +
 *  2026-11-04 [无敌]旗——审计 C 级收编：从域事件拆出，适配层判定行专用，**永不入 21 号 wire**） */
struct FireDiagnostics {
    float oX = 0.f, oY = 0.f, oZ = 0.f;        // 射线起点（眼位）
    float dX = 0.f, dY = 0.f, dZ = 0.f;        // 射线方向（单位向量）
    float dist = 0.f;                          // 命中距离（未命中=Lmax）
    float shooterX = 0.f, shooterY = 0.f, shooterZ = 0.f;  // 射手实体权威位（19 上报推算值）
    float victimX = 0.f, victimY = 0.f, victimZ = 0.f;     // 受害实体权威位（未命中=0）
    float hbRadius = 0.f, hbHalfH = 0.f;                   // 本次判定用命中体（几何合同烘焙值）
    std::uint16_t rewindMs = 0;                // ep06：本次回溯量 now−judgeAt（0=不回溯）
    std::uint32_t nearPid = 0;                 // miss 最近候选 pid（0=无候选——全员死亡/未激活）
    float nearPerp = 0.f;                      // 其回溯采样位到射线的垂直距离（cm）
    float nearX = 0.f, nearY = 0.f, nearZ = 0.f;  // 该候选的采样位（judgeAt 时刻）
    bool nearInvuln = false;                   // 该候选仅因重生无敌被挡（2026-11-04"指着身体打不中"实锤）
};

struct ScoreRow {                          // ep07：结算行（25 号载荷源）
    std::uint32_t playerId = 0;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};

struct MatchEvent {
    enum class Type { MatchStart, EntityLeave, Snapshot, FireResult,
                      Kill, Respawn, Ended };   // ep07：+三态
    Type type;
    std::uint32_t roomId = 0;
    std::vector<RosterEntry> roster;      // MatchStart：全员名册
    std::uint32_t leavePlayerId = 0;      // EntityLeave
    std::vector<EntitySnapshot> entities; // Snapshot：在局实体全量（按接收者滤自己）
    FireResultData fire;                  // FireResult：领域裁定 + 21 号载荷源
    FireDiagnostics fireDiag;             // 判定链诊断快照（适配层判定行专用·永不入 wire·2026-11-04 拆分）
    // —— ep07 ——
    std::uint32_t killVictim = 0, killKiller = 0;      // Kill（23 号）
    std::uint16_t respawnDelayMs = 0;                  // Kill（倒计时下发·ep08 进数值表零改动）
    std::uint32_t respawnPid = 0;                      // Respawn（24 号）
    std::uint8_t spawnIndex = 0;                       // Respawn（落点序·比分/回放语义）
    std::uint16_t invulnMs = 0;                        // Respawn（无敌窗）
    float respawnX = 0.f, respawnY = 0.f, respawnZ = 0.f, respawnYawDeg = 0.f;  // v4.5：权威落位坐标（客户端传送同源）
    std::uint8_t endReason = 0;                        // Ended（1=击杀上限 2=真人全离）
    std::vector<ScoreRow> scores;                      // Ended（结算比分）
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
    void SetShooterRtt(std::uint32_t playerId, std::uint16_t rttMs);   // ep06：22 号 RTT 上报（回溯量源）
    void UpdateEntity(std::uint32_t playerId, float x, float y, float z,
                      float yaw, float pitch, std::uint8_t flags, std::uint32_t seq = 0,
                      std::uint64_t recvMs = 0);   // ep06：接收时刻（单钟·历史环时间戳）

    /** 每帧驱动 tick 调用（ep07：时间注入——快照+死亡倒计时+重生+结束判定统一在此）：
     *  在局则返回 Snapshot/Kill/Respawn/Ended 事件（FrameWriter 按接收者翻译） */
    std::vector<MatchEvent> Tick(std::uint64_t nowMs);

    /** ep07：终局中止（最后真人离开❗12——立即停用+清场，无事件） */
    void AbortMatch();

    /** ep07：只读访问（BotBrain 感知/驱动器组装视图用——域内只读，无绕过写口） */
    const std::vector<Entity>& Entities() const { return entities_; }
    const MapGeometry& Geometry() const { return geometry_; }
    // 活动区推导界（适配层数据校验告警用——出生点在界外=烘焙数据问题）
    float PlayMinX() const { return playMinX_; }
    float PlayMaxX() const { return playMaxX_; }
    float PlayMinY() const { return playMinY_; }
    float PlayMaxY() const { return playMaxY_; }
    bool IsEnded() const { return bEnded_; }
    std::uint32_t RoomId() const { return roomId_; }   // 常驻单局所属房间（人机驱动自愈只中止本房对局）
    const Entity* EntityById(std::uint32_t pid) const;

    // —— M5：开火裁决（D5 系列；几何合同=构建期烘焙注入，拒绝运行时上报）——
    /** 装配注入：main 启动时经 MapGeometryLoader 读几何合同后调用（缺失=空表，兼容无图环境） */
    void SetGeometry(const MapGeometry& geometry);

    /** 20 号开火裁决（唯一入口）。①在对局（未在局/未上报=丢）→ ②③④ 武器门（fire_judge 纯函数，
     *  任一不过=拒收回传——21 号 v4.6 gate 字节，客户端撤销预测扣弹）→ ⑤ hitscan 求交（先遮挡
     *  原语后实体圆柱，取最近 t；judgeAtMs 预留=ep06 回溯只换判定时刻）→ ⑥ 扣血钳 0 →
     *  ⑦ 扣弹+空匣即上钟（ConsumeAmmo——2026-11-05 死窗根除）→ ⑧ FireResult 全房间广播事件。
     *  nowMs 由适配层注入（steady 时钟，域不持钟） */
    std::vector<MatchEvent> Fire(std::uint32_t shooterPid, std::uint64_t nowMs);

    /** 自主换弹（2026-11-05 用户令：完整对局需可自主换弹·29 号）：换弹钟权威在服务器——客户端
     *  R 键只发意图，本端校验后上钟；到期补满仍由 ConsumeWeaponGate 惰性完成（换弹中→弹匣空→
     *  满匣一条状态机不打断）。静默布尔（无效=false，调用方落日志）——无事件无广播（远端换弹
     *  表现 ep14） */
    bool Reload(std::uint32_t playerId, std::uint64_t nowMs);

private:
    std::vector<Entity> entities_;         // 在局实体（名册序；StartMatch 建，RemovePlayer 删）
    // 重生选点 RNG（2026-11-04 用户令：合同出生点均匀随机，替代 D7-03 离敌最远——可预期性差+
    // 摊开后跳变尺度大；2s 无敌兜底出生保护）。种子一次，跨局复用不影响均匀性。
    std::mt19937 rng_{std::random_device{}()};
    std::uint32_t roomId_ = 0;
    bool bActive_ = false;
    MapGeometry geometry_;                 // 几何合同（构建期烘焙注入；缺省=空遮挡+默认胶囊 40/90）
    // 活动区边界（2026-11-04 审计 A 级升级：从合同**地板盒**推导，不再拍常量——地板=足印最大的
    // 遮挡盒（hx*hy 最大）；多地图/挪出生点后规则随数据走。出生点在界外时落位钳入+启动告警，
    // 数据修正在烘焙端红字拦（与"掩体压出生点"同纪律：数据问题数据入口拦）。
    float playMinX_ = -kPlayAreaHalfUu, playMaxX_ = kPlayAreaHalfUu;
    float playMinY_ = -kPlayAreaHalfUu, playMaxY_ = kPlayAreaHalfUu;
    bool bEnded_ = false;                  // ep07：终局置位（Fire/UpdateEntity 静默丢）
    std::uint8_t endReason_ = 0;
    std::uint64_t snapshotStopAtMs = 0;    // ep07：Ended 后保留快照 2s（结算面板背景）再停
    Entity* FindEntity(std::uint32_t pid);             // 内部查找（非 const 版复用）
};

} // namespace sightline::match
