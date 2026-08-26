// tests/domain/test_room.cpp —— 房间状态机与对战规则单测（零网络、零 IO）

#include <cstdio>
#include "domain/room.h"

using namespace sightline::domain;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

static Room makeRoomWith2Players() {
    RoomRules rules;   // 默认：2人开局、100血、25伤害、3杀获胜
    Room r(1, rules);
    Player a; a.id = 1; a.name = "A";
    Player b; b.id = 2; b.name = "B";
    r.addPlayer(a);
    r.addPlayer(b);
    return r;
}

int main() {
    // 1. 状态机：Waiting 未满员不开战
    {
        Room r(1, RoomRules{});
        Player a; a.id = 1;
        r.addPlayer(a);
        CHECK(r.state() == RoomState::Waiting);
        CHECK(!r.tryStart());
    }

    // 2. 两人成局
    {
        Room r = makeRoomWith2Players();
        CHECK(r.tryStart());
        CHECK(r.state() == RoomState::Playing);
        CHECK(!r.tryStart());   // 不可重复开局
    }

    // 3. Waiting 阶段禁止移动/开火
    {
        Room r(1, RoomRules{});
        Player a; a.id = 1;
        r.addPlayer(a);
        CHECK(!r.applyMove(1, Vec3{1, 0, 0}, 0.f));
        auto fr = r.applyFire(1, Vec3{0, 0, 0}, Vec3{1, 0, 0});
        CHECK(!fr.valid);
    }

    // 4. 开火命中链路：4 枪击杀；死者不能继续挨打，重生恢复
    {
        Room r = makeRoomWith2Players();
        r.tryStart();

        // 把 B 摆到 A 的正前方 10 米（A 出生点在 (18,0,0)）
        auto placeBInFrontOfA = [&r]() {
            const Player* a = r.findPlayer(1);
            Player* b = r.findPlayer(2);
            b->position = a->position + Vec3{10, 0, 0};
        };
        placeBInFrontOfA();

        auto shot = [&]() {
            const Player* a = r.findPlayer(1);
            return r.applyFire(1, a->position, Vec3{1, 0, 0});
        };

        auto s1 = shot();
        CHECK(s1.valid && s1.hit && s1.victim == 2 && s1.victim_hp == 75);

        shot(); shot();   // 累计 3 枪后 hp=25
        auto s4 = shot(); // 第 4 枪击杀
        CHECK(s4.hit && s4.victim_dead && s4.victim_hp == 0);
        CHECK(!s4.game_over);   // 才 1 杀

        // B 死了不再成为命中候选
        auto s5 = shot();
        CHECK(!s5.hit);
        CHECK(r.respawn(2));
        CHECK(r.findPlayer(2)->hp == 100);
        CHECK(r.findPlayer(2)->alive);
    }

    // 5. 三杀获胜 → Finished（每轮重生后重新摆位）
    {
        Room r = makeRoomWith2Players();
        r.tryStart();

        for (int round = 0; round < 3; ++round) {
            Player* b = r.findPlayer(2);
            const Player* a = r.findPlayer(1);
            b->position = a->position + Vec3{10, 0, 0};
            for (int i = 0; i < 4; ++i) {
                r.applyFire(1, a->position, Vec3{1, 0, 0});
            }
            r.respawn(2);
        }
        const Player* a = r.findPlayer(1);
        CHECK(a->kills == 3);
        CHECK(r.state() == RoomState::Finished);
    }

    // 6. 对战中有人退出：最后一人判胜
    {
        Room r = makeRoomWith2Players();
        r.tryStart();
        auto out = r.removePlayer(2);
        CHECK(out.was_playing && out.game_over && out.winner == 1);
        CHECK(r.state() == RoomState::Finished);
    }

    // 7. 满员拒绝加入
    {
        RoomRules rules;
        rules.max_players = 2;
        Room r(1, rules);
        Player a; a.id = 1;
        Player b; b.id = 2;
        Player c; c.id = 3;
        CHECK(r.addPlayer(a) && r.addPlayer(b));
        CHECK(!r.addPlayer(c));
    }

    if (g_failures == 0) std::printf("test_room: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
