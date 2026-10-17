// test_lobby.cpp —— LobbyService 纯单测：不起网络，喂请求断言领域事件（整洁架构体检第 3 条）
// 编译：g++ -std=c++20 -I include tests/test_lobby.cpp src/lobby/lobby_service.cpp -o /tmp/test_lobby && /tmp/test_lobby
#include "lobby/lobby.h"

#include <iostream>
#include <string>

using namespace sightline::lobby;

static int g_checked = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checked;                                                                \
        if (!(cond)) {                                                              \
            std::cerr << "FAIL line " << __LINE__ << ": " #cond << "\n";            \
            std::cerr << "已通过 " << (g_checked - 1) << " 项\n";                    \
            return 1;                                                               \
        }                                                                           \
    } while (0)

static const Event* find(const std::vector<Event>& evs, EvType t) {
    for (const auto& e : evs) if (e.type == t) return &e;
    return nullptr;
}

static void stage(const char* name) {
    std::cout << "—— " << name << " ——（已过 " << g_checked << " 项）\n";
}

int main() {
    LobbyService s;

    stage("登录");
    s.OnConnected(1, 0);
    auto ev = s.Login(1, "alpha");
    CHECK(find(ev, EvType::LoginOk));
    CHECK(find(ev, EvType::LoginOk)->playerId == 1);
    CHECK(find(ev, EvType::RoomListPush));                 // 登录回执带首次列表快照
    CHECK(s.IsLoggedIn(1));

    ev = s.Login(2, "");                                    // 空名
    auto* f = find(ev, EvType::LoginFail);
    CHECK(f && f->code == 2);
    ev = s.Login(2, std::string(17, 'a'));                  // 超长 17B
    CHECK((f = find(ev, EvType::LoginFail)) && f->code == 2);

    stage("顶号：alpha 在 fd3 再次登录，顶掉 fd1");
    ev = s.Login(3, "alpha");
    auto* k = find(ev, EvType::Kicked);
    CHECK(k && k->fd == 1 && k->code == 1);                // 旧连接被踢
    CHECK(find(ev, EvType::LoginOk) && find(ev, EvType::LoginOk)->playerId == 2);   // 失败登录不消耗 id
    CHECK(!s.IsLoggedIn(1) && s.IsLoggedIn(3));

    stage("宽限与超时");
    s.OnConnected(9, 0);
    ev = s.SweepLoginTimeouts(59999);
    CHECK(!find(ev, EvType::Kicked));                       // 59.9s：未超时（宽限 60s）
    ev = s.SweepLoginTimeouts(60001);
    k = find(ev, EvType::Kicked);
    CHECK(k && k->fd == 9 && k->code == 2);                 // 60s+: 登录超时踢出

    stage("建房");
    ev = s.CreateRoom(4, "第一房");                          // 未登录先建房
    f = find(ev, EvType::CreateFail);
    CHECK(f && f->code == 1);
    s.OnConnected(4, 0);
    ev = s.Login(4, "beta");
    CHECK(find(ev, EvType::LoginOk));
    ev = s.CreateRoom(4, "第一房");                          // 房间名允许重名
    auto* c = find(ev, EvType::CreateOk);
    CHECK(c && c->roomId == 1);
    ev = s.Login(5, "gamma");
    CHECK(find(ev, EvType::LoginOk));
    ev = s.CreateRoom(5, "第一房");                          // 同名第二房
    c = find(ev, EvType::CreateOk);
    CHECK(c && c->roomId == 2);
    ev = s.CreateRoom(4, "另一房");                          // 已在房间再建
    f = find(ev, EvType::CreateFail);
    CHECK(f && f->code == 2);

    stage("进房与满员");
    s.OnConnected(6, 0); s.OnConnected(7, 0); s.OnConnected(8, 0);
    CHECK(find(s.Login(6, "delta"), EvType::LoginOk));
    CHECK(find(s.Login(7, "eps"), EvType::LoginOk));
    CHECK(find(s.Login(8, "zeta"), EvType::LoginOk));

    ev = s.Join(6, 999);
    f = find(ev, EvType::JoinFail);
    CHECK(f && f->code == 2);                               // 房间不存在
    ev = s.Join(6, 1);
    auto* j = find(ev, EvType::JoinOk);
    CHECK(j && j->roomId == 1 && j->count == 2);            // beta(房主)+delta
    CHECK(find(ev, EvType::RoomStatePush) && find(ev, EvType::RoomListPush));
    ev = s.Join(6, 1);
    f = find(ev, EvType::JoinFail);
    CHECK(f && f->code == 4);                               // 已在房间

    CHECK(find(s.Join(7, 1), EvType::JoinOk));              // 3 人
    CHECK(find(s.Join(8, 1), EvType::JoinOk));              // 4 人 = 满
    // 第五人 eta（gamma 已在自己建的房间 2，不能用来测满员）
    s.OnConnected(11, 0);
    CHECK(find(s.Login(11, "eta"), EvType::LoginOk));
    ev = s.Join(11, 1);
    f = find(ev, EvType::JoinFail);
    CHECK(f && f->code == 3);                               // 已满拒绝

    stage("房主转移：房主 beta(4) 离开 → 最早加入的 delta(6) 接任");
    ev = s.Leave(4);
    auto* st = find(ev, EvType::RoomStatePush);
    CHECK(st && st->hostName == "delta");                   // delta 早于 eps/zeta 加入
    CHECK(st->members.size() == 3);
    CHECK(st->members[0] == "delta" && st->members[1] == "eps" && st->members[2] == "zeta");
    CHECK(find(ev, EvType::RoomListPush));                  // 人数变了 → 大厅列表刷新

    stage("断线（在房中）= 同 Leave 路径");
    ev = s.Disconnect(6);
    st = find(ev, EvType::RoomStatePush);
    CHECK(st && st->hostName == "eps");                     // 下一位接任
    CHECK(!s.IsLoggedIn(6));

    // ---- 列表快照内容（房名/人数/房主逐字段）----
    ev = s.CreateRoom(4, "快照房A");                        // beta 再建一房 → roomId=3
    auto* ck = find(ev, EvType::CreateOk);
    CHECK(ck && ck->roomId == 3);
    ev = s.Leave(8);                                        // zeta 离开房间1 → 触发全量列表推送
    auto* lp = find(ev, EvType::RoomListPush);
    CHECK(lp && lp->rooms.size() == 3 && lp->count == 3);   // 房间1 / 房间2(gamma) / 房间3
    auto brief = [&lp](uint32_t id) -> const RoomBrief* {
        for (const auto& r : lp->rooms) if (r.roomId == id) return &r;
        return nullptr;
    };
    CHECK(brief(1) && brief(1)->roomName == "第一房" && brief(1)->memberCount == 1
          && brief(1)->hostName == "eps");
    CHECK(brief(3) && brief(3)->roomName == "快照房A" && brief(3)->memberCount == 1
          && brief(3)->hostName == "beta");
    CHECK(brief(2) && brief(2)->memberCount == 1 && brief(2)->hostName == "gamma");  // gamma 自建的房间 2

    stage("房间解散：最后一人离开 → 列表移除");
    ev = s.Leave(7);                                        // eps 离开 → 房间1 人数归 0 解散
    lp = find(ev, EvType::RoomListPush);
    CHECK(lp && lp->rooms.size() == 2);                     // 只剩 房间2(gamma) / 房间3(快照房A)
    CHECK(brief(1) == nullptr && brief(2) != nullptr && brief(3) != nullptr);

    stage("优雅关闭：ShutdownBroadcast → 在线+宽限期各得 Kicked(3)（D3-19）");
    {
        LobbyService s2;
        s2.OnConnected(21, 0);
        (void)find(s2.Login(21, "关服甲"), EvType::LoginOk);    // 在线玩家
        s2.OnConnected(22, 0);                                   // 宽限期裸连接
        const auto sd = s2.ShutdownBroadcast();
        CHECK(sd.size() == 2);
        bool hitOnline = false, hitGrace = false;
        for (const auto& e : sd) {
            CHECK(e.type == EvType::Kicked && e.code == 3);
            hitOnline |= (e.fd == 21);
            hitGrace |= (e.fd == 22);
        }
        CHECK(hitOnline && hitGrace);                            // 两类连接全覆盖
    }

    std::cout << "全部通过：" << g_checked << " 项断言 ✓（LobbyService 纯逻辑，未起网络）\n";
    return 0;
}
