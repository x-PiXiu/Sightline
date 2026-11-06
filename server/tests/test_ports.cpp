// test_ports.cpp —— ep08 步骤 2：端口 / 值对象 / 纯策略单测
// 编译：g++ -std=c++20 -I include tests/test_ports.cpp -o /tmp/test_ports && /tmp/test_ports
// 对应设计草案 §9「策略单测（零 IO）」与步骤 2 验收门「新不变式单测绿 / 238 断言行为零变化」。
// 纪律：零 Lua、零 DB、零网络、零线程；只 include 实体层与用例层纯头文件。
#include "application/durability_policy.h"
#include "application/policies.h"
#include "match/game_config.h"
#include "match/invariants.h"

#include <cmath>
#include <cstdio>
#include <limits>

using namespace sightline;
namespace app = sightline::application;

static int g_checked = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);   \
            return 1;                                                   \
        }                                                               \
    } while (0)

int main() {
    // ---- 1) GameConfig::Validate（G1：实体方法，坏表整表拒收）----
    {
        match::GameConfig cfg;
        CHECK(!cfg.Validate().has_value());                 // 默认配置合法
        CHECK(cfg.maxHp == 100 && cfg.weaponCount == 2);
        CHECK(cfg.weapons[0].mag == 30 && cfg.weapons[0].fireMs == 95);
        CHECK(cfg.weapons[1].damage == 20 && cfg.weapons[1].fireMs == 220);

        match::GameConfig bad = cfg;
        bad.weapons[0].damage = 0;      CHECK(bad.Validate().has_value());
        bad = cfg; bad.weapons[0].mag = 0;          CHECK(bad.Validate().has_value());
        bad = cfg; bad.weapons[0].reloadMs = 0;     CHECK(bad.Validate().has_value());
        bad = cfg; bad.weapons[0].fireMs = 0;       CHECK(bad.Validate().has_value());
        bad = cfg; bad.bot.fireIntervalMs = 50.f;   CHECK(bad.Validate().has_value()); // 快于 rifle 95ms
        bad = cfg; bad.bot.errEndDeg = 9.f;         CHECK(bad.Validate().has_value());
        bad = cfg; bad.deathmatch.invulnMs = 3000;  CHECK(bad.Validate().has_value());
        bad = cfg; bad.formula.falloffFactor = 1.5f;CHECK(bad.Validate().has_value());
        bad = cfg; bad.weaponCount = 5;             CHECK(bad.Validate().has_value());
    }

    // ---- 2) DamageInvariants（C++ 与 Lua 两条路径共用）----
    {
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const float inf = std::numeric_limits<float>::infinity();
        CHECK(match::DamageInvariants::Clamp(100.f, 75.f) == 75.f);
        CHECK(match::DamageInvariants::Clamp(100.f, -5.f) == 0.f);
        CHECK(match::DamageInvariants::Clamp(100.f, 120.f) == 100.f);   // 单调不增
        CHECK(std::isnan(match::DamageInvariants::Clamp(100.f, nan)) == false);
        CHECK(match::DamageInvariants::Clamp(100.f, nan) == 100.f);
        CHECK(match::DamageInvariants::Clamp(100.f, inf) == 100.f);
        CHECK(match::DamageInvariants::Clamp(100.f, -inf) == 100.f);   // ±Inf 同属非有限：不掉血，不静默致死
        CHECK(match::DamageInvariants::Clamp(0.f, -inf) == 0.f);        // upper 随 hp 走

        app::BuiltinDamagePolicy builtin;
        CHECK(builtin.Apply(100.f, 25.f, {}) == 75.f);   // 等价现行 DamageApply（钳 0 不死）
        CHECK(builtin.Apply(10.f, 25.f, {}) == 0.f);
    }

    // ---- 3) BotIntentSanitizer ----
    {
        match::BotIntent in;
        in.moveTarget.x = 999999.f;
        in.moveTarget.y = -999999.f;
        in.aimYawDeg = std::numeric_limits<float>::quiet_NaN();
        const auto out = match::BotIntentSanitizer::Sanitize(in, 4000.f);
        CHECK(out.moveTarget.x == 4000.f);
        CHECK(out.moveTarget.y == -4000.f);
        CHECK(out.aimYawDeg == 0.f);
        CHECK(std::isfinite(out.aimYawDeg));
    }

    // ---- 4) DurabilityPolicy（§5.3 定案：Must 落 WAL，不无限积压）----
    {
        app::SinkHealth ok{true, true, true};
        app::SinkHealth down{false, false, false};

        app::QueueState mustOk{0, 1024, app::Durability::Must, ok};
        CHECK(app::DurabilityPolicy::Classify(mustOk) == app::QueueAction::Enqueue);

        app::QueueState mustDown{0, 1024, app::Durability::Must, down};
        CHECK(app::DurabilityPolicy::Classify(mustDown) == app::QueueAction::SpillToWal);

        app::QueueState mustFull{1024, 1024, app::Durability::Must, ok};
        CHECK(app::DurabilityPolicy::Classify(mustFull) == app::QueueAction::SpillToWal);

        app::QueueState statFull{1024, 1024, app::Durability::BestEffort, ok};
        CHECK(app::DurabilityPolicy::Classify(statFull) == app::QueueAction::DropAndCount);

        app::QueueState statOk{0, 1024, app::Durability::BestEffort, ok};
        CHECK(app::DurabilityPolicy::Classify(statOk) == app::QueueAction::Enqueue);

        const auto d = app::DurabilityPolicy::Degrade(down);
        CHECK(d.continueMatch && d.warn);                  // 三库全挂：对局照跑 + 告警
        CHECK(!app::DurabilityPolicy::Degrade(ok).warn);
    }

    // ---- 5) 238 断言行为零变化：两参数调用走默认 WeaponParams ----
    //     复现 tests/test_match.cpp L181-188 的武器门序列。
    {
        match::WeaponState w;
        CHECK(match::ConsumeWeaponGate(w, 1000) == match::FireGate::Ok);
        w.lastFireMs = 1000; w.bHasFired = true;
        CHECK(match::ConsumeWeaponGate(w, 1050) == match::FireGate::RateLimited);
        CHECK(match::ConsumeWeaponGate(w, 1097) == match::FireGate::Ok);
        w.magUsed = match::kMagCapacity;
        CHECK(match::ConsumeWeaponGate(w, 2000) == match::FireGate::MagEmpty);
        CHECK(match::ConsumeWeaponGate(w, 4000) == match::FireGate::Reloading);
        CHECK(match::ConsumeWeaponGate(w, 4600) == match::FireGate::Ok);
        CHECK(w.magUsed == 0);

        match::WeaponState a;
        for (int i = 0; i < 30; ++i) match::ConsumeAmmo(a, 1000);
        CHECK(a.reloadUntilMs == 1000 + match::kReloadMs);   // 空匣即上钟（2500ms）
    }

    std::printf("全部通过：%d 项断言 OK（ep08 步骤 2 端口 / 值对象 / 纯策略，零 IO）\n", g_checked);
    return 0;
}
