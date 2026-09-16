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

    // 8. P2 道具：布点/拾取/回血上限/半径/冷却重生（纯 domain 规则，零网络）
    {
        Room r = makeRoomWith2Players();
        r.tryStart();
        r.spawnItems();

        // 8.1 布点：4 个道具（东西血包 + 南北弹药箱），NetID 从 1 连续自增
        CHECK(r.items().size() == 4);
        CHECK(r.items()[0].net_id == 1 && r.items()[3].net_id == 4);
        CHECK(r.items()[0].type_id == ItemTypeId::HealthPack);
        CHECK(r.items()[2].type_id == ItemTypeId::AmmoBox);

        // 8.2 站上血包拾取：hp 100 满血捡 → 回血封顶不溢出
        auto pick = r.tryPickup(1, r.items()[0].pos, 1000.0);
        CHECK(pick.has_value());
        CHECK(pick->type_id == ItemTypeId::HealthPack && pick->picker == 1);
        CHECK(pick->picker_hp == 100);
        CHECK(r.findPlayer(1)->hp == 100);

        // 8.3 冷却中不能重复拾取
        CHECK(!r.tryPickup(2, r.items()[0].pos, 1001.0).has_value());

        // 8.4 半径外不触发：道具0 在 (-800,0)，道具1 在 (800,0)，相距 1600 > 150
        CHECK(!r.tryPickup(1, r.items()[0].pos, 1002.0).has_value());

        // 8.5 拾取半径边缘可触发（水平距离恰为半径 150）
        {
            const float pickup_r = RoomRules{}.item_pickup_radius;
            Vec3 edge = r.items()[1].pos;
            edge.x -= pickup_r;
            auto p2 = r.tryPickup(1, edge, 1003.0);
            CHECK(p2.has_value() && p2->net_id == 2);
        }

        // 8.6 残血回血：60 + 50 = 110 封顶 100
        {
            Room r3 = makeRoomWith2Players();
            r3.tryStart();
            r3.spawnItems();
            r3.findPlayer(1)->hp = 60;
            auto heal = r3.tryPickup(1, r3.items()[0].pos, 0.0);
            CHECK(heal.has_value() && heal->picker_hp == 100);
        }

        // 8.7 死人不能拾取；Waiting 状态不能拾取
        {
            Room r4 = makeRoomWith2Players();
            r4.tryStart();
            r4.spawnItems();
            r4.findPlayer(2)->alive = false;
            CHECK(!r4.tryPickup(2, r4.items()[2].pos, 0.0).has_value());

            Room r5(1, RoomRules{});
            Player a; a.id = 1;
            r5.addPlayer(a);
            r5.spawnItems();
            CHECK(!r5.tryPickup(1, r5.items()[0].pos, 0.0).has_value());
        }

        // 8.8 冷却到点重生：tickItems 返回重置道具，之后可再次拾取
        {
            RoomRules rules;
            rules.item_respawn_ms = 100.0;   // 缩短冷却便于测试
            Room r2(1, rules);
            Player a; a.id = 1; a.name = "A";
            Player b; b.id = 2; b.name = "B";
            r2.addPlayer(a); r2.addPlayer(b);
            r2.tryStart();
            r2.spawnItems();
            CHECK(r2.tryPickup(1, r2.items()[0].pos, 0.0).has_value());

            CHECK(r2.tickItems(50.0).empty());                        // 冷却未到：不重生
            auto after = r2.tickItems(150.0);                         // 到点：重生
            CHECK(after.size() == 1 && after[0].net_id == r2.items()[0].net_id);
            CHECK(!r2.items()[0].taken);
            CHECK(r2.tryPickup(2, r2.items()[0].pos, 200.0).has_value());   // 又能捡了
        }
    }

    // 9. P2 顺带修复：出生点半径 1800（18 米），不再是 18cm
    {
        Room r(1, RoomRules{});
        Vec3 sp = r.spawnPoint(0);
        CHECK(sp.x > 1700.f && sp.x < 1900.f);
        CHECK(sp.y == 0.f);
    }

    if (g_failures == 0) std::printf("test_room: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
