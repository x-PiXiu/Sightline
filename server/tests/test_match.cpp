// test_match.cpp —— M3 对局链路纯单测：MatchService 用例 + D3-15 编排过桥（LobbyService 组合）
// 不起网络：喂查询/请求，断言事件与名册（整洁架构体检第 3 条）
// 编译：g++ -std=c++20 -I include tests/test_match.cpp src/match/match_service.cpp src/lobby/lobby_service.cpp -o /tmp/test_match && /tmp/test_match
#include "match/match_service.h"
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

    std::cout << "全部通过：" << g_checked << " 项断言 ✓（MatchService + D3-15 过桥，未起网络）\n";
    return 0;
}
