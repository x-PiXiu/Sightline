// test_match.cpp —— M3 对局链路纯单测：MatchSession 用例 + D3-15 编排过桥（LobbyService 组合）
// 不起网络：喂查询/请求，断言事件与名册（整洁架构体检第 3 条）
// 编译：g++ -std=c++20 -I include tests/test_match.cpp src/match/match_session.cpp src/lobby/lobby_service.cpp src/logger/logger.cpp -o /tmp/test_match && /tmp/test_match
#include "match/match_session.h"
#include "match/nav_grid.h"
#include "match/perception.h"
#include "match/bot_brain.h"
#include "match/fire_judge.h"
#include "adapters/map_geometry.h"
#include "adapters/bot_driver.h"
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
    MatchSession m;

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
    ev = m.Tick(1000);
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
    ev = m.Tick(1000);
    CHECK(ev.size() == 1 && ev[0].entities.size() == 1 && ev[0].entities[0].playerId == 302);

    stage("M6 补丁：未上报实体不入快照（防 (0,0,0) 幻影出生）");
    std::vector<RosterEntry> roster4 = {{401, "甲", 0}, {402, "乙", 0}};
    ev = m.StartMatch(21, roster4);
    CHECK(ev.size() == 1);
    ev = m.Tick(1000);                                  // 双双未上报：快照为空
    CHECK(ev.size() == 1 && ev[0].entities.empty());
    m.UpdateEntity(401, 10.f, 20.f, 30.f, 0.f, 0.f, 0x00);  // 只有甲上报
    ev = m.Tick(1000);
    CHECK(ev.size() == 1 && ev[0].entities.size() == 1);
    CHECK(ev[0].entities[0].playerId == 401);               // 乙仍未上报：不出现
    CHECK(m.RemovePlayer(402).size() == 1);                 // 未上报者也可离场
    ev = m.Tick(1000);
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
    ev = m.Tick(1000);
    CHECK(ev.size() == 1 && ev[0].entities.size() == 2);
    CHECK(ev[0].entities[0].playerId == 501);
    CHECK(ev[0].entities[0].x == 16.5f && ev[0].entities[0].ackedSeq == 2);
    CHECK(ev[0].entities[0].yaw == 90.f);                    // 朝向末次上报即权威
    CHECK(ev[0].entities[1].playerId == 502);
    CHECK(std::fabs(ev[0].entities[1].x - 100.f) < 0.01f);   // 权威=100uu 界上，不是上报的 400
    CHECK(ev[0].entities[1].ackedSeq == 2);
    // 越序输入：seq 回退不改 ackedSeq；位移 3.5uu 诚实照算（权威继续跟声明轨迹）
    m.UpdateEntity(501, 20.f, 0.f, 100.f, 0.f, 0.f, 0x00, 1);
    ev = m.Tick(1000);
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

    // ===== 第 5 期：开火命中（D5 系列；fire_judge 纯决策头 + MatchSession 编排 + 几何合同装载）=====
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
    CHECK(ConsumeWeaponGate(w, 2000) == FireGate::MagEmpty);     // ③ 卫哨（直改状态路径；常规触发已前移 ConsumeAmmo）
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
    MatchSession fm;
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
    CHECK(RejMag.size() == 1 && !RejMag[0].fire.bAccepted && RejMag[0].fire.gate == 1);   // ② 换弹中（钟已由 ConsumeAmmo 在打空瞬间上=4000+2500）
    CHECK(fm.Fire(10, 5000).size() == 1 && !fm.Fire(10, 5000).empty());   // ② 换弹中（5000 < 6500）
    fm.UpdateEntity(10, 0, 0, 90, 0, 0, 0, 3);                   // 射手转回 +X
    const auto fEv3 = fm.Fire(10, 6600);                          // ② 到期补满（6500=打空弹 4000+2500）→ 正常出膛
    CHECK(fEv3.size() == 1 && fEv3[0].fire.bHit && fEv3[0].fire.victimPid == 11);
    CHECK(fEv3[0].fire.victimHp == 25);                           // 前已两中（75→50）→ 50-25=25

    stage("死窗根除（2026-11-05 用户日志三窗口实锤）：打空瞬间钟已上（无需下一发尝试）→ 复火首枪到期即接受");
    {
        MatchSession dm;
        std::vector<RosterEntry> rosterD{{30, "a", 0}, {31, "b", 0}};
        dm.StartMatch(1, rosterD);
        dm.SetGeometry(MapGeometry{});
        dm.UpdateEntity(30, 0, 0, 90, 0, 0, 0, 1);
        dm.UpdateEntity(31, 500, 300, 90, 0, 0, 0, 1);           // 侧偏 300 > r=40：全程脱靶（Kill 事件不掺和——
                                                                 //  本 stage 只验门/弹匣记账；命中路径 e2e 已盖）
        for (int i = 0; i < 30; ++i)                             // 30 发打空（第 30 发 t=3900 消费）
            CHECK(dm.Fire(30, 1000 + i * 100).size() == 1);
        const auto* de = dm.EntityById(30);
        CHECK(de && de->weapon.magUsed == kMagCapacity);
        CHECK(de->weapon.reloadUntilMs == 3900 + kReloadMs);     // ★核心：不发第 31 发钟已上（原实现此刻=0
                                                                 //  ——复火才发现空匣再上钟=比客户端晚整个换弹窗）
        const auto r1 = dm.Fire(30, 3950);                        // 客户端补满后复火（原死窗内）
        CHECK(r1.size() == 1 && !r1[0].fire.bAccepted && r1[0].fire.gate == 1);   // 换弹中拒（钟 6400 前有效）
        const auto r2 = dm.Fire(30, 6410);                        // 钟到期+10ms 首枪 → 直接接受（无第二段拒绝）
        CHECK(r2.size() == 1 && r2[0].fire.bAccepted);
    }

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
        MatchSession hm;
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
        MatchSession tm;
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
    // 钳制时间版（ep06·2026-10-31 联调实锤）：GameThread 冻结后恢复的首条上报 seq gap=1
    // 但位移=冻结时长×速度——纯 seq 版误钳诚实玩家 → 和解回拉（用户"按 W 被往后拉"根因）。
    // bound=max(seq版, 时间版)——时间版按到达间隔放行（cap 0.6s 防瞬移）。
    {
        MatchSession cm;
        std::vector<RosterEntry> rs{{30, "a", 0}};
        cm.StartMatch(1, rs);
        cm.UpdateEntity(30, 0.f, 0.f, 100.f, 0.f, 0.f, 0x00, 1, 1000);            // 首报
        // 冻结 500ms 恢复：gap=1、位移 200uu、到达间隔 500ms → 时间版 bound=500 → 放行（不钳）
        cm.UpdateEntity(30, 200.f, 0.f, 100.f, 0.f, 0.f, 0x00, 2, 1500);
        auto ev1 = cm.Tick(1000);
        CHECK(ev1.size() == 1 && ev1[0].entities[0].x == 200.f);                  // 诚实位移原样入权威
        // 时间版 cap：间隔 2s → cap 0.6s → bound 600；位移 800uu → 钳 600 → 权威=800（防瞬移）
        cm.UpdateEntity(30, 1000.f, 0.f, 100.f, 0.f, 0.f, 0x00, 3, 3500);
        auto ev2 = cm.Tick(1000);
        CHECK(ev2.size() == 1 && std::fabs(ev2[0].entities[0].x - 800.f) < 0.01f);
        // seq 版回归：recvMs=0（旧签名路径）瞬移 400uu → 照旧钳 100
        cm.UpdateEntity(30, 1200.f, 0.f, 100.f, 0.f, 0.f, 0x00, 4);               // 无 recvMs
        auto ev3 = cm.Tick(1000);
        CHECK(ev3.size() == 1 && std::fabs(ev3[0].entities[0].x - 900.f) < 0.01f);// 800+100=界上
    }
    // 垂直分钳（2026-11-04 坠楼段复盘）：下落（dz<0）放宽到 kMaxFallSpeedUuPerSec×gapScale=400×gapScale
    // ——诚实重力长坠不再逐拍被钳（和解不再持续回拉）；上升维持原界（防飞天）；水平界不变
    {
        MatchSession vm;
        std::vector<RosterEntry> rs{{31, "a", 0}};
        vm.StartMatch(1, rs);
        vm.UpdateEntity(31, 0.f, 0.f, 2000.f, 0.f, 0.f, 0x01, 1, 1000);           // 首报：高空
        vm.UpdateEntity(31, 0.f, 0.f, 1650.f, 0.f, 0.f, 0x01, 2, 1033);           // 下落 350uu ≤ 400 界 → 放行
        auto ev1 = vm.Tick(1000);
        CHECK(ev1.size() == 1 && ev1[0].entities[0].z == 1650.f);                 // 诚实下落原样入权威
        vm.UpdateEntity(31, 0.f, 0.f, 1850.f, 0.f, 0.f, 0x01, 3, 1066);           // 上升 200uu > bound 100 → 钳 100
        auto ev2 = vm.Tick(1000);
        CHECK(ev2.size() == 1 && ev2[0].entities[0].z == 1750.f);                 // 防飞天仍在
        vm.UpdateEntity(31, 0.f, 0.f, 1150.f, 0.f, 0.f, 0x01, 4, 1099);           // 下落 600uu > 400 界 → 钳 400
        auto ev3 = vm.Tick(1000);
        CHECK(ev3.size() == 1 && ev3[0].entities[0].z == 1350.f);                 // 终端速度界生效
    }
    // 活动区钳制（2026-11-04 审计 A 级收编）：UpdateEntity 权威位 x/y 钳 ±kPlayAreaHalfUu——
    // 对全体实体生效（真人同样被钳；此前仅人机适配器钳目标=边界形同虚设）。z 不钳（坠落合法）。
    {
        MatchSession pam;
        std::vector<RosterEntry> rs{{32, "a", 0}};
        pam.StartMatch(1, rs);
        std::uint64_t now = 1000;
        std::uint32_t sq = 0;
        pam.UpdateEntity(32, 0.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++sq, now);          // 首报（界内原点）
        for (int i = 0; i < 41; ++i) {                                            // 诚实步行 41×100uu → 越东界（回退界 ±4000）
            now += 33;
            pam.UpdateEntity(32, float(100 * (i + 1)), 0.f, 92.f, 0.f, 0.f, 0x00, ++sq, now);
        }
        auto ev1 = pam.Tick(now);
        CHECK(ev1.size() == 1 && ev1[0].entities[0].x == 4000.f);                 // ★权威位钳在活动区界上
        now += 33;
        pam.UpdateEntity(32, 4100.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++sq, now);       // 继续向外顶
        auto ev2 = pam.Tick(now);
        CHECK(ev2.size() == 1 && ev2[0].entities[0].x == 4000.f);                 // 顶墙停驻（回拉由客户端尾款平滑接走）
        for (int i = 0; i < 45; ++i) {                                            // 转向南界步行 45×100uu
            now += 33;
            pam.UpdateEntity(32, 4100.f, float(-100 * (i + 1)), 92.f, 0.f, 0.f, 0x00, ++sq, now);
        }
        auto ev3 = pam.Tick(now);
        CHECK(ev3.size() == 1 && ev3[0].entities[0].x == 4000.f && ev3[0].entities[0].y == -4000.f);  // 双轴各自钳
        {  // 界内诚实移动不受影响（新实体——避免从界外瞬回被超速钳制拦截的合理行为干扰断言）
            MatchSession pbm;
            std::vector<RosterEntry> rs{{33, "b", 0}};
            pbm.StartMatch(1, rs);
            std::uint64_t now2 = 1000;
            std::uint32_t sq2 = 0;
            pbm.UpdateEntity(33, 100.f, -100.f, 92.f, 0.f, 0.f, 0x00, ++sq2, now2);
            auto ev4 = pbm.Tick(now2);
            CHECK(ev4.size() == 1 && ev4[0].entities[0].x == 100.f && ev4[0].entities[0].y == -100.f);
        }
    }
    // 活动区推导（2026-11-04 03:20 局数据冲突回归）：界=合同地板盒（足印最大）——出生点在界外
    // 时锚/落位钳入（24 号带钳后坐标=客户端传送不再被拽回），越界步行钳在推导界上
    {
        MatchSession dm3;
        MapGeometry g;
        g.occluders.boxes.push_back(OccluderBox{0, 0, -200.5f, 1500.f, 1500.f, 200.f, 0});   // 地板 ±1500
        g.occluders.boxes.push_back(OccluderBox{100, 0, 50.f, 10.f, 10.f, 50.f, 0});         // 小盒不得干扰推导
        g.spawnPoints = {SpawnPoint{1700.f, 0.f, 92.f, 0.f}};                                // 界外出生点
        dm3.SetGeometry(g);
        std::vector<RosterEntry> rs{{72, "a", 0}};
        dm3.StartMatch(5, rs);
        const auto* pe = dm3.EntityById(72);
        CHECK(pe && pe->spawnX == 1500.f && pe->x == 1500.f);      // ★锚/落位钳入推导活动区
        dm3.UpdateEntity(72, 1700.f, 0.f, 92.f, 0.f, 0.f, 0, 1, 1000);   // 客户端仍报界外旧位
        CHECK(dm3.EntityById(72)->rebasePending);                  // 锚定门拒（钳后锚 1500 的 200cm 外）
        dm3.UpdateEntity(72, 1480.f, 0.f, 92.f, 0.f, 0.f, 0, 2, 1030);   // 落钳后锚旁 → 放行
        const auto* pa = dm3.EntityById(72);
        CHECK(pa && !pa->rebasePending && pa->x == 1480.f);        // 权威=上报位（界内）
        dm3.UpdateEntity(72, 1650.f, 0.f, 92.f, 0.f, 0.f, 0, 3, 1060);   // 越推导界
        CHECK(dm3.EntityById(72)->x == 1500.f);                    // ★钳在推导界上（非常量 2000）
    }

    // —— ep07 死亡/重生/结算/导航/感知/人机脑 ——
    stage("ep07 死亡状态机：4 枪击杀/Kill 事件/死亡门/死者不入候选/重生满血/无敌/历史清/握手");
    {
        MatchSession dm;
        std::vector<RosterEntry> rs{{40, "s", 0}, {41, "v", 1}};
        dm.StartMatch(1, rs);
        dm.SetGeometry(MapGeometry{});
        dm.UpdateEntity(40, 0, 0, 90, 0, 0, 0, 1, 1000);
        dm.UpdateEntity(41, 200, 0, 90, 180, 0, 0, 1, 1000);
        std::uint64_t now = 1000;
        std::uint64_t lastFireAt = 0;
        bool sawKill = false;
        for (int i = 0; i < 4; ++i) {
            now += 200;
            dm.UpdateEntity(40, 0, 0, 90, 0, 0, 0, static_cast<std::uint32_t>(2 + i), now);
            lastFireAt = now + 10;
            auto ev = dm.Fire(40, now + 10);
            bool fired = false;
            for (const auto& e : ev) {
                if (e.type == MatchEvent::Type::Kill) {
                    sawKill = true;
                    CHECK(e.killVictim == 41 && e.killKiller == 40 && e.respawnDelayMs == 3000);
                }
                if (e.type == MatchEvent::Type::FireResult) fired = true;
            }
            CHECK(fired);
        }
        CHECK(sawKill);
        auto d40 = dm.EntityById(40), d41 = dm.EntityById(41);
        CHECK(d40 && d41);
        CHECK(d41->bDead && d41->deaths == 1 && d40->kills == 1);
        CHECK(d41->respawnAtMs == lastFireAt + 3000);   // Kill 时刻 = 第 4 发 Fire 时刻
        // 死者开火：静默拒绝（FireResult bAccepted=false）
        now += 100;
        dm.UpdateEntity(41, 200, 0, 90, 180, 0, 0, 2, now);
        auto dead = dm.Fire(41, now);
        CHECK(dead.size() == 1 && !dead[0].fire.bAccepted);
        // 死者不入候选：40 打 41 原位 → miss
        now += 100;
        dm.UpdateEntity(40, 0, 0, 90, 0, 0, 0, 8, now);
        auto miss = dm.Fire(40, now);
        CHECK(miss.size() == 1 && miss[0].fire.bHit == false);
        // 重生倒计时：Tick 到期 → Respawn（无合同点=原位复活回退路径）
        now += 4000;
        auto evr = dm.Tick(now);
        bool sawRespawn = false;
        for (const auto& e : evr) {
            if (e.type == MatchEvent::Type::Respawn) {
                sawRespawn = true;
                CHECK(e.respawnPid == 41 && e.invulnMs == 2000);
            }
        }
        CHECK(sawRespawn);
        d41 = dm.EntityById(41);
        CHECK(d41 && !d41->bDead && d41->weapon.hp == 100.f && d41->invulnUntilMs > now);
        CHECK(d41->histCount == 0 && d41->bornAtMs == now);
        // 无敌期不被命中（候选过滤）
        auto inv = dm.Fire(40, now);
        CHECK(inv.size() == 1 && inv[0].fire.bHit == false);
        // 重生握手：出生点附近首条放行（原位复活=锚=死亡位）
        now += 100;
        dm.UpdateEntity(41, 200, 0, 90, 180, 0, 0, 3, now);
        auto d41b = dm.EntityById(41);
        CHECK(d41b && !d41b->rebasePending);
    }
    stage("ep07 重生握手回归：在途旧报只拒不改基准 → 放行首拍权威位=上报位（拉扯第五轮·无过冲）");
    {
        MatchSession rk;
        MapGeometry g;
        g.spawnPoints = {SpawnPoint{0, 0, 92, 0}, SpawnPoint{400, 0, 92, 180}, SpawnPoint{-400, 0, 92, 0}};
        rk.SetGeometry(g);
        std::vector<RosterEntry> rs{{60, "h", 0}, {61, "v", 1}};
        rk.StartMatch(3, rs);
        rk.UpdateEntity(60, 10, 0, 92, 0, 0, 0, 1, 1000);          // 60 锚点旁激活（狙击位）
        rk.UpdateEntity(61, 400, 0, 92, 180, 0, 0, 1, 1000);       // 61 锚点旁激活（靶位）
        std::uint64_t now = 1000;
        bool killed = false;
        for (int i = 0; i < 4; ++i) {                              // 4 发×25 伤 → 击杀
            now += 200;
            rk.UpdateEntity(60, 10, 0, 92, 0, 0, 0, static_cast<std::uint32_t>(2 + i), now);
            auto ev = rk.Fire(60, now + 10);
            for (const auto& e : ev)
                if (e.type == MatchEvent::Type::Kill) killed = true;
        }
        CHECK(killed);
        now += 100;
        rk.UpdateEntity(61, 400, 0, 92, 180, 0, 0, 9, now);        // 死亡期上报：bDead 早退（既有行为）
        now += 4000;
        auto rev = rk.Tick(now);                                   // 到期重生 → 离敌最远 = spawnPoints[2]
        const auto* rr = find(rev, MatchEvent::Type::Respawn);
        CHECK(rr != nullptr && rr->respawnPid == 61);
        const auto* rb = rk.EntityById(61);
        CHECK(rb && !rb->bDead && rb->rebasePending);
        const float sx = rb->x, sy = rb->y;
        // 落位∈合同出生点（2026-11-04 用户令：选点改均匀随机——测试只验合同约束不验策略）
        CHECK((sx == 0.f && sy == 0.f) || (sx == 400.f && sy == 0.f) || (sx == -400.f && sy == 0.f));
        now += 33;
        // 在途旧报位置远离全部合同点（选点已改均匀随机——旧报恰好落在新锚上会被握手放行）
        rk.UpdateEntity(61, 2000, 2000, 92, 180, 0, 0, 10, now);   // 重生后在途旧报 → 拒绝
        const auto* rj = rk.EntityById(61);
        CHECK(rj && rj->rebasePending);                            // 仍在握手期
        CHECK(rj->lx == sx && rj->ly == sy);                       // ★增量基准保持 spawn（修复前被 :104 污染回尸体位）
        now += 33;
        rk.UpdateEntity(61, sx + 40.f, sy, 92, 0, 0, 0, 11, now);  // 传送后首条：锚点旁 40cm → 放行
        const auto* rp = rk.EntityById(61);
        CHECK(rp && !rp->rebasePending);
        CHECK(rp->x == sx + 40.f && rp->y == sy && rp->z == 92.f); // ★权威位逐分量=上报位（修复前 300~600uu 过冲+追爬）
    }
    stage("自主换弹（29 号语义）：半匣上钟/换弹中火拒/重复换弹拒/到期满匣续射/未激活拒");
    {
        MatchSession sm;
        std::vector<RosterEntry> rs{{90, "a", 0}};
        sm.StartMatch(7, rs);
        CHECK(sm.Reload(90, 1000) == false);                   // 未激活（未上报）→ 拒
        sm.UpdateEntity(90, 0, 0, 92, 0, 0, 0, 1, 1000);
        std::uint64_t now = 1000;
        std::uint32_t q = 1;
        for (int i = 0; i < 10; ++i) {                         // 打 10 发（magUsed=10）
            now += 150;
            sm.UpdateEntity(90, 0, 0, 92, 0, 0, 0, ++q, now);
            (void)sm.Fire(90, now + 5);
        }
        CHECK(sm.Reload(90, now) == true);                     // ★半匣自主换弹 ✓ 上钟
        now += 100;
        sm.UpdateEntity(90, 0, 0, 92, 0, 0, 0, ++q, now);
        auto ev = sm.Fire(90, now + 5);
        CHECK(ev.size() == 1 && ev[0].fire.bAccepted == false && ev[0].fire.gate == 1);   // 换弹中火拒（FireGate::Reload=1）
        CHECK(sm.Reload(90, now + 50) == false);               // 重复换弹拒
        now += 2600;                                           // 越过换弹窗
        sm.UpdateEntity(90, 0, 0, 92, 0, 0, 0, ++q, now);
        auto ev2 = sm.Fire(90, now + 5);
        CHECK(ev2.size() == 1 && ev2[0].fire.bAccepted == true);   // ★到期满匣续射（gate 惰性补满生效）
    }
    stage("ep07 重生选点排除已占用：活人占 A 旁 40cm→死者必落 B；两点全占→回退仍能重生");
    {
        MatchSession om;
        MapGeometry g;
        g.spawnPoints = {SpawnPoint{0, 0, 92, 0}, SpawnPoint{800, 0, 92, 180}};
        om.SetGeometry(g);
        std::vector<RosterEntry> rs{{80, "h", 0}, {81, "v", 1}};
        om.StartMatch(6, rs);
        om.UpdateEntity(80, 40, 0, 92, 0, 0, 0, 1, 1000);      // 活人贴 A 点 40cm（<150 → A 排除）
        om.UpdateEntity(81, 800, 0, 92, 180, 0, 0, 1, 1000);   // 靶位 B（唯一自由候选）
        std::uint64_t now = 1000;
        bool killed = false;
        for (int i = 0; i < 4; ++i) {                          // 4 发×25 → 击杀
            now += 200;
            om.UpdateEntity(80, 40, 0, 92, 0, 0, 0, static_cast<std::uint32_t>(2 + i), now);
            auto ev = om.Fire(80, now + 10);
            for (const auto& e : ev)
                if (e.type == MatchEvent::Type::Kill) killed = true;
        }
        CHECK(killed);
        now += 4000;
        auto rev = om.Tick(now);                               // 到期重生：B 被死者占用（死者不入扫描）→
        const auto* rr = find(rev, MatchEvent::Type::Respawn); //  A 被活人排斥 → 唯一候选=B（确定性）
        CHECK(rr != nullptr && rr->respawnX == 800.f && rr->respawnY == 0.f);
        // 全占回退：活人挪到 B 旁 40cm → A/B 双占 → 回退全员随机，仍必须重生
        now += 100;
        for (int i = 0; i < 8; ++i) {                          // 80 从 40 步行到 760（8×100uu 贴界）
            now += 33;
            om.UpdateEntity(80, float(40 + 90 * (i + 1)), 0.f, 92.f, 0.f, 0.f, 0x00, static_cast<std::uint32_t>(20 + i), now);
        }
        const auto* w80 = om.EntityById(80);
        CHECK(w80 && w80->x == 760.f);                         // 界内诚实移动原样入权威
        now += 2500;                                           // 越过 81 重生无敌窗（开火即破仅豁免射手——
                                                               //  靶侧无敌须等表走完才能再被击杀）
        om.UpdateEntity(81, 800.f, 0.f, 92.f, 180.f, 0.f, 0.f, 100, now);   // 81 重生后上报（真实世界客户端/
                                                               //  人机驱动 30Hz 必报——否则历史环空=回溯采样空=永 miss）
        om.UpdateEntity(80, 760.f, 0.f, 92.f, 0.f, 0.f, 0x00, 40, now);
        bool killed2 = false;
        for (int i = 0; i < 4; ++i) {                          // 再杀 81（此时 81 已在 B 重生过、活体）
            now += 200;
            om.UpdateEntity(80, 760.f, 0.f, 92.f, 0.f, 0.f, 0x00, static_cast<std::uint32_t>(41 + i), now);
            auto ev = om.Fire(80, now + 10);
            for (const auto& e : ev)
                if (e.type == MatchEvent::Type::Kill) killed2 = killed2 || e.killVictim == 81;
        }
        CHECK(killed2);
        now += 4000;
        auto rev2 = om.Tick(now);                              // A 距 760>150 自由？A(0,0) 距 760 → 自由；
        const auto* rr2 = find(rev2, MatchEvent::Type::Respawn);// B(800,0) 距 760 点 40 → 排斥 → 必落 A
        CHECK(rr2 != nullptr && rr2->respawnX == 0.f && rr2->respawnY == 0.f);
    }
    stage("ep07 无敌诊断：重生 2s 保护内完美瞄准=miss+最近候选[无敌]；保护过期=命中（选点随机下射手恒居西）");
    {
        MatchSession iv;
        MapGeometry g;
        g.spawnPoints = {SpawnPoint{-300.f, 0.f, 92.f, 0.f}, SpawnPoint{500.f, 0.f, 92.f, 180.f}};
        iv.SetGeometry(g);
        std::vector<RosterEntry> rs{{70, "h", 0}, {71, "v", 1}};
        iv.StartMatch(4, rs);
        iv.UpdateEntity(70, -290.f, 0.f, 92.f, 0.f, 0.f, 0, 1, 1000);   // 射手锚旁激活
        iv.UpdateEntity(71, 500.f, 0.f, 92.f, 180.f, 0.f, 0.f, 1, 1000);// 靶位激活
        std::uint64_t now = 1000;
        std::uint32_t hseq = 1;
        for (int i = 0; i < 4; ++i) {                          // 射手撤到 −700：两个候选重生点都在正前方
            now += 33;
            iv.UpdateEntity(70, float(-290 - 100 * (i + 1)), 0.f, 92.f, 0.f, 0.f, 0x00, ++hseq, now);
        }
        now += 33;
        iv.UpdateEntity(70, -690.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++hseq, now);
        bool killed = false;
        for (int i = 0; i < 4; ++i) {                          // 4 发×25 → 击杀（yaw0：射线过靶心旁 8cm）
            now += 200;
            iv.UpdateEntity(70, -690.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++hseq, now);
            auto ev = iv.Fire(70, now + 10);
            for (const auto& e : ev)
                if (e.type == MatchEvent::Type::Kill) killed = true;
        }
        CHECK(killed);
        now += 4000;
        auto rev = iv.Tick(now);                               // 到期重生：均匀随机二选一
        const auto* rv = find(rev, MatchEvent::Type::Respawn);
        CHECK(rv != nullptr);
        const float rx = rv->respawnX, ryv = rv->respawnY;     // 以实际落位为准
        CHECK((rx == -300.f && ryv == 0.f) || (rx == 500.f && ryv == 0.f));
        now += 100;
        iv.UpdateEntity(71, rx, ryv, 92.f, 180.f, 0.f, 0.f, 9, now);   // 重生后首报（激活+入历史环）
        now += 100;                                            // 无敌窗内（respawn+200 < +2000）
        iv.UpdateEntity(70, -690.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++hseq, now);
        auto miss = iv.Fire(70, now + 10);                     // 完美瞄准：yaw0 射线正穿重生点旁 8cm
        CHECK(miss.size() == 1 && !miss[0].fire.bHit);         // 判定语义不变：无敌不可命中
        CHECK(miss[0].fireDiag.nearPid == 71);                 // ★诊断可见：最近候选=无敌目标
        CHECK(miss[0].fireDiag.nearInvuln);                    // ★[无敌] 旗置位（原=整只剔除零反馈）
        CHECK(miss[0].fireDiag.nearPerp < 70.f);               // 垂距≈63.5（含眼高 63cm 差，点到射线）
        now += 2200;                                           // 越过无敌窗
        iv.UpdateEntity(70, -690.f, 0.f, 92.f, 0.f, 0.f, 0x00, ++hseq, now);
        auto hit = iv.Fire(70, now + 10);
        CHECK(hit.size() == 1 && hit[0].fire.bHit && hit[0].fire.victimPid == 71);   // 保护过期→命中
        now += 200;
        iv.UpdateEntity(70, -690.f, 0.f, 92.f, -60.f, 0.f, 0.f, ++hseq, now);  // 偏 60° 打偏
        auto miss2 = iv.Fire(70, now + 10);
        CHECK(miss2.size() == 1 && !miss2[0].fire.bHit);
        CHECK(miss2[0].fireDiag.nearPid == 71 && !miss2[0].fireDiag.nearInvuln);   // 同款 miss 不带旗
    }
    stage("ep07 终局：10 杀 Ended（原因 1·比分两行）/终局静默/AbortMatch");
    {
        MatchSession em;
        std::vector<RosterEntry> rs{{50, "a", 0}, {51, "b", 1}};
        em.StartMatch(2, rs);
        em.SetGeometry(MapGeometry{});
        std::uint64_t now = 1000;
        std::uint32_t seqA = 1, seqB = 1;
        bool sawEnded = false;
        for (int round = 0; round < 12; ++round) {
            now += 5000;
            auto tev = em.Tick(now);                 // Ended 由 Tick 统一产出（Fire 只置位）
            for (const auto& e : tev)
                if (e.type == MatchEvent::Type::Ended) {
                    sawEnded = true;
                    CHECK(e.endReason == 1 && e.scores.size() == 2);
                }
            now += 2100;
            em.UpdateEntity(50, 0, 0, 90, 0, 0, 0, ++seqA, now);
            em.UpdateEntity(51, 200, 0, 90, 180, 0, 0, ++seqB, now);
            for (int i = 0; i < 4; ++i) {
                now += 150;
                em.UpdateEntity(50, 0, 0, 90, 0, 0, 0, ++seqA, now);
                auto ev = em.Fire(50, now);
                for (const auto& e : ev)
                    if (e.type == MatchEvent::Type::Ended) {
                        sawEnded = true;
                        CHECK(e.endReason == 1 && e.scores.size() == 2);
                    }
            }
            if (sawEnded) break;
        }
        CHECK(sawEnded);
        CHECK(em.IsEnded());
        now += 100;
        em.UpdateEntity(50, 0, 0, 90, 0, 0, 0, ++seqA, now);
        auto after = em.Fire(50, now);
        CHECK(after.size() == 1 && !after[0].fire.bAccepted);
        em.AbortMatch();
        CHECK(!em.IsActive(50));
    }
    stage("ep07 导航栅格：地板排除/墙 blocked/绕墙路径/围死不可达");
    {
        OccluderTable occ;
        OccluderBox floor; floor.cx = 0; floor.cy = 0; floor.cz = -200.5f;
        floor.hx = 2000; floor.hy = 2000; floor.hz = 200;
        occ.boxes.push_back(floor);
        OccluderBox wall; wall.cx = 1507; wall.cy = -777; wall.cz = 49;
        wall.hx = 25; wall.hy = 206; wall.hz = 212.5;
        occ.boxes.push_back(wall);
        const NavGrid g = BuildNavGrid(occ);
        CHECK(!g.Blocked(g.CellIx(0.f), g.CellIy(0.f)));
        CHECK(g.Blocked(g.CellIx(1507.f), g.CellIy(-777.f)));
        auto path = FindPath(g, 0.f, -777.f, 1800.f, -777.f);
        CHECK(path.has_value() && path->size() >= 3);
        bool crossBlocked = false;
        for (const auto& p : *path)
            if (g.Blocked(g.CellIx(p.x), g.CellIy(p.y))) crossBlocked = true;
        CHECK(!crossBlocked);
        OccluderTable box;
        auto addWall = [&](float x, float y, float hx, float hy) {
            OccluderBox w2; w2.cx = x; w2.cy = y; w2.cz = 90; w2.hx = hx; w2.hy = hy; w2.hz = 180;
            box.boxes.push_back(w2);
        };
        addWall(500, 500, 200, 25); addWall(500, 900, 200, 25);
        addWall(300, 700, 25, 200); addWall(700, 700, 25, 200);
        const NavGrid g2 = BuildNavGrid(box);
        auto none = FindPath(g2, 0.f, 0.f, 500.f, 700.f);
        CHECK(!none.has_value());
    }
    stage("ep07 感知：无墙可见/墙后不可见（复用 fire_judge 求交=所见即所及）");
    {
        OccluderTable occ;
        OccluderBox wall; wall.cx = 1507; wall.cy = -777; wall.cz = 49;
        wall.hx = 25; wall.hy = 206; wall.hz = 212.5;
        occ.boxes.push_back(wall);
        CHECK(CanSee(occ, Vec3{800, -777, 90}, 0.f, Vec3{1200, -777, 150}));
        CHECK(!CanSee(occ, Vec3{800, -777, 90}, 0.f, Vec3{1800, -777, 150}));
    }
    stage("ep07 BotBrain FSM：发现→Engage 开火/背对不射/丢视线 Search/遗忘 Patrol/低血 Cover");
    {
        BotState st = BotState::Patrol;
        BotMemory m;
        BotTuning t;
        BotWorldView w;
        w.self = {0, 0, 90, 0, 100, 0, false};
        w.patrolTarget = Vec2{300, -300};
        auto i0 = Decide(w, st, m, 1000, t);
        CHECK(st == BotState::Patrol && i0.hasMove);
        w.visible = SeenEnemy{2, 500.f, 0.f, 500.f};
        auto i1 = Decide(w, st, m, 1100, t);
        CHECK(st == BotState::Engage && !i1.bFire);   // 看见那一拍只切状态（反应延迟语义）
        auto i1b = Decide(w, st, m, 1133, t);          // 下一拍：对准（aim 0±4°·delta≤6）→ 开火
        CHECK(i1b.bFire);
        w.self.yaw = 180.f;                            // 背对目标
        auto i2 = Decide(w, st, m, 1200, t);
        CHECK(!i2.bFire && !i2.hasMove);               // 不射（未对准）·不追（500<900 射程内）
        w.self.yaw = 0.f;
        w.visible.reset();
        Decide(w, st, m, 1300, t);
        CHECK(st == BotState::Search);
        Decide(w, st, m, 1300 + 3001, t);
        CHECK(st == BotState::Patrol);
        w.visible = SeenEnemy{2, 500.f, 0.f, 500.f};
        w.self.hp = 20.f;
        auto i3 = Decide(w, st, m, 1300 + 3100, t);
        CHECK(st == BotState::TakeCover && i3.hasMove);   // 受击（100→20 新伤害）→ 脱战窗 1.5s
        auto i4 = Decide(w, st, m, 1300 + 3100 + 1500, t);// 窗满 → 无条件反击（不再无限后撤·用户实锤）
        CHECK(st == BotState::Engage);
        auto i5 = Decide(w, st, m, 1300 + 3100 + 1550, t);// 下一拍：血没再降 → 不重入 Cover（反振荡）
        CHECK(st == BotState::Engage);
    }

    // ── 2026-11-03 联调回归：同一服务进程连开多局（用户现场"开枪无回包"根因=终局字段跨局泄漏）──
    // 旧测试每段 new 一个 MatchSession，从没测过"上一局结束后同实例再开局"——bug 就藏在这道缝里
    stage("ep07 跨局复位①：AbortMatch 后同实例再开局——首报激活/开火必有判定/不得开局即结算");
    {
        MatchSession xm;
        xm.SetGeometry(MapGeometry{});
        std::vector<RosterEntry> rs{{60, "h", 0}, {61, "b", 0}};
        xm.StartMatch(3, rs);
        xm.UpdateEntity(60, 442, -474, 90, 133, 0, 0, 1, 1000);
        xm.AbortMatch();                                       // 房间解散自愈路径（❗12）
        xm.StartMatch(4, rs);                                  // 同一服务进程的下一局
        CHECK(find(xm.Tick(2000), MatchEvent::Type::Ended) == nullptr);   // 新局不得一开局就结算
        xm.UpdateEntity(60, 442, -474, 90, 133, 0, 0, 1, 2000);           // 真实出生点（非原点）
        const auto* e60 = xm.EntityById(60);
        CHECK(e60 && e60->bHasReport);                         // 首报必须激活实体
        CHECK(find(xm.Fire(60, 2100), MatchEvent::Type::FireResult) != nullptr);   // 开火必有判定
        CHECK(find(xm.Tick(2200), MatchEvent::Type::Snapshot) != nullptr);         // 新局持续出快照
    }
    stage("ep07 跨局复位②：击杀上限终局+快照停发后同实例再开局（用户现场形态：无结算行·开枪无回包）");
    {
        MatchSession km;
        km.SetGeometry(MapGeometry{});
        std::vector<RosterEntry> rs{{70, "a", 0}, {71, "b", 0}};
        km.StartMatch(5, rs);
        std::uint64_t now = 1000;
        std::uint32_t sa = 1, sb = 1;
        for (int round = 0; round < 12 && !km.IsEnded(); ++round) {
            now += 5000;
            km.Tick(now);                                      // 到期重生
            now += 2100;                                       // 无敌期过
            km.UpdateEntity(70, 0, 0, 90, 0, 0, 0, ++sa, now);
            km.UpdateEntity(71, 200, 0, 90, 180, 0, 0, ++sb, now);
            for (int i = 0; i < 4; ++i) {
                now += 150;
                km.UpdateEntity(70, 0, 0, 90, 0, 0, 0, ++sa, now);
                km.Fire(70, now);
            }
        }
        CHECK(km.IsEnded());
        km.Tick(now + 10);                                     // 产出 Ended（停发时刻上弦）
        km.Tick(now + 2500);                                   // 结算背景 2s 用毕 → 快照停发
        km.StartMatch(6, rs);                                  // 同一服务进程的下一局
        now += 5000;
        CHECK(find(km.Tick(now), MatchEvent::Type::Snapshot) != nullptr);   // 新局不得首拍即停用
        km.UpdateEntity(70, 442, -474, 90, 133, 0, 0, 1, now);
        const auto* e70 = km.EntityById(70);
        CHECK(e70 && e70->bHasReport && !km.IsEnded());
        CHECK(find(km.Fire(70, now + 100), MatchEvent::Type::FireResult) != nullptr);
    }
    stage("ep07 人机驱动：出生点取用例分配（不叠真人）/航点真巡逻/开火与击杀事件回传");
    {
        LobbyService lob;
        (void)lob.Login(801, "host");
        (void)lob.CreateRoom(801, "botroom");
        std::uint32_t rid = 0;
        CHECK(lob.RoomIdByFd(801, rid));
        const int botPid = lob.AddBot(rid);
        CHECK(botPid > 0);
        const auto bot = static_cast<std::uint32_t>(botPid);
        const auto host = static_cast<std::uint32_t>(lob.PlayerIdByFd(801));
        MapGeometry g;
        g.spawnPoints = {SpawnPoint{442, -474, 90, 133}, SpawnPoint{820, -820, 90, -47}};
        g.patrolPoints = {Vec3{1050, -820, 90}, Vec3{200, -1200, 90}, Vec3{1200, -200, 90}};
        // ep09：Tick 已路由化（MatchShards）——session 经容器持有（引用语义兼容后续 bm. 调用）
        MatchShards bmShards{ShardDeps{}, 1};
        MatchSession& bm = bmShards.OpenRoom(rid);
        bm.SetGeometry(g);
        adapters::BotDriver drv;
        drv.OnBotAdded(rid, bot, "Bot", 1);
        // 生产形态：适配层按房间名册组 entries（spawnIndex 全 0——名册序由用例 StartMatch 分配）
        std::vector<RosterEntry> entries{{host, "host", 0}, {bot, "Bot", 0}};
        bm.StartMatch(rid, entries);
        drv.OnMatchStart(rid, bm.Geometry(), bm, 1000);
        const auto* be = bm.EntityById(bot);
        CHECK(be && be->bHasReport);
        CHECK(be->x == 820.f && be->y == -820.f);              // 名册序 1 → spawnPoints[1]：不叠房主出生点
        bool reached = false;                                  // 真人未上报=不可见 → 人机只巡逻
        std::uint64_t now = 1000;
        for (int i = 0; i < 300 && !reached; ++i) {            // ≤10s（400cm/s·最远航点 <8m）
            (void)drv.Tick(now += 33, bmShards, lob);
            for (const auto& p : g.patrolPoints)
                if (std::hypot(be->x - p.x, be->y - p.y) < 80.f) reached = true;
        }
        CHECK(reached);                                        // 真去航点（不是默认目标场心 (0,0)）
        // 真人现身锚点旁 30cm（开局锚定握手放行范围内激活）→ 人机开火/击杀事件必须从 Tick 返回
        const float hx = 472.f, hy = -474.f;                   // host 锚=spawnPoints[0]=(442,-474)
        std::uint32_t hseq = 0;
        bool sawShot = false, sawRejected = false, sawKill = false;
        for (int i = 0; i < 180 && !sawKill; ++i) {            // ≤6s
            now += 33;
            bm.UpdateEntity(host, hx, hy, 90, 180, 0, 0, ++hseq, now);
            for (const auto& e : drv.Tick(now, bmShards, lob)) {
                if (e.type == MatchEvent::Type::FireResult && e.fire.shooterPid == bot) {
                    sawShot = true;
                    if (!e.fire.bAccepted) sawRejected = true;
                }
                if (e.type == MatchEvent::Type::Kill && e.killKiller == bot && e.killVictim == host)
                    sawKill = true;
            }
            (void)bm.Tick(now);
        }
        CHECK(sawShot && !sawRejected);                        // 开火回传；被武器门拦的不回传（不刷日志）
        CHECK(sawKill);                                        // 击杀回传（受害客户端靠 23 号知道自己死了）
        // 开局锚定握手（2026-11-03"开局拉出地图"修复）：有合同出生点时实体未激活前，
        // 远离锚点的上报（切图前旧世界角色的 19 号）必须被忽略
        {
            MatchSession am;
            am.SetGeometry(g);
            std::vector<RosterEntry> rs{{901, "h", 0}, {902, "b", 1}};
            am.StartMatch(rid, rs);
            am.UpdateEntity(901, 1500, 300, 90, 0, 0, 0, 1, 1000);   // 远离锚点 (442,-474) 的"旧包"
            CHECK(am.EntityById(901) && !am.EntityById(901)->bHasReport);   // 被握手过滤：未激活
            const auto tickEvs = am.Tick(1100);   // 具名接收——Tick 返回临时 vector，指针跨语句
            const auto* emptySnap = find(tickEvs, MatchEvent::Type::Snapshot);   // 使用即悬空（ASan 实锤 2026-11-08）
            CHECK(emptySnap != nullptr && emptySnap->entities.empty());   // 未激活不入快照（快照为空）
            am.UpdateEntity(901, 460, -490, 90, 0, 0, 0, 2, 1200);   // 锚点附近首条 → 放行
            const auto* h901 = am.EntityById(901);
            CHECK(h901 && h901->bHasReport);
            CHECK(h901->x == 460.f && h901->y == -490.f);      // 权威位=上报位（首报即真值）
        }
    }
    stage("ep07 人机出界钳制：ClampMoveTarget 界外钳回/界内不动 + TakeCover 撤退顶界实测（Bot2 (3177,−3918) 实锤回归）");
    {
        const auto nav0 = BuildNavGrid(OccluderTable{});       // 无地板盒 → 回退栅格 ±4100
        auto out = adapters::ClampMoveTarget(nav0, {5000.f, -6000.f});
        CHECK(out.x == 4100.f && out.y == -4100.f);            // 界外 → 栅格角（地板+100 建格余量）
        auto in = adapters::ClampMoveTarget(nav0, {300.f, -300.f});
        CHECK(in.x == 300.f && in.y == -300.f);                // 界内 → 原样
        auto edge = adapters::ClampMoveTarget(nav0, {-9999.f, 42.f});
        CHECK(edge.x == -4100.f && edge.y == 42.f);            // 单轴越界只钳该轴
    }
    {
        LobbyService lob2;
        (void)lob2.Login(901, "host");
        (void)lob2.CreateRoom(901, "edge");
        std::uint32_t rid2 = 0;
        CHECK(lob2.RoomIdByFd(901, rid2));
        const int botPid2 = lob2.AddBot(rid2);
        const auto bot2 = static_cast<std::uint32_t>(botPid2);
        const auto host2 = static_cast<std::uint32_t>(lob2.PlayerIdByFd(901));
        MapGeometry g;
        g.spawnPoints = {SpawnPoint{1700, 0, 92, 0}, SpawnPoint{3900, 0, 92, 180}};   // 人机贴东缘出生（感知半径内可见玩家）
        MatchShards bm2Shards{ShardDeps{}, 1};
        MatchSession& bm2 = bm2Shards.OpenRoom(rid2);
        bm2.SetGeometry(g);
        adapters::BotDriver drv2;
        drv2.OnBotAdded(rid2, bot2, "Bot", 1);
        std::vector<RosterEntry> entries{{host2, "host", 0}, {bot2, "Bot", 0}};
        bm2.StartMatch(rid2, entries);
        drv2.OnMatchStart(rid2, bm2.Geometry(), bm2, 1000);
        std::uint64_t now = 1000;
        std::uint32_t pseq = 1;
        for (int i = 0; i < 3; ++i) {                          // 3 发×25 → 人机 hp=25（<30 触发 TakeCover）
            now += 200;
            bm2.UpdateEntity(host2, 1710, 0, 92, 0, 0, 0, ++pseq, now);
            (void)bm2.Fire(host2, now);
            (void)drv2.Tick(now, bm2Shards, lob2);
        }
        const auto* bb = bm2.EntityById(bot2);
        CHECK(bb && bb->weapon.hp == 25.f);                    // 残血：撤退前提成立
        for (int i = 0; i < 200; ++i) {                        // ~6.6s：背敌撤退 400cm/s 顶向栅格界（±4100）
            now += 33;
            (void)drv2.Tick(now, bm2Shards, lob2);
        }
        bb = bm2.EntityById(bot2);
        CHECK(bb && bb->x <= 4000.01f);                        // ★不越过活动区（域层钳·意图层已被钳到栅格界 4100）
        CHECK(bb && bb->y == 0.f);                             // 沿轴撤退无漂移
    }
    stage("ep07 合同自检：出生点被掩体压住可检出（栅格同语义）/净空放行");
    {
        // 用户现场形态（2026-11-03）：184×128×128 箱压在出生点旁 56cm——出生格 blocked
        MapGeometry g;
        g.occluders.boxes.push_back(OccluderBox{56, 0, 92, 92, 64, 64, 0});
        g.spawnPoints = {SpawnPoint{0, 0, 92, 0}};
        CHECK(BlockedSpawnPoints(g).size() == 1);              // 压住 → 检出
        g.spawnPoints.push_back(SpawnPoint{400, -400, 91.5f, 0});
        const auto v = BlockedSpawnPoints(g);
        CHECK(v.size() == 1 && v[0] == 0);                     // 净空点放行；下标对位
        g.occluders.boxes.push_back(OccluderBox{1507, -777, 49, 25, 206.25f, 212.5f, 0});   // 长墙
        CHECK(BlockedSpawnPoints(g).size() == 1);              // 远墙不影响
        g.occluders.boxes.clear();
        CHECK(BlockedSpawnPoints(g).empty());                  // 撤箱全放行
    }

    std::cout << "全部通过：" << g_checked << " 项断言 ✓（MatchSession + D3-15 过桥，未起网络）\n";
    return 0;
}
