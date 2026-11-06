// test_lua_policy.cpp —— ep08 步骤 4：Lua 策略对拍 + 不变式门禁（§10.2.7 验收门 1/2/3/5/6）
// 覆盖：E2 脚本缺失兜底 / 等价脚本对拍（epsilon）/ 真公式手算（headshot·falloff）/
//       坏 patch 拒收保留旧引用 / 条件崩溃→pcall 兜底+计数 / 内存 cap OOM 兜底 /
//       沙箱逃逸全拒 / bot 双路径（ref=-1 内建 · patch 后走 Lua）。
// 铁律：零网络 / 零 DB；本测试属最外层（允许 include lua.h——内层禁令不适用测试）。
// 编译：见 build.sh（lua5.4 + sol2 头路径）。
#include "adapters/scripting_lua.h"
#include "match/policies.h"

#include <lua.hpp>   // 经 extern "C" 包裹（lua.h 无 C++ 守卫·同 scripting_lua.cpp）

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace fs = std::filesystem;
namespace mc = sightline::match;
namespace app = sightline::application;

static int g_checked = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);    \
            return 1;                                                   \
        }                                                               \
    } while (0)

namespace {

// ICombatParams 桩（Formula=默认公式参数——脚本注入源，热更由 ConfigService 管，测试不涉及）
struct StubCombat final : app::ICombatParams {
    std::uint16_t MaxHp() const override { return 100; }
    mc::WeaponParams Weapon(mc::EWeaponType) const override { return mc::kDefaultWeaponParams; }
    mc::FormulaParams Formula() const override { return mc::FormulaParams{}; }  // 2.0 / 3000 / 0.8
};

void WriteFile(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << content;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool GlobalIsNil(lua_State* L, const char* n) {
    lua_getglobal(L, n);
    const bool isnil = lua_isnil(L, -1);
    lua_pop(L, 1);
    return isnil;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "sightline_lua_policy";
    fs::remove_all(dir);

    // ---- 1) E2 部署韧性：脚本全缺 → Create 成功、策略直走内建（服务照起） ----
    {
        auto rt = adapters::LuaRuntime::Create({dir.string()});
        CHECK(rt != nullptr);
        CHECK(rt->Ref("damage") == -1);
        CHECK(rt->Ref("bot_ai") == -1);
        adapters::LuaDamagePolicy dmg(rt, nullptr);          // combat 空也先被 ref=-1 拦到内建
        CHECK(dmg.Apply(100.f, 25.f, {}) == 75.f);
        CHECK(rt->Fallbacks() == 0);                         // 缺文件=静默路径（启动已 WARNING），不算崩溃
    }

    // ---- 2) 等价脚本对拍（验收门 1：调用链路逐点一致，epsilon） ----
    WriteFile(dir / "damage.lua",
              "return function(hp, base, ctx, params)\n"
              "    return math.max(0, hp - base)\n"
              "end\n");
    StubCombat combat;
    auto rt = adapters::LuaRuntime::Create({dir.string()});
    CHECK(rt != nullptr);
    CHECK(rt->Ref("damage") >= 0);
    CHECK(rt->Ref("bot_ai") == -1);                          // 步 8 前 bot_ai 缺失=预期
    {
        adapters::LuaDamagePolicy luaDmg(rt, &combat);
        mc::BuiltinDamagePolicy builtin;
        const float samples[][2] = {{100, 25}, {10, 25}, {25, 25}, {75, 50}, {1, 1}, {0, 100}, {99, 200}};
        for (const auto& s : samples) {
            const float a = luaDmg.Apply(s[0], s[1], mc::HitContext{100.f, false});
            const float b = builtin.Apply(s[0], s[1], mc::HitContext{100.f, false});
            CHECK(std::fabs(a - b) < 1e-4f);
        }
        CHECK(rt->Fallbacks() == 0);                         // 对拍全走 Lua（无兜底）
    }

    // ---- 3) 真公式手算（assets/scripts/damage.lua 经 patch 装入——headshot/falloff 三路） ----
    {
        const std::string real = ReadFile(fs::path("assets/scripts/damage.lua"));
        CHECK(!real.empty());                                // CWD=server（build.sh 约定）
        const auto pr = rt->Patch("damage", real);
        CHECK(pr.ok);
        adapters::LuaDamagePolicy luaDmg(rt, &combat);
        // base 路径：dist=100 < 3000 无衰减、无爆头 → hp-25
        CHECK(std::fabs(luaDmg.Apply(100.f, 25.f, {100.f, false}) - 75.f) < 1e-4f);
        // 爆头：×2.0 → hp-50
        CHECK(std::fabs(luaDmg.Apply(100.f, 25.f, {100.f, true}) - 50.f) < 1e-4f);
        // 远距衰减：dist=4000 > 3000 → ×0.8 → hp-20
        CHECK(std::fabs(luaDmg.Apply(100.f, 25.f, {4000.f, false}) - 80.f) < 1e-4f);
        // 钳 0 不死（脚本 math.max(0,...) + 实体层 Clamp 双保险）
        CHECK(luaDmg.Apply(10.f, 25.f, {100.f, false}) == 0.f);
        CHECK(rt->Fallbacks() == 0);
    }

    // ---- 4) 坏 patch 四连拒（验收门 2：拒收保留旧引用·行为不变） ----
    {
        adapters::LuaDamagePolicy luaDmg(rt, &combat);
        const float before = luaDmg.Apply(100.f, 25.f, {100.f, false});   // 75（真公式）
        CHECK(std::fabs(before - 75.f) < 1e-4f);
        CHECK(!rt->Patch("damage", "return function(hp,base,ctx,params) return 0/0 end").ok);        // NaN
        CHECK(!rt->Patch("damage", "return function(hp,base,ctx,params) return hp + 50 end").ok);     // >hp
        CHECK(!rt->Patch("damage", "return function(hp,base,ctx,params) return -1 end").ok);          // 负数
        CHECK(!rt->Patch("damage", "return function(hp,base,ctx,params) error('boom') end").ok);      // 抛错
        CHECK(!rt->Patch("damage", "not a lua at all !!!").ok);                                       // 编译失败
        CHECK(!rt->Patch("no_such_script", "").ok);                                                   // 未知名
        // 四连拒后旧引用仍在：行为仍是真公式
        CHECK(std::fabs(luaDmg.Apply(100.f, 25.f, {100.f, false}) - 75.f) < 1e-4f);
        CHECK(std::fabs(luaDmg.Apply(100.f, 25.f, {4000.f, false}) - 80.f) < 1e-4f);
    }

    // ---- 5) 条件崩溃→pcall 兜底+计数（验收门 3：自测过、实战崩——服务器不塌） ----
    {
        // 自测用 dist=100（不崩），实战 dist>500 才 error——坏脚本必须先过自测才能进 Apply，
        // 否则在 Patch 就被拒（门 2），永远走不到 pcall 兜底（门 3）。
        const auto pr = rt->Patch("damage",
            "return function(hp, base, ctx, params)\n"
            "    if ctx.dist > 500 then error('boom at range') end\n"
            "    return math.max(0, hp - base)\n"
            "end\n");
        CHECK(pr.ok);                                          // dist=100 自测通过
        adapters::LuaDamagePolicy luaDmg(rt, &combat);
        const auto fb0 = rt->Fallbacks();
        const float near = luaDmg.Apply(100.f, 25.f, {100.f, false});
        CHECK(std::fabs(near - 75.f) < 1e-4f);                 // 近距=脚本正常路径
        CHECK(rt->Fallbacks() == fb0);                         // 未崩不计数
        const float far_ = luaDmg.Apply(100.f, 25.f, {4000.f, false});
        CHECK(rt->Fallbacks() > fb0);                          // 远距 error() → pcall 兜底+计数
        CHECK(far_ == 75.f);                                   // 兜底=内建=75
    }

    // ---- 6) 内存 cap（验收门 5：死循环分配 → OOM 在 pcall 内被接住 → 兜底） ----
    {
        adapters::LuaRuntimeConfig cfg;
        cfg.scriptDir = dir.string();
        cfg.memoryCapBytes = 1u << 20;                         // 1MB 小帽
        auto rtSmall = adapters::LuaRuntime::Create(cfg);
        CHECK(rtSmall != nullptr);                             // 小帽照样起（E2）
        const auto pr = rtSmall->Patch("damage",
            "return function(hp, base, ctx, params)\n"
            "    if ctx.dist > 500 then\n"
            "        local t = {}\n"
            "        while true do t[#t + 1] = {} end\n"       // 分配到爆（pcall 内=可捕获）
            "    end\n"
            "    return math.max(0, hp - base)\n"
            "end\n");
        CHECK(pr.ok);
        adapters::LuaDamagePolicy bomb(rtSmall, &combat);
        CHECK(std::fabs(bomb.Apply(100.f, 25.f, {100.f, false}) - 75.f) < 1e-4f);   // 近距正常
        const auto fb0 = rtSmall->Fallbacks();
        const float oom = bomb.Apply(100.f, 25.f, {4000.f, false});                  // 远距爆
        CHECK(rtSmall->Fallbacks() > fb0);                     // OOM→pcall 接住→兜底+计数
        CHECK(oom == 75.f);
        CHECK(rtSmall->UsedBytes() <= rtSmall->Cap());         // 计数器没被爆穿
    }

    // ---- 7) 沙箱逃逸全拒（验收门 6） ----
    {
        lua_State* L = rt->L();
        CHECK(L != nullptr);
        for (const char* n : {"dofile", "loadfile", "load", "loadstring",
                              "require", "collectgarbage", "os", "io", "package"})
            CHECK(GlobalIsNil(L, n));
        CHECK(luaL_dostring(L, "return dofile('/etc/passwd')") != LUA_OK); lua_pop(L, 1);
        CHECK(luaL_dostring(L, "return os.execute('id')") != LUA_OK);      lua_pop(L, 1);
        CHECK(luaL_dostring(L, "return require('os')") != LUA_OK);         lua_pop(L, 1);
        CHECK(luaL_dostring(L, "return load('return 1')()") != LUA_OK);    lua_pop(L, 1);
        // 内层纪律镜像：本状态里写不出文件（沙箱即证据）
        CHECK(luaL_dostring(L, "local f = io.open('/tmp/x', 'w')") != LUA_OK); lua_pop(L, 1);
    }

    // ---- 8) bot 双路径（ref=-1 内建 · patch 后走 Lua+Sanitize） ----
    {
        adapters::LuaBotPolicy botPol(rt);
        mc::BotWorldView w;
        w.patrolTarget = {100.f, 0.f};
        mc::BotState st = mc::BotState::Patrol;
        mc::BotMemory mem;
        // ref=-1（bot_ai.lua 不存在）→ 内建巡逻
        const auto i1 = botPol.Decide(w, st, mem, 1000, mc::BotTuning{});
        CHECK(i1.hasMove);
        CHECK(std::fabs(i1.moveTarget.x - 100.f) < 1e-3f);
        // patch 一个越界脚本 → Sanitize 收口（A3 出口校验）
        const auto pr = rt->Patch("bot_ai",
            "return function(view, state, mem, now_ms, tuning)\n"
            "    state.s = 0\n"
            "    return { has_move = true, move_x = 999999, move_y = -999999,\n"
            "             aim_yaw_deg = 0/0, fire = false }\n"
            "end\n");
        CHECK(pr.ok);
        const auto i2 = botPol.Decide(w, st, mem, 1000, mc::BotTuning{});
        CHECK(i2.hasMove);
        CHECK(i2.moveTarget.x <= 4000.f && i2.moveTarget.y >= -4000.f);   // 活动区 clamp
        CHECK(std::isfinite(i2.aimYawDeg));                               // NaN 兜底
        CHECK(!i2.bFire);
    }

    // ---- 9) 步骤 8 · §4.7 第一层：bot_ai.lua 等价性对拍（真脚本 vs BuiltinBotPolicy） ----
    // 门 1 的 bot 半边（切默认前的一次性门禁）：八条固定转移 + 200 步确定性 fuzz——
    // 同输入双实现逐字段一致（epsilon）；Fallbacks()==0 证明全程真走 Lua（防"双双兜底=假等价"）。
    {
        fs::path real;
        for (const char* c : {"assets/scripts/bot_ai.lua", "../assets/scripts/bot_ai.lua"}) {
            if (fs::exists(c)) { real = c; break; }
        }
        CHECK(!real.empty());
        WriteFile(dir / "bot_ai.lua", ReadFile(real));
        auto rt2 = adapters::LuaRuntime::Create({dir.string()});
        CHECK(rt2 != nullptr);
        CHECK(rt2->Ref("bot_ai") >= 0);
        adapters::LuaBotPolicy luaPol(rt2);
        mc::BuiltinBotPolicy builtin;
        const mc::BotTuning t{};

        std::string diff;
        const auto close = [](double a, double b) { return std::fabs(a - b) < 1e-3; };
        const auto compare = [&](const mc::BotIntent& ia, const mc::BotIntent& ib,
                                 const mc::BotState& sa, const mc::BotState& sb,
                                 const mc::BotMemory& ra, const mc::BotMemory& rb,
                                 const char* tag) {
            diff.clear();
            if (ia.hasMove != ib.hasMove) diff += " hasMove";
            if (!close(ia.moveTarget.x, ib.moveTarget.x) || !close(ia.moveTarget.y, ib.moveTarget.y)) diff += " move";
            if (!close(ia.aimYawDeg, ib.aimYawDeg)) diff += " aim";
            if (ia.bFire != ib.bFire) diff += " fire";
            if (sa != sb) diff += " state";
            if (ra.firstSeenMs != rb.firstSeenMs) diff += " firstSeen";
            if (ra.lastSeenMs != rb.lastSeenMs) diff += " lastSeen";
            if (!close(ra.lastKnown.x, rb.lastKnown.x) || !close(ra.lastKnown.y, rb.lastKnown.y)) diff += " lk";
            if (ra.hasLastKnown != rb.hasLastKnown) diff += " hasLk";
            if (!close(ra.aimErrDeg, rb.aimErrDeg)) diff += " err";
            if (ra.nextCoverOkMs != rb.nextCoverOkMs) diff += " coverOk";
            if (!close(ra.lastHpSeen, rb.lastHpSeen)) diff += " hpSeen";
            if (!diff.empty()) std::printf("对拍分歧[%s]:%s\n", tag, diff.c_str());
        };
        // -> int：CHECK 宏内含 return 1，会把 lambda 返回类型推成 int——
        // 显式声明，否则空 diff 走到末尾=UB（GCC 发 ud2 → SIGILL，实测）。
        const auto run_case = [&](const char* tag, mc::BotWorldView w, mc::BotState st,
                                  mc::BotMemory mem, std::uint64_t now) -> int {
            mc::BotState sa = st, sb = st;
            mc::BotMemory ma = mem, mb = mem;
            const auto ia = builtin.Decide(w, sa, ma, now, t);
            const auto ib = luaPol.Decide(w, sb, mb, now, t);
            compare(ia, ib, sa, sb, ma, mb, tag);
            CHECK(diff.empty());
            return 0;
        };

        // 八条固定状态转移
        CHECK(run_case("patrol-idle", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f; return w;
        }(), mc::BotState::Patrol, mc::BotMemory{}, 1000) == 0);
        CHECK(run_case("first-sight-far", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.yaw = 0.f; w.visible = mc::SeenEnemy{7u, -2000.f, 500.f, 2100.f}; return w;
        }(), mc::BotState::Patrol, mc::BotMemory{}, 2000) == 0);
        CHECK(run_case("engage-fire", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.yaw = 0.f; w.visible = mc::SeenEnemy{7u, 500.f, 0.f, 500.f}; return w;
        }(), mc::BotState::Engage, [] {
            mc::BotMemory m; m.firstSeenMs = 1500; m.lastSeenMs = 1500;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 4.f; m.lastHpSeen = 100.f; return m;
        }(), 1600) == 0);
        CHECK(run_case("engage-decayed", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.yaw = 0.f; w.visible = mc::SeenEnemy{7u, 500.f, 0.f, 500.f}; return w;
        }(), mc::BotState::Engage, [] {
            mc::BotMemory m; m.firstSeenMs = 1000; m.lastSeenMs = 5500;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 1.5f; m.lastHpSeen = 100.f; return m;
        }(), 6000) == 0);
        CHECK(run_case("lose-sight", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.x = 400.f; w.self.y = 0.f; w.self.yaw = 90.f; return w;
        }(), mc::BotState::Engage, [] {
            mc::BotMemory m; m.firstSeenMs = 1000; m.lastSeenMs = 1000;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 4.f; m.lastHpSeen = 100.f; return m;
        }(), 1100) == 0);
        CHECK(run_case("search-arrive", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.x = 470.f; w.self.y = 0.f; w.self.yaw = 180.f; return w;
        }(), mc::BotState::Search, [] {
            mc::BotMemory m; m.firstSeenMs = 1000; m.lastSeenMs = 1000;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 4.f; m.lastHpSeen = 100.f; return m;
        }(), 1200) == 0);
        CHECK(run_case("cover-drop", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.hp = 20.f; w.self.yaw = 0.f; w.visible = mc::SeenEnemy{7u, 500.f, 0.f, 500.f}; return w;
        }(), mc::BotState::Engage, [] {
            mc::BotMemory m; m.firstSeenMs = 1000; m.lastSeenMs = 1000;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 4.f; m.lastHpSeen = 100.f; return m;
        }(), 1100) == 0);
        CHECK(run_case("cover-expired", [] {
            mc::BotWorldView w; w.patrolTarget = {300.f, -200.f}; w.hpMax = 100.f;
            w.self.hp = 20.f; w.self.yaw = 0.f; w.visible = mc::SeenEnemy{7u, 500.f, 0.f, 500.f}; return w;
        }(), mc::BotState::TakeCover, [] {
            mc::BotMemory m; m.firstSeenMs = 1000; m.lastSeenMs = 1000;
            m.lastKnown = mc::Vec2{500.f, 0.f}; m.hasLastKnown = true;
            m.aimErrDeg = 4.f; m.nextCoverOkMs = 900; m.lastHpSeen = 100.f; return m;
        }(), 1100) == 0);

        // 200 步确定性 fuzz（LCG）：随机位置/可见性/血量/换弹，30Hz 步进——状态跨步连续演进
        {
            std::uint32_t seed = 0xB0CA1;
            const auto rnd = [&seed](float lo, float hi) {
                seed = seed * 1664525u + 1013904223u;
                const float u = static_cast<float>(seed >> 8) / 16777216.f;
                return lo + (hi - lo) * u;
            };
            mc::BotWorldView w; w.hpMax = 100.f; w.patrolTarget = {500.f, 300.f};
            mc::BotState s1 = mc::BotState::Patrol, s2 = s1;
            mc::BotMemory m1, m2 = m1;
            std::uint64_t now = 1000;
            for (int k = 0; k < 200; ++k) {
                w.self.x = rnd(-3000.f, 3000.f);
                w.self.y = rnd(-3000.f, 3000.f);
                w.self.yaw = rnd(-180.f, 180.f);
                w.self.hp = rnd(1.f, 100.f);
                w.self.reloading = (k % 13) == 0;
                if (rnd(0.f, 1.f) < 0.6f) {
                    const float ex = w.self.x + rnd(-2500.f, 2500.f);
                    const float ey = w.self.y + rnd(-2500.f, 2500.f);
                    const float dx = ex - w.self.x, dy = ey - w.self.y;
                    w.visible = mc::SeenEnemy{9u, ex, ey, std::sqrt(dx * dx + dy * dy)};
                } else {
                    w.visible = std::nullopt;
                }
                if (rnd(0.f, 1.f) < 0.3f) {
                    w.lastKnown = mc::Vec3{w.self.x + rnd(-1500.f, 1500.f), 0.f,
                                           w.self.y + rnd(-1500.f, 1500.f)};
                } else {
                    w.lastKnown = std::nullopt;
                }
                now += 33;
                const auto ia = builtin.Decide(w, s1, m1, now, t);
                const auto ib = luaPol.Decide(w, s2, m2, now, t);
                compare(ia, ib, s1, s2, m1, m2, "fuzz");
                CHECK(diff.empty());
            }
        }
        CHECK(rt2->Fallbacks() == 0);   // 全程真走 Lua（双双兜底=假等价的防线）
    }

    fs::remove_all(dir);
    std::printf("全部通过：%d 项断言 OK（ep08 步骤 4+8 Lua 策略对拍/门禁·验收门 1/2/3/5/6+bot 等价）\n", g_checked);
    return 0;
}
