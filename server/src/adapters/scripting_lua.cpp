// 构建守卫：无 lua5.4 环境（零依赖 lite 构建）下本文件编译为空 TU——
// 头文件声明不依赖 lua（前置声明 lua_State），main/测试的使用点同受 SIGHTLINE_HAS_LUA 门控。
#if defined(SIGHTLINE_HAS_LUA) && SIGHTLINE_HAS_LUA
// scripting_lua.cpp —— ep08 步骤 4：Lua 运行时实现（★ 全服务端唯一 include lua.h / sol2 的翻译单元）
// §11.6 第 1 条自检把内层出现 sol2/lua.h 判违规——本文件是唯一的白名单点。
// 结构（§10.2.2~5）：CountedAlloc(B4 内存 cap) → 沙箱（选择性库 + NilGlobal）→ loadbuffer/ref
//                 → Patch（编译→自测→引用 swap）→ 双策略（pcall + 实体层不变式双路径共用）。
// 两处实勘修正（对 §10.2 草稿，见头文件注）：ILogger& → 既有 LOG_* 宏；LuaDamagePolicy 加
// ICombatParams*（params 来源，脚本签名 §10.2.4 需要）。
#include "adapters/scripting_lua.h"

#include "logger/logger.h"
#include "match/invariants.h"

// ⚠ Ubuntu 的 lua.h 无 extern "C" 守卫（上游惯例）——C++ 侧必须经 lua.hpp 包裹，
// 否则符号被 C++ 修饰 → 链接 undefined reference（本机实锤）。
#include <lua.hpp>

#include <sol/sol.hpp>

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

// LuaStateDeleter 在头文件里是全局域类型——定义必须同域（放 adapters 内=编译错）
void LuaStateDeleter::operator()(lua_State* L) const noexcept {
    if (L) lua_close(L);                    // 触发已分配内存的释放回调（CountedAlloc nsize=0）
}

namespace adapters {
namespace mc = sightline::match;
namespace app = sightline::application;

// ===================== 分配器 / 生命周期 =====================

// B4 内存 cap：超限返回 nullptr → Lua 抛 "not enough memory" → pcall 接住 → 兜底/拒收。
static void* CountedAlloc(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
    auto& c = *static_cast<LuaAllocCtx*>(ud);
    if (nsize == 0) {                       // 释放
        if (ptr) c.used -= osize;
        std::free(ptr);
        return nullptr;
    }
    const std::size_t after = c.used + nsize - (ptr ? osize : 0);
    if (after > c.cap) return nullptr;      // 超 32MB：拒分配（Lua 转为内存错误）
    void* p = std::realloc(ptr, nsize);
    if (p) c.used = after;
    return p;
}

// ===================== LuaRuntime =====================

LuaRuntime::LuaRuntime(const LuaRuntimeConfig& cfg) : cfg_(cfg) {
    alloc_.cap = cfg.memoryCapBytes;
}

bool LuaRuntime::InitSandbox() {
    lua_State* raw = lua_newstate(&CountedAlloc, &alloc_);
    if (!raw) return false;
    L_.reset(raw);
    lua_State* L = L_.get();
    // §4.3 沙箱红线：只开 base/string/math/table——os/io/package 一律不装（不用 luaL_openlibs）
    luaL_requiref(L, "_G", luaopen_base, 1);      lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1); lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);  lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);  lua_pop(L, 1);
    // base 库自带的文件/字节码入口全部掐掉（v1.3 修正：只开 base 仍能 dofile 读磁盘任意文件）
    for (const char* n : {"dofile", "loadfile", "load", "loadstring", "require", "collectgarbage"}) {
        lua_pushnil(L);
        lua_setglobal(L, n);
    }
    return true;
}

std::shared_ptr<LuaRuntime> LuaRuntime::Create(const LuaRuntimeConfig& cfg) {
    auto rt = std::shared_ptr<LuaRuntime>(new LuaRuntime(cfg));
    if (!rt->InitSandbox()) {
        LOG_ERROR("[Lua] 沙箱初始化失败（lua_newstate 返回 null——内存 cap=" +
                  std::to_string(cfg.memoryCapBytes) + " 过小？）");
        return nullptr;
    }
    rt->LoadFunction("damage");    // 公式面（step 4 交付物）
    rt->LoadFunction("bot_ai");    // 决策面（步 8 交付物——此前缺失=内建兜底，E2 告警一次）
    return rt;
}

LuaRuntime::~LuaRuntime() = default;   // L_ 析构（逆序：L_ 晚于 alloc_ 声明→先关）→ lua_close

static std::string ReadFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

int* LuaRuntime::RefSlot(const std::string& name) {
    if (name == "damage") return &refDamage_;
    if (name == "bot_ai") return &refBot_;
    return nullptr;
}

int LuaRuntime::Ref(const std::string& name) const {
    if (name == "damage") return refDamage_;
    if (name == "bot_ai") return refBot_;
    return -1;
}

int LuaRuntime::LoadFunction(const std::string& name) {
    lua_State* L = L_.get();
    if (!L) return -1;
    const std::string path = cfg_.scriptDir + "/" + name + ".lua";
    const std::string src = ReadFile(path);
    if (src.empty()) {
        // E2：脚本是增强不是依赖——缺失/空文件不拦启动，内建兜底照跑（必须喊一声）
        LOG_WARNING("[Lua] 脚本缺失或为空: " + path + "（走内建兜底）");
        return -1;
    }
    lastSource_[name] = src;                            // ep09 G5：加载初值=回退恢复点
    const std::string chunk = "@" + path;
    if (luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str()) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        LOG_WARNING("[Lua] 编译失败 " + name + ": " + (e ? e : "?"));
        lua_pop(L, 1);
        return -1;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {          // 执行 chunk 取其 return 的函数
        const char* e = lua_tostring(L, -1);
        LOG_WARNING("[Lua] chunk 执行失败 " + name + ": " + (e ? e : "?"));
        lua_pop(L, 1);
        return -1;
    }
    if (!lua_isfunction(L, -1)) {
        LOG_WARNING("[Lua] " + name + " 未返回函数（return function(...) end）");
        lua_pop(L, 1);
        return -1;
    }
    int* slot = RefSlot(name);
    if (!slot) { lua_pop(L, 1); return -1; }
    if (*slot != -1) luaL_unref(L, LUA_REGISTRYINDEX, *slot);
    *slot = luaL_ref(L, LUA_REGISTRYINDEX);          // 弹栈入库
    LOG_INFO("[Lua] 已加载 " + name + " → ref=" + std::to_string(*slot));
    return *slot;
}

// ===================== Patch（§10.2.3 热修三步）=====================
// 自测样本（§4.7 第二层不变式）：damage 三次调用验 有限/[0,hp]/单调(对 base)/确定性——
// 不过 = 拒收保留旧引用。bot 验 返回表+朝向有限。样本常量是探针输入，非业务值（C1 不适用）。

static bool CallDamageSample(lua_State* L, int ref, float hp, float base, float* out) {
  try {
    sol::state_view sv(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    sol::protected_function f(L, -1);
    lua_pop(L, 1);
    sol::table ctx = sv.create_table();
    ctx["dist"] = 100.0;              // 远小于 falloff_start：测纯 base 路径
    ctx["headshot"] = false;
    sol::table par = sv.create_table();
    par["headshot_multiplier"] = 2.0;
    par["falloff_start_cm"] = 3000.0;
    par["falloff_factor"] = 0.8;
    auto r = f(static_cast<double>(hp), static_cast<double>(base), ctx, par);
    if (!r.valid()) return false;
    const double v = r.get<double>();        // 非数字 → sol2 抛 → 外层 catch → 拒收
    if (!std::isfinite(v)) return false;
    *out = static_cast<float>(v);
    return true;
  } catch (...) { return false; }
}

static bool CallBotSample(lua_State* L, int ref) {
  try {
    sol::state_view sv(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    sol::protected_function f(L, -1);
    lua_pop(L, 1);
    sol::table view = sv.create_table();
    sol::table self = sv.create_table();
    self["x"] = 0.0; self["y"] = 0.0; self["z"] = 90.0; self["yaw"] = 0.0;
    self["hp"] = 100.0; self["mag_used"] = 0; self["reloading"] = false;
    view["self"] = self;
    view["visible"] = sol::nil;
    view["last_known"] = sol::nil;
    sol::table patrol = sv.create_table();
    patrol["x"] = 100.0; patrol["y"] = 0.0;
    view["patrol"] = patrol;
    view["hp_max"] = 100.0;
    sol::table st = sv.create_table();
    st["s"] = 0;                                   // BotState::Patrol
    sol::table mem = sv.create_table();
    mem["first_seen_ms"] = 0; mem["last_seen_ms"] = 0;
    mem["lk_x"] = 0.0; mem["lk_y"] = 0.0; mem["has_last_known"] = false;
    mem["aim_err_deg"] = 0.0; mem["next_cover_ok_ms"] = 0; mem["last_hp_seen"] = 100.0;
    sol::table tune = sv.create_table();
    tune["react_ms"] = 250.0; tune["err_start_deg"] = 4.0; tune["err_end_deg"] = 1.0;
    tune["err_decay_deg_per_s"] = 3.0; tune["turn_rate_deg_s"] = 360.0;
    tune["forget_ms"] = 3000.0; tune["engage_range_cm"] = 900.0;
    tune["hp_cover_below"] = 30.0; tune["move_speed_cm_s"] = 400.0;
    tune["fire_interval_ms"] = 400.0;
    auto r = f(view, st, mem, 1000.0, tune);
    if (!r.valid()) return false;
    if (r.get_type() != sol::type::table) return false;         // 必须返回意图表（get_type=noexcept）
    // 只验结构不验值域（分层设计）：bot 的值域守卫=出口 BotIntentSanitizer::Sanitize（A3 第二层），
    // 自测再查一遍=重复逻辑；damage 无第二层运行时守卫才在自测做值不变式。
    return true;
  } catch (...) { return false; }
}

app::PatchResult LuaRuntime::Patch(std::string_view name, std::string_view source) {
    lua_State* L = L_.get();
    if (!L) return {false, "lua 状态未初始化"};
    const std::string n(name);
    int* slot = RefSlot(n);
    if (!slot) return {false, "未知脚本名: " + n + "（damage / bot_ai）"};

    std::string src(source);
    if (src.empty()) {                       // console `patch <name>` 流：从自己的 scriptDir 重读
        src = ReadFile(cfg_.scriptDir + "/" + n + ".lua");
        if (src.empty()) return {false, "脚本不存在: " + n + ".lua（" + cfg_.scriptDir + "）"};
    }
    // 1) 编译（失败=拒收，旧引用不动）
    const std::string chunk = "@" + n + ".lua(patch)";
    if (luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str()) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        const std::string err = e ? e : "compile error";
        lua_pop(L, 1);
        LOG_WARNING("[Lua] patch 拒收(编译) " + n + ": " + err);
        return {false, "编译失败: " + err};
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* e = lua_tostring(L, -1);
        const std::string err = e ? e : "chunk error";
        lua_pop(L, 1);
        LOG_WARNING("[Lua] patch 拒收(chunk) " + n + ": " + err);
        return {false, "chunk 执行失败: " + err};
    }
    if (!lua_isfunction(L, -1)) { lua_pop(L, 1); return {false, "chunk 未返回函数"}; }
    // 2) 沙箱自测（栈顶=候选函数；不过=拒收保留旧引用）
    {
        const int cand = luaL_ref(L, LUA_REGISTRYINDEX);        // 暂存候选（弹栈）
        bool ok = false;
        if (n == "damage") {
            float a = 0.f, b = 0.f, c = 0.f;
            ok = CallDamageSample(L, cand, 100.f, 25.f, &a) &&
                 CallDamageSample(L, cand, 100.f, 50.f, &b) &&  // 单调：base 大→余血小
                 CallDamageSample(L, cand, 100.f, 25.f, &c) &&  // 确定性：同输入同输出
                 std::isfinite(a) && a >= 0.f && a <= 100.f &&
                 b <= a + 1e-6f && std::fabs(a - c) < 1e-6f;
        } else if (n == "bot_ai") {
            ok = CallBotSample(L, cand);
        }
        if (!ok) {
            luaL_unref(L, LUA_REGISTRYINDEX, cand);
            LOG_WARNING("[Lua] patch 拒收(不变式自测) " + n + "——旧引用保留");
            return {false, "不变式自测失败（有限/[0,hp]/单调/确定性 或 bot 返回表）"};
        }
        // 3) 引用原子换（单线程）——旧闭包 unref=upvalue 彩蛋的正解（§4.5：换引用不改值）
        const int old = *slot;
        *slot = cand;
        if (old != -1) luaL_unref(L, LUA_REGISTRYINDEX, old);
        lastSource_[n] = src;                           // ep09 G5：成功 patch 更新恢复点
    }
    LOG_INFO("[Lua] patch 生效 " + n + "（引用已换·旧闭包交 GC）");
    return {true, n + " patch 生效"};
}

// ===================== LuaDamagePolicy =====================

LuaDamagePolicy::LuaDamagePolicy(std::shared_ptr<LuaRuntime> rt,
                                 const app::ICombatParams* combat)
    : rt_(std::move(rt)), combat_(combat) {}

float LuaDamagePolicy::Apply(float hp, float base, const mc::HitContext& ctx) {
    lua_State* L = rt_ ? rt_->L() : nullptr;
    const int ref = rt_ ? rt_->Ref("damage") : -1;
    if (!L || ref < 0 || !combat_)                       // E2：脚本缺失 → 内建
        return builtin_.Apply(hp, base, ctx);
    // 余量守卫：实参表构建在 pcall 外——lua(C 版) 分配失败走 longjmp=panic 而非可捕获错误。
    // 距 cap <16KB 时根本不进 Lua（表构建仅 ~200B，16KB 足够余量），把 OOM 留给 pcall 内部。
    if (rt_->UsedBytes() + 16384 > rt_->Cap()) {
        rt_->CountFallback();
        LOG_WARNING("[Lua] 内存余量不足→内建兜底 used=" + std::to_string(rt_->UsedBytes()));
        return builtin_.Apply(hp, base, ctx);
    }
    try {
        sol::state_view sv(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        sol::protected_function f(L, -1);
        lua_pop(L, 1);
        sol::table ctxT = sv.create_table();
        ctxT["dist"] = static_cast<double>(ctx.dist);
        ctxT["headshot"] = ctx.headshot;
        const auto fp = combat_->Formula();              // 每次读原子视图（热更即时）
        sol::table parT = sv.create_table();
        parT["headshot_multiplier"] = static_cast<double>(fp.headshotMultiplier);
        parT["falloff_start_cm"] = static_cast<double>(fp.falloffStartCm);
        parT["falloff_factor"] = static_cast<double>(fp.falloffFactor);
        auto r = f(static_cast<double>(hp), static_cast<double>(base), ctxT, parT);
        if (!r.valid()) {
            sol::error e = r;
            rt_->CountFallback();
            LOG_WARNING(std::string("[Lua] damage pcall 失败→内建兜底: ") + e.what());
            return builtin_.Apply(hp, base, ctx);
        }
        const float raw = static_cast<float>(r.get<double>());
        // §10.2.5 出口不变式：合法值经实体层 Clamp 必须恒等——NaN/越界会被改写≠raw → 拒
        if (!std::isfinite(raw) || mc::DamageInvariants::Clamp(hp, raw) != raw) {
            rt_->CountFallback();
            LOG_WARNING("[Lua] damage 出口越界→内建兜底 raw=" + std::to_string(raw));
            return builtin_.Apply(hp, base, ctx);
        }
        return raw;
    } catch (...) {                                        // 谦卑兜底：绝不让异常穿出适配器
        rt_->CountFallback();
        LOG_WARNING("[Lua] damage 异常→内建兜底");
        return builtin_.Apply(hp, base, ctx);
    }
}

// ===================== LuaBotPolicy =====================
// 协议（步 8 的 bot_ai.lua 同此）：
//   入参 view{self{...},visible?,last_known?,patrol,hp_max} / state{s=0..3} / mem{...} /
//        now_ms / tuning{十旋钮}
//   返回 intent{has_move,move_x,move_y,aim_yaw_deg,fire}；state.s 与 mem 字段由脚本就地改写，
//   调用后读回（Lua 值语义——状态必须走表才可变）。
//   出口一律过实体层 BotIntentSanitizer（双路径共用，§10.2.5）。

LuaBotPolicy::LuaBotPolicy(std::shared_ptr<LuaRuntime> rt) : rt_(std::move(rt)) {}

mc::BotIntent LuaBotPolicy::Decide(const mc::BotWorldView& w, mc::BotState& state,
                                   mc::BotMemory& mem, std::uint64_t nowMs,
                                   const mc::BotTuning& t) {
    lua_State* L = rt_ ? rt_->L() : nullptr;
    const int ref = rt_ ? rt_->Ref("bot_ai") : -1;
    if (!L || ref < 0)                                     // E2：bot_ai.lua 步 8 前缺失 → 内建
        return builtin_.Decide(w, state, mem, nowMs, t);
    if (rt_->UsedBytes() + 16384 > rt_->Cap()) {           // 同 Apply：实参表构建在 pcall 外的余量守卫
        rt_->CountFallback();
        return builtin_.Decide(w, state, mem, nowMs, t);
    }
    try {
        sol::state_view sv(L);
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        sol::protected_function f(L, -1);
        lua_pop(L, 1);

        sol::table view = sv.create_table();
        sol::table self = sv.create_table();
        self["x"] = static_cast<double>(w.self.x);
        self["y"] = static_cast<double>(w.self.y);
        self["z"] = static_cast<double>(w.self.z);
        self["yaw"] = static_cast<double>(w.self.yaw);
        self["hp"] = static_cast<double>(w.self.hp);
        self["mag_used"] = w.self.magUsed;
        self["reloading"] = w.self.reloading;
        view["self"] = self;
        if (w.visible) {
            sol::table v = sv.create_table();
            v["pid"] = static_cast<double>(w.visible->pid);
            v["x"] = static_cast<double>(w.visible->x);
            v["y"] = static_cast<double>(w.visible->y);
            v["dist"] = static_cast<double>(w.visible->dist);
            view["visible"] = v;
        } else view["visible"] = sol::nil;
        if (w.lastKnown) {
            sol::table lk = sv.create_table();
            lk["x"] = static_cast<double>(w.lastKnown->x);
            lk["y"] = static_cast<double>(w.lastKnown->y);
            lk["z"] = static_cast<double>(w.lastKnown->z);
            view["last_known"] = lk;
        } else view["last_known"] = sol::nil;
        sol::table patrol = sv.create_table();
        patrol["x"] = static_cast<double>(w.patrolTarget.x);
        patrol["y"] = static_cast<double>(w.patrolTarget.y);
        view["patrol"] = patrol;
        view["hp_max"] = static_cast<double>(w.hpMax);

        sol::table stT = sv.create_table();
        stT["s"] = static_cast<int>(state);
        sol::table memT = sv.create_table();
        memT["first_seen_ms"] = static_cast<double>(mem.firstSeenMs);
        memT["last_seen_ms"] = static_cast<double>(mem.lastSeenMs);
        memT["lk_x"] = static_cast<double>(mem.lastKnown.x);
        memT["lk_y"] = static_cast<double>(mem.lastKnown.y);
        memT["has_last_known"] = mem.hasLastKnown;
        memT["aim_err_deg"] = static_cast<double>(mem.aimErrDeg);
        memT["next_cover_ok_ms"] = static_cast<double>(mem.nextCoverOkMs);
        memT["last_hp_seen"] = static_cast<double>(mem.lastHpSeen);

        sol::table tune = sv.create_table();
        tune["react_ms"] = static_cast<double>(t.reactMs);
        tune["err_start_deg"] = static_cast<double>(t.errStartDeg);
        tune["err_end_deg"] = static_cast<double>(t.errEndDeg);
        tune["err_decay_deg_per_s"] = static_cast<double>(t.errDecayDegPerS);
        tune["turn_rate_deg_s"] = static_cast<double>(t.turnRateDegS);
        tune["forget_ms"] = static_cast<double>(t.forgetMs);
        tune["engage_range_cm"] = static_cast<double>(t.engageRangeCm);
        tune["hp_cover_below"] = static_cast<double>(t.hpCoverBelow);
        tune["move_speed_cm_s"] = static_cast<double>(t.moveSpeedCmS);
        tune["fire_interval_ms"] = static_cast<double>(t.fireIntervalMs);
        // FSM 语义常量同路注入（§4.6：值唯一真相源=bot_brain.h detail 常量——脚本零 local 常量）
        tune["fire_cone_deg"] = static_cast<double>(mc::detail::kFireConeDeg);
        tune["cover_window_ms"] = static_cast<double>(mc::detail::kCoverWindowMs);
        tune["retreat_dist_cm"] = static_cast<double>(mc::detail::kRetreatDistCm);
        tune["jitter_period_ms"] = static_cast<double>(mc::detail::kJitterPeriodMs);
        tune["arrive_radius_cm"] = static_cast<double>(mc::detail::kArriveRadiusCm);

        auto r = f(view, stT, memT, static_cast<double>(nowMs), tune);
        if (!r.valid()) {
            sol::error e = r;
            rt_->CountFallback();
            LOG_WARNING(std::string("[Lua] bot_ai pcall 失败→内建兜底: ") + e.what());
            return builtin_.Decide(w, state, mem, nowMs, t);
        }
        if (r.get_type() != sol::type::table) {
            rt_->CountFallback();
            LOG_WARNING("[Lua] bot_ai 未返回意图表→内建兜底");
            return builtin_.Decide(w, state, mem, nowMs, t);
        }
        sol::table intent = r;
        // 读回状态（脚本就地改写）
        const int ns = stT["s"].get_or(0);
        state = (ns >= 0 && ns <= 3) ? static_cast<mc::BotState>(ns) : state;
        mem.firstSeenMs = static_cast<std::uint64_t>(memT["first_seen_ms"].get_or(0.0));
        mem.lastSeenMs = static_cast<std::uint64_t>(memT["last_seen_ms"].get_or(0.0));
        mem.lastKnown.x = static_cast<float>(memT["lk_x"].get_or(0.0));
        mem.lastKnown.y = static_cast<float>(memT["lk_y"].get_or(0.0));
        mem.hasLastKnown = memT["has_last_known"].get_or(false);
        mem.aimErrDeg = static_cast<float>(memT["aim_err_deg"].get_or(0.0));
        mem.nextCoverOkMs = static_cast<std::uint64_t>(memT["next_cover_ok_ms"].get_or(0.0));
        mem.lastHpSeen = static_cast<float>(memT["last_hp_seen"].get_or(100.0));

        mc::BotIntent out;
        out.hasMove = intent["has_move"].get_or(false);
        out.moveTarget.x = static_cast<float>(intent["move_x"].get_or(0.0));
        out.moveTarget.y = static_cast<float>(intent["move_y"].get_or(0.0));
        out.aimYawDeg = static_cast<float>(intent["aim_yaw_deg"].get_or(0.0));
        out.bFire = intent["fire"].get_or(false);
        // 出口 sanitize（双路径共用——NaN/越界在此收口，A3）
        return mc::BotIntentSanitizer::Sanitize(out);
    } catch (...) {
        rt_->CountFallback();
        LOG_WARNING("[Lua] bot_ai 异常→内建兜底");
        return builtin_.Decide(w, state, mem, nowMs, t);
    }
}

std::string LuaRuntime::LastGoodSource(std::string_view name) const {
    const auto it = lastSource_.find(std::string(name));
    return it == lastSource_.end() ? std::string() : it->second;
}

} // namespace adapters

#endif // SIGHTLINE_HAS_LUA
