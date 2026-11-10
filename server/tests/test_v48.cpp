// test_v48.cpp —— ep08 步骤 6：契约 v4.8 wire 断言 + 换枪状态机
// 覆盖：28 号 Parse/Encode/DecodeC2S、30 号变长 payload 编解码（含 damage）、
//       17 号 flags bit3~4、换枪状态机与 per-weapon 换弹钟。
// 铁律：零网络 / 零 Lua / 零 DB，纯字节 + 纯逻辑断言。
#include "match/fire_judge.h"
#include "match/match_session.h"
#include "protocol/messages.h"
#include "protocol/msg_ids.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

using namespace sightline;
namespace wire = sightline::wire;
namespace mc = sightline::match;

static int g_checked = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);   \
            return 1;                                                   \
        }                                                               \
    } while (0)

static std::span<const std::uint8_t> SP(const std::string& s) {
    return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

int main() {
    // ---- 1) 消息号占用（28 / 29 / 30 互不冲突）----
    CHECK(static_cast<int>(protocol::MSG_C2S_SWITCH_WEAPON) == 28);
    CHECK(static_cast<int>(protocol::MSG_C2S_RELOAD) == 29);
    CHECK(static_cast<int>(protocol::MSG_S2C_CONFIG_VERSION) == 30);
    CHECK(static_cast<int>(protocol::MSG_C2S_FIRE) == 20);

    // ---- 2) 28 号：1B type，非法值/长度整包拒收 ----
    {
        std::string ok{char(1)};
        const auto m = wire::SwitchWeaponMsg::Parse(SP(ok));
        CHECK(m.has_value() && m->weaponType == 1);
        CHECK(m->Encode() == ok);
        CHECK(!wire::SwitchWeaponMsg::Parse(SP(std::string{})).has_value());
        CHECK(!wire::SwitchWeaponMsg::Parse(SP(std::string{char(1), char(0)})).has_value());
        CHECK(!wire::SwitchWeaponMsg::Parse(SP(std::string{char(4)})).has_value());
        const auto decoded = wire::DecodeC2S(28, SP(ok));
        CHECK(decoded.has_value());
        CHECK(std::holds_alternative<wire::SwitchWeaponMsg>(*decoded));
        CHECK(!wire::DecodeC2S(28, SP(std::string{char(9)})).has_value());
    }

    // ---- 3) 30 号：变长 payload 字节级布局 + roundtrip ----
    {
        wire::ConfigVersionMsg msg;
        msg.version = "v7-a3f2c1d8";
        msg.weapons.push_back({0, 25, 30, 2500, 95});
        msg.weapons.push_back({1, 20, 12, 1800, 220});
        const std::string p = msg.Encode();
        CHECK(!p.empty());
        CHECK(static_cast<std::uint8_t>(p[0]) == msg.version.size());
        CHECK(std::string(p.data() + 1, msg.version.size()) == msg.version);
        CHECK(static_cast<std::uint8_t>(p[1 + msg.version.size()]) == 2);
        CHECK(p.size() == 1 + msg.version.size() + 1 + 2 * wire::ConfigVersionMsg::kRowSize);
        const std::size_t o = 2 + msg.version.size();
        CHECK(static_cast<std::uint8_t>(p[o]) == 0);        // type
        CHECK(static_cast<std::uint8_t>(p[o + 1]) == 25);  // damage（飘字来源）
        CHECK(static_cast<std::uint8_t>(p[o + 2]) == 30);  // mag
        CHECK(static_cast<std::uint8_t>(p[o + 3]) == (2500 & 0xFF));
        CHECK(static_cast<std::uint8_t>(p[o + 4]) == (2500 >> 8));

        const auto back = wire::ConfigVersionMsg::Parse(SP(p));
        CHECK(back.has_value());
        CHECK(back->version == msg.version);
        CHECK(back->weapons.size() == 2);
        CHECK(back->weapons[0].damage == 25 && back->weapons[0].reloadMs == 2500);
        CHECK(back->weapons[1].type == 1 && back->weapons[1].mag == 12);
        CHECK(back->weapons[1].fireIntervalMs == 220);

        // 坏包：截断一行 / N 超过 4 / verLen 越界 / 空包
        CHECK(!wire::ConfigVersionMsg::Parse(SP(p.substr(0, p.size() - 1))).has_value());
        std::string badN = p;
        badN[1 + msg.version.size()] = char(5);
        CHECK(!wire::ConfigVersionMsg::Parse(SP(badN)).has_value());
        std::string badLen = p;
        badLen[0] = char(255);
        CHECK(!wire::ConfigVersionMsg::Parse(SP(badLen)).has_value());
        CHECK(!wire::ConfigVersionMsg::Parse(SP(std::string{})).has_value());
    }

    // ---- 4) 17 号 flags bit3~4：低位语义不被破坏 ----
    {
        const std::uint8_t base = 0x07;   // bit0 腾空 + bit1 死亡 + bit2 无敌
        for (std::uint8_t t = 0; t < 4; ++t)
            CHECK(wire::GetWeaponType(wire::PutWeaponType(base, t)) == t);
        CHECK((wire::PutWeaponType(base, 2) & 0x07) == 0x07);
        CHECK(wire::PutWeaponType(0x18, 0) == 0);
    }

    // ---- 5) 换枪状态机 + per-weapon 换弹钟 ----
    {
        mc::MatchSession m;
        mc::GameConfig gc;                              // v1.6：SetWeaponTable → SetConfig（完整规则快照）
        gc.weapons = {mc::kDefaultWeaponParams,
                      mc::WeaponParams{20, 12, 1800, 220},
                      mc::WeaponParams{}, mc::WeaponParams{}};
        gc.weaponCount = 2;
        gc.maxHp = 100;
        m.SetConfig(gc);
        m.StartMatch(1, {{1, "alpha", 0}});
        m.UpdateEntity(1, 0.f, 0.f, 90.f, 0.f, 0.f, 0, 1, 1000);
        CHECK(m.WeaponTypeOf(1) == 0);
        CHECK(!m.SwitchWeapon(1, 2, 1000));            // 配置表只有 2 把
        CHECK(m.SwitchWeapon(1, 1, 1000));
        CHECK(m.WeaponTypeOf(1) == 1);
        CHECK(m.Entities()[0].weapon.reloadUntilMs == 0);
        CHECK(m.Entities()[0].weapon.lastFireMs == 0);

        const auto events = m.Fire(1, 1000);           // 单人局必 miss，但扣弹照走
        CHECK(!events.empty());
        CHECK(m.Entities()[0].weapon.magUsed == 1);
        CHECK(m.Reload(1, 1000));
        CHECK(m.Entities()[0].weapon.reloadUntilMs == 1000 + 1800);   // Pistol 的 reload_ms

        // 换枪取消在途换弹 + 满匣 + 首发复位
        CHECK(m.SwitchWeapon(1, 0, 1100));
        CHECK(m.Entities()[0].weapon.magUsed == 0);
        CHECK(m.Entities()[0].weapon.reloadUntilMs == 0);
        CHECK(!m.Entities()[0].weapon.bHasFired);

        // 17 号快照 flags 带 weaponType
        m.UpdateEntity(1, 0.f, 0.f, 90.f, 0.f, 0.f, 0, 2, 1200);
        const auto tick = m.Tick(1200);
        CHECK(!tick.empty());
        const auto it = std::find_if(tick.begin(), tick.end(), [](const mc::MatchEvent& e) {
            return e.type == mc::MatchEvent::Type::Snapshot;
        });
        CHECK(it != tick.end());
        CHECK(!it->entities.empty());
        CHECK(wire::GetWeaponType(it->entities[0].flags) == 0);
    }

    // ---- 6) per-weapon 伤害纯函数（配置表读取链路的终点）----
    CHECK(mc::DamageApply(100.f, 50.f) == 50.f);
    CHECK(mc::DamageApply(20.f, 50.f) == 0.f);
    CHECK(mc::DamageApply(100.f) == 75.f);             // 旧入口行为零变化

    // ---- 7) 热重载三语义（§3.5 · 步骤 3 验收门：改表不重编，行为按类生效）----
    {
        // 语义① 数值类=下一发即时生效（伤害 25→50，SetConfig 后第二发按新值扣）
        mc::MatchSession m;
        m.StartMatch(1, {{1, "a", 0}, {2, "b", 0}});
        m.UpdateEntity(1, 0.f, 0.f, 90.f, 0.f, 0.f, 0, 1, 1000);
        m.UpdateEntity(2, 500.f, 0.f, 90.f, 0.f, 0.f, 0, 1, 1000);
        auto ev = m.Fire(1, 1000);
        CHECK(!ev.empty() && ev[0].fire.bHit && ev[0].fire.victimHp == 75);   // 100-25
        mc::GameConfig c;                                       // 默认 rifle damage=25
        c.weapons[0].damage = 50;
        m.SetConfig(c);                                         // 模拟 reload 后新表
        ev = m.Fire(1, 1100);                                   // ≥95ms 射速门
        CHECK(!ev.empty() && ev[0].fire.bHit && ev[0].fire.victimHp == 25);   // 75-50=25 即时生效
    }
    {
        // 语义② 倒计时类=在途不动（换弹钟已上，热改 reloadMs 不改写已存到期时刻）
        mc::MatchSession m;
        m.StartMatch(1, {{1, "a", 0}});
        m.UpdateEntity(1, 0.f, 0.f, 90.f, 0.f, 0.f, 0, 1, 1000);
        CHECK(m.Fire(1, 1500).size() == 1);                     // 打一发（满匣 R 被拒——magUsed=0 门）
        CHECK(m.Reload(1, 2000));                               // 半匣上钟：到期 = 2000+2500 = 4500
        mc::GameConfig c;
        c.weapons[0].reloadMs = 9999;
        m.SetConfig(c);                                         // 热改换弹时长
        CHECK(m.Entities()[0].weapon.reloadUntilMs == 4500);    // ★在途钟不被新值改写（绝对时刻）
        auto ev = m.Fire(1, 4600);                              // 老钟到期 → 补满 → 接受
        CHECK(!ev.empty() && ev[0].fire.bAccepted);
    }
    {
        // 语义③ 容量收缩=自愈（magUsed 28 > 新匣 25 → 下一发 MagEmpty 触发自动换弹 → 补到新容量）
        mc::MatchSession m;
        m.StartMatch(1, {{1, "a", 0}});
        m.UpdateEntity(1, 0.f, 0.f, 90.f, 0.f, 0.f, 0, 1, 1000);
        for (int i = 0; i < 28; ++i) (void)m.Fire(1, 1000 + i * 100);   // 打 28 发（<30 不上钟）
        CHECK(m.Entities()[0].weapon.magUsed == 28);
        mc::GameConfig c;
        c.weapons[0].mag = 25;
        m.SetConfig(c);                                         // 收缩：28 > 25
        auto rej = m.Fire(1, 3900);                             // 下一发：magUsed ≥ 新匣 → MagEmpty
        CHECK(!rej.empty() && !rej[0].fire.bAccepted);          // 触发自动换弹（自愈入口·该发被拒）
        auto ok = m.Fire(1, 3900 + 2500 + 10);                  // 默认 reloadMs 到期 → 补满续射
        CHECK(!ok.empty() && ok[0].fire.bAccepted);
        CHECK(m.Entities()[0].weapon.magUsed == 1);             // 补满（0 用）后又消耗 1 发=新容量语义
    }

    std::printf("全部通过：%d 项断言 OK（ep08 步骤 6 契约 v4.8 / 换枪状态机 / 热重载三语义）\n", g_checked);
    return 0;
}
