// test_match.cpp —— M3 对局链路纯单测：MatchService 用例 + D3-15 编排过桥（LobbyService 组合）
// 不起网络：喂查询/请求，断言事件与名册（整洁架构体检第 3 条）
// 编译：g++ -std=c++20 -I include tests/test_match.cpp src/match/match_service.cpp src/lobby/lobby_service.cpp -o /tmp/test_match && /tmp/test_match
#include "match/match_service.h"
#include "match/fire_judge.h"
#include "adapters/map_geometry.h"
#include "lobby/lobby.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

using namespace sightline::match;
using sightline::lobby::LobbyService;
using sightline::lobby::RoomMemberInfo;

static int g_checked = 0;
static int g_stage = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checked;                                                                    \
        if (!(cond)) {                                                                  \
            std::cout << "✗ 断言失败（第 " << g_checked << " 项）: " #cond << std::endl; \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static void stage(const std::string& name) {
    ++g_stage;
    std::cout << "—— " << name << " ——（已过 " << g_checked << " 项）" << std::endl;
}

[[maybe_unused]] static const MatchEvent* find(const std::vector<MatchEvent>& evs, MatchEvent::Type t) {
    for (const auto& e : evs) if (e.type == t) return &e;
    return nullptr;
}

int main() {
    MatchService m;

    stage("开始对局：出生点 = 名册序，全员在局");
    std::vector<RosterEntry> roster = {
        {101, "alpha", 0}, {102, "beta", 0}, {103, "gamma", 0}, {104, "delta", 0}};
    auto ev = m.StartMatch(7, roster);
    CHECK(ev.size() == 1);
    CHECK(ev[0].type == MatchEvent::Type::MatchStart && ev[0].roomId == 7);
    CHECK(ev[0].roster.size() == 4);
    CHECK(ev[0].roster[0].playerId == 101 && ev[0].roster[0].spawnIndex == 0);
    CHECK(ev[0].roster[3].playerId == 104 && ev[0].roster[3].spawnIndex == 3);
    CHECK(m.IsActive(101) && m.IsActive(104));

    stage("对局外成员离场：不在局 → 空事件");
    CHECK(m.RemovePlayer(999).empty());

    stage("在局成员离场：产出 EntityLeave（roomId 正确）");
    ev = m.RemovePlayer(102);
    CHECK(ev.size() == 1);
    CHECK(ev[0].type == MatchEvent::Type::EntityLeave);
    CHECK(ev[0].roomId == 7 && ev[0].leavePlayerId == 102);
    CHECK(!m.IsActive(102) && m.IsActive(103));
    ev = m.RemovePlayer(102);                               // 幂等：已离场再离 = 空
    CHECK(ev.empty());

    stage("重复开始：重置名册与房间");
    std::vector<RosterEntry> roster2 = {{201, "new", 0}};
    ev = m.StartMatch(9, roster2);
    CHECK(ev.size() == 1 && ev[0].roomId == 9);
    CHECK(m.IsActive(201) && !m.IsActive(101));             // 旧名册失效
    CHECK(m.RemovePlayer(101).empty());                     // 旧成员不再产生离场

    stage("M4 实体暂存与快照：UpdateEntity 入库 / TickSnapshot 全量出");
    std::vector<RosterEntry> roster3 = {{301, "甲", 0}, {302, "乙", 0}};
    ev = m.StartMatch(11, roster3);
    CHECK(ev.size() == 1 && ev[0].type == MatchEvent::Type::MatchStart);
    m.UpdateEntity(301, 100.f, 200.f, 150.f, 45.f, -10.f, 0x01);
    m.UpdateEntity(302, 300.f, 400.f, 150.f, -90.f, 20.f, 0x00);
    m.UpdateEntity(999, 1.f, 1.f, 1.f, 0.f, 0.f, 0x00);     // 不在局：静默忽略
    ev = m.TickSnapshot();
    CHECK(ev.size() == 1);
    CHECK(ev[0].type == MatchEvent::Type::Snapshot && ev[0].roomId == 11);
    CHECK(ev[0].entities.size() == 2);
    CHECK(ev[0].entities[0].playerId == 301);
    CHECK(ev[0].entities[0].x == 100.f && ev[0].entities[0].y == 200.f);
    CHECK(ev[0].entities[0].yaw == 45.f && ev[0].entities[0].pitch == -10.f);
    CHECK(ev[0].entities[0].flags == 0x01);
    CHECK(ev[0].entities[1].playerId == 302 && ev[0].entities[1].flags == 0x00);
    ev = m.RemovePlayer(301);                               // 离场后快照不再含 301
    CHECK(ev.size() == 1 && ev[0].type == MatchEvent::Type::EntityLeave);
    ev = m.TickSnapshot();
    CHECK(ev.size() == 1 && ev[0].entities.size() == 1 && ev[0].entities[0].playerId == 302);

    stage("M6 补丁：未上报实体不入快照（防 (0,0,0) 幻影出生）");
    std::vector<RosterEntry> roster4 = {{401, "甲", 0}, {402, "乙", 0}};
    ev = m.StartMatch(21, roster4);
    CHECK(ev.size() == 1);
    ev = m.TickSnapshot();                                  // 双双未上报：快照为空
    CHECK(ev.size() == 1 && ev[0].entities.empty());
    m.UpdateEntity(401, 10.f, 20.f, 30.f, 0.f, 0.f, 0x00);  // 只有甲上报
    ev = m.TickSnapshot();
    CHECK(ev.size() == 1 && ev[0].entities.size() == 1);
    CHECK(ev[0].entities[0].playerId == 401);               // 乙仍未上报：不出现
    CHECK(m.RemovePlayer(402).size() == 1);                 // 未上报者也可离场
    ev = m.TickSnapshot();
    CHECK(ev.size() == 1 && ev[0].entities.size() == 1
          && ev[0].entities[0].playerId == 401);            // 乙离场后：快照只剩甲

    stage("M3 权威推算：首报即真值 / 诚实路径恒等 / 超界钳回 / ackedSeq 只进不退");
    std::vector<RosterEntry> rosterM3 = {{501, "pred", 0}, {502, "cheat", 0}};
    ev = m.StartMatch(31, rosterM3);
    CHECK(ev.size() == 1);
    m.UpdateEntity(501, 0.f, 0.f, 100.f, 0.f, 0.f, 0x00, 1);   // 首报：权威=上报位
    m.UpdateEntity(502, 0.f, 0.f, 100.f, 0.f, 0.f, 0x00, 1);
    // 诚实路径：位移 16.5uu（≈500uu/s×33ms 拍）< 界 100uu → 权威与上报恒等
    m.UpdateEntity(501, 16.5f, 0.f, 100.f, 90.f, 0.f, 0x00, 2);
    // 超界路径：瞬移声明 400uu → 钳回界上（保方向弃模长）
    m.UpdateEntity(502, 400.f, 0.f, 100.f, 0.f, 0.f, 0x00, 2);
    ev = m.TickSnapshot();
    CHECK(ev.size() == 1 && ev[0].entities.size() == 2);
    CHECK(ev[0].entities[0].playerId == 501);
    CHECK(ev[0].entities[0].x == 16.5f && ev[0].entities[0].ackedSeq == 2);
    CHECK(ev[0].entities[0].yaw == 90.f);                    // 朝向末次上报即权威
    CHECK(ev[0].entities[1].playerId == 502);
    CHECK(std::fabs(ev[0].entities[1].x - 100.f) < 0.01f);   // 权威=100uu 界上，不是上报的 400
    CHECK(ev[0].entities[1].ackedSeq == 2);
    // 越序输入：seq 回退不改 ackedSeq；位移 3.5uu 诚实照算（权威继续跟声明轨迹）
    m.UpdateEntity(501, 20.f, 0.f, 100.f, 0.f, 0.f, 0x00, 1);
    ev = m.TickSnapshot();
    CHECK(ev[0].entities[0].ackedSeq == 2);                 // 只进不退
    CHECK(ev[0].entities[0].x == 20.f);

    stage("D3-15 过桥编排：lobby 纯查询 → match（模拟 wireMatch 组合）");
    LobbyService lobby;
    lobby.OnConnected(1, 0);
    (void)lobby.Login(1, "alpha");
    lobby.OnConnected(2, 0);
    (void)lobby.Login(2, "beta");
    lobby.OnConnected(3, 0);
    (void)lobby.Login(3, "gamma");
    CHECK(lobby.CreateRoom(1, "第一房").size() >= 1);        // alpha 建房（房主）
    CHECK(lobby.Join(2, 1).size() >= 1);
    CHECK(lobby.Join(3, 1).size() >= 1);

    uint32_t roomId = 0;
    CHECK(lobby.RoomIdByFd(2, roomId) && roomId != 0);      // fd→RoomId 翻译
    CHECK(!lobby.RoomIdByFd(99, roomId));                   // 未登录/不在房 → false
    CHECK(lobby.IsHost(1) && !lobby.IsHost(2) && !lobby.IsHost(3));   // 首位=房主
    CHECK(lobby.PlayerIdByFd(2) == 2 && lobby.PlayerIdByFd(99) == -1);

    const auto members = lobby.RoomRoster(roomId);          // 名册序 = 加入序（首位=房主）
    CHECK(members.size() == 3);
    CHECK(members[0].playerId == 1 && members[0].name == "alpha");
    CHECK(members[2].playerId == 3 && members[2].name == "gamma");

    // 组合编排：名册过桥 → StartMatch → 出生点与名册序一致
    std::vector<RosterEntry> bridge;
    for (const auto& mi : members) bridge.push_back({mi.playerId, mi.name, 0});
    ev = m.StartMatch(roomId, bridge);
    CHECK(ev[0].roster[0].spawnIndex == 0 && ev[0].roster[0].playerId == 1);
    CHECK(ev[0].roster[2].spawnIndex == 2 && ev[0].roster[2].playerId == 3);

    stage("非房主请求由适配层拦截（此处仅验用例收到的名册不受影响）");
    // （IsHost 拒绝发生在 wireMatch——用例层只接收合法输入；拦截行为在服务器联调期验证）

    // ===== 第 5 期：开火命中（D5 系列；fire_judge 纯决策头 + MatchService 编排 + 几何合同装载）=====
    stage("M5 方向重建：yaw/pitch → 单位向量（§4.1 合同公式）");
    const auto d0 = DirectionFromYawPitch(0.f, 0.f);
    CHECK(std::fabs(d0.x - 1.f) < 1e-5 && std::fabs(d0.y) < 1e-5 && std::fabs(d0.z) < 1e-5);
    const auto d90 = DirectionFromYawPitch(90.f, 0.f);
    CHECK(std::fabs(d90.x) < 1e-5 && std::fabs(d90.y - 1.f) < 1e-5);
    const auto d45 = DirectionFromYawPitch(0.f, 45.f);
    CHECK(std::fabs(d45.z - 0.70710678f) < 1e-5);

    stage("M5 武器门：首发免射速 / 50ms 拒 97ms 收（容差 D5-14）/ 换弹状态机闭环（审计修复①）");
    WeaponState w;
    CHECK(ConsumeWeaponGate(w, 1000) == FireGate::Ok);           // 首发免射速门（bHasFired=false）
    w.lastFireMs = 1000; w.bHasFired = true;                     // 记账在用例⑦——纯函数测试自账
    CHECK(ConsumeWeaponGate(w, 1050) == FireGate::RateLimited);  // 50ms < 95ms 容差 → 拒
    CHECK(ConsumeWeaponGate(w, 1097) == FireGate::Ok);           // 97ms ≥ 95ms → 收
    w.magUsed = kMagCapacity;
    CHECK(ConsumeWeaponGate(w, 2000) == FireGate::MagEmpty);     // ③ 触发换弹（reloadUntil=4500）
    CHECK(ConsumeWeaponGate(w, 4000) == FireGate::Reloading);    // ② 换弹中
    CHECK(ConsumeWeaponGate(w, 4600) == FireGate::Ok);           // ② 到期就地补满
    CHECK(w.magUsed == 0);

    stage("M5 求交：圆柱命中 / 未命中终点 / 遮挡挡弹 hitPoint=墙面");
    const Vec3 o{0, 0, 90};
    const Vec3 rayX{1, 0, 0};
    const EntityHitbox hb;
    {
        const std::vector<EntityPose> hit{{7, 500, 0, 90}};
        const auto v = CastRay(o, rayX, OccluderTable{}, hit, hb);
        CHECK(v.hit && v.victimPid == 7);
        CHECK(std::fabs(v.hitX - 460.f) < 1.0f);                 // 命中面 = 圆柱近面（500-40）
    }
    {
        const std::vector<EntityPose> miss{{8, 500, 200, 90}};   // 侧偏 200 > R=40
        const auto v = CastRay(o, rayX, OccluderTable{}, miss, hb);
        CHECK(!v.hit && std::fabs(v.hitX - kMaxRangeUu) < 1.0f); // 未命中终点 = Lmax 端点
    }
    {
        OccluderTable occ;
        occ.boxes.push_back({250, -80, 90, 30, 120, 90, 0.f});   // AABB 罩住射线 y=0
        const std::vector<EntityPose> behind{{9, 500, 0, 90}};
        const auto v = CastRay(o, rayX, occ, behind, hb);
        CHECK(!v.hit);                                            // 隔墙 → 未命中（穿墙防线）
        CHECK(std::fabs(v.hitX - 220.f) < 1.0f);                  // hitPoint = 墙面（近面 250-30）
    }

    stage("M5 求交：旋转 OBB 不变胖（AABB 误挡真身不挡）/ 球遮挡");
    {
        OccluderTable occ;
        occ.boxes.push_back({300, 300, 90, 200, 20, 90, 45.f});  // 旋转 45°：真身 x∈[144,456]
        const Vec3 rayY{0, 1, 0};
        const Vec3 oX{130, 0, 90};
        const auto vMiss = CastRay(oX, rayY, occ, {}, hb);
        CHECK(std::fabs(vMiss.hitY - kMaxRangeUu) < 1.0f);       // OBB：x=130 不碰真身（AABB 会误挡）
        occ.boxes[0].yawDeg = 0.f;                               // 同中心同尺寸 AABB：x∈[100,500] 罩住射线
        const auto vHit = CastRay(oX, rayY, occ, {}, hb);
        CHECK(std::fabs(vHit.hitY - 280.f) < 1.0f);              // AABB：挡在 y=280（胖盒对比）
    }
    {
        OccluderTable occ;
        occ.spheres.push_back({300, 0, 90, 50.f});
        const auto v = CastRay(o, rayX, occ, {}, hb);
        CHECK(std::fabs(v.hitX - 250.f) < 1.0f);                 // 球面 300-50
    }

    stage("M5 开火编排：命中扣血 / 幻影枪容差 / 未命中事件 / 打空换弹恢复（审计修复①端到端）");
    MatchService fm;
    std::vector<RosterEntry> roster5{{10, "a", 0}, {11, "b", 0}, {12, "c", 0}};
    fm.StartMatch(1, roster5);
    fm.SetGeometry(MapGeometry{});                               // 空遮挡（显式注入）
    fm.UpdateEntity(10, 0, 0, 90, 0, 0, 0, 1);                   // 射手 @ 原点（首报即真值）
    fm.UpdateEntity(11, 500, 0, 90, 0, 0, 0, 1);                 // 受击者正前 5m
    CHECK(fm.Fire(12, 1000).empty());                            // ① 未上报者：静默丢
    auto fEv = fm.Fire(10, 1000);
    CHECK(fEv.size() == 1 && fEv[0].type == MatchEvent::Type::FireResult);
    CHECK(fEv[0].fire.shooterPid == 10 && fEv[0].fire.bHit && fEv[0].fire.victimPid == 11);
    CHECK(fEv[0].fire.victimHp == 75);                            // 100 - 25
    CHECK(fm.Fire(10, 1097).size() == 1);                        // 97ms 间隔：容差内接受（mag=2）
    const auto Rej = fm.Fire(10, 1147);
    CHECK(Rej.size() == 1 && !Rej[0].fire.bAccepted && !Rej[0].fire.bHit);   // 50ms：RateLimited（诊断事件回传）
    fm.UpdateEntity(10, 0, 0, 90, 90, 0, 0, 2);                  // 射手转向 +Y
    const auto fEv2 = fm.Fire(10, 1300);
    CHECK(fEv2.size() == 1 && !fEv2[0].fire.bHit && fEv2[0].fire.victimPid == 0);
    {   // 未命中 = 射线终点（眼位/方向符号化引用生产函数——眼位合同变更自动适配，2026-10-29 眼位镜像合同改造）
        const Vec3 expO = RayOriginFromEntity(0, 0, 90, 90);
        const Vec3 expD = DirectionFromYawPitch(90, 0);
        CHECK(std::fabs(fEv2[0].fire.hitX - (expO.x + expD.x * kMaxRangeUu)) < 1.0f
           && std::fabs(fEv2[0].fire.hitY - (expO.y + expD.y * kMaxRangeUu)) < 1.0f);
    }
    for (int i = 0; i <= 26; ++i) (void)fm.Fire(10, 1400 + i * 100);   // 打满剩余（mag 3+27=30）
    const auto RejMag = fm.Fire(10, 4100);
    CHECK(RejMag.size() == 1 && !RejMag[0].fire.bAccepted);      // ③ 弹匣空 → 触发换弹（诊断事件回传）
    CHECK(fm.Fire(10, 5000).size() == 1 && !fm.Fire(10, 5000).empty());   // ② 换弹中（第二次调用落在 4100+900ms 仍换弹中）
    fm.UpdateEntity(10, 0, 0, 90, 0, 0, 0, 3);                   // 射手转回 +X
    const auto fEv3 = fm.Fire(10, 6600);                          // ② 到期补满（4100+2500）→ 正常出膛
    CHECK(fEv3.size() == 1 && fEv3[0].fire.bHit && fEv3[0].fire.victimPid == 11);
    CHECK(fEv3[0].fire.victimHp == 25);                           // 前已两中（75→50）→ 50-25=25

    stage("M5 几何合同装载：ParseMapGeometry（适配层核心，纯文本可测——整洁架构体检第 3 条）");
    const std::string geo = R"({
        "map": "NewMap", "unit": "cm",
        "occluders": [ { "primitives": [ { "type": "box", "pos": [250, -80, 90], "size": [30, 120, 90] } ] } ],
        "entityHitbox": { "radius": 42, "halfHeight": 96 } })";
    const auto geoParsed = adapters::ParseMapGeometry(geo);
    CHECK(geoParsed.has_value());
    CHECK(geoParsed->occluders.boxes.size() == 1 && std::fabs(geoParsed->occluders.boxes[0].hx - 30.f) < 1e-5);
    CHECK(std::fabs(geoParsed->hitbox.radius - 42.f) < 1e-5 && std::fabs(geoParsed->hitbox.halfHeight - 96.f) < 1e-5);
    CHECK(!adapters::ParseMapGeometry("{ not json").has_value());          // 非法 JSON → nullopt（fail-fast 源头）
    CHECK(!adapters::ParseMapGeometry("{\"unit\":\"m\"}").has_value()); // 单位合同只认 cm

    // —— ep06 延迟补偿回溯：历史环/插值/回溯翻转/钳制（单钟·非零中心回归）——
    stage("ep06 历史环与回溯判定：插值/回溯翻转/钳制/非零中心 Z 回归");
    {
        MatchService hm;
        std::vector<RosterEntry> rs{{20, "a", 0}, {21, "b", 0}};
        hm.StartMatch(1, rs);
        hm.SetGeometry(MapGeometry{});
        // 目标(pid20)沿 +Y 匀速横穿并出界：t=1000 @ (500,-40,90) → t=2000 @ (500,+80,90)
        // 射手(pid21) @ (420,0,90) yaw=0（+X）；眼位 (440,8,153)（相机镜像合同 20/8/63）
        hm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 1, 1000);
        hm.UpdateEntity(20, 500, -40, 90, 180, 0, 0, 1, 1000);
        hm.UpdateEntity(20, 500, 80, 90, 180, 0, 0, 2, 2000);   // t=2000 横穿完成（y=+80，已出界）
        hm.SetShooterRtt(21, 300);
        // 插值命中：fire@1500 → judgeAt=1500 → 插值 y=+20（横向 |20-8|=12<r=34）→ HIT
        hm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 3, 1501);
        auto f1 = hm.Fire(21, 1500);
        CHECK(f1.size() == 1 && f1[0].fire.bHit && f1[0].fire.victimPid == 20);   // 过去在射线上=命中
        // 钳最新出界：fire@2500 → judgeAt=2500 ≥ 最新采样 → 钳 (500,+80) → 横向 72>34 → MISS（冻结语义）
        hm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 4, 2500);
        auto f2 = hm.Fire(21, 2500);
        CHECK(f2.size() == 1 && !f2[0].fire.bHit);
        // 上限钳制：rtt=2000 > 1000 → judgeAt 钳到 now−1000（不越窗）
        hm.SetShooterRtt(21, 2000);
        hm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 5, 3500);
        auto f3 = hm.Fire(21, 3500);                                              // judgeAt=2500 → 钳最新 → MISS
        CHECK(f3.size() == 1 && !f3[0].fire.bHit);                                // 上限钳制生效（越窗不补偿）
        // Z 回归（非零中心）：射手降到 z=45（眼 z=108）水平打 z=90 目标 → HIT
        hm.UpdateEntity(20, 500, 0, 90, 180, 0, 0, 5, 4000);
        hm.UpdateEntity(21, 420, 0, 45, 0, 0, 0, 6, 4000);
        auto f4 = hm.Fire(21, 4000);
        CHECK(f4.size() == 1 && f4[0].fire.bHit);                                 // 同高水平=命中（Z 回归）
    }
    // 环形裁剪：35 条上报 → histCount=32（覆盖最老）——以判定行为验收
    {
        MatchService tm;
        std::vector<RosterEntry> rs{{20, "a", 0}, {21, "b", 0}};
        tm.StartMatch(1, rs);
        tm.SetGeometry(MapGeometry{});
        for (int i = 0; i < 35; ++i)
            tm.UpdateEntity(20, float(500 + i), 0.f, 90.f, 0.f, 0.f, 0.f,
                            std::uint32_t(i + 1), std::uint64_t(1000 + i * 33));
        tm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 1, 4600);
        auto f5 = tm.Fire(21, 4700);                                              // judgeAt=4700
        CHECK(f5.size() == 1 && f5[0].fire.bHit);                                 // 最新位在射线上=命中
    }
    // 环形裁剪：35 条上报 → histCount=32（覆盖最老）——以判定行为验收
    {
        MatchService tm;
        std::vector<RosterEntry> rs{{20, "a", 0}, {21, "b", 0}};
        tm.StartMatch(1, rs);
        tm.SetGeometry(MapGeometry{});
        for (int i = 0; i < 35; ++i)
            tm.UpdateEntity(20, float(500 + i), 0.f, 90.f, 0.f, 0.f, 0.f,
                            std::uint32_t(i + 1), std::uint64_t(1000 + i * 33));
        tm.UpdateEntity(21, 420, 0, 90, 0, 0, 0, 1, 4600);
        auto f5 = tm.Fire(21, 4700);                                              // judgeAt=4700
        CHECK(f5.size() == 1 && f5[0].fire.bHit);                                 // 最新位在射线上=命中
    }

    std::cout << "全部通过：" << g_checked << " 项断言 ✓（MatchService + D3-15 过桥，未起网络）\n";
    return 0;
}
