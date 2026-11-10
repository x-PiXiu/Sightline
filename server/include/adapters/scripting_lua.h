// scripting_lua.h —— ep08 步骤 4：Lua 运行时与策略适配（声明层）
// 纪律（§10.2）：唯一 include <sol2/sol.hpp> / <lua.h> 的翻译单元 = src/adapters/scripting_lua.cpp；
// 本头只前置声明 lua_State。内层（match/application）永不见本头——经 match/policies.h 端口对接。
// 两处对 §10.2 草稿的实勘修正（v1.9）：
//   ① 草稿的 ILogger& 未引入——现有适配器一律用 LOG_* 单例宏，不为 Lua 开第二套日志抽象；
//   ② LuaDamagePolicy 构造加 ICombatParams*——§10.2.4 脚本签名要 params（公式参数），
//      草稿单参构造没有 params 来源；指针指向组合根注入的视图，每次 Apply 读原子=热更即时。
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "application/config_views.h"   // PatchResult / ICombatParams（用例层 POD）
#include "application/ports.h"          // IPatchChannel（继承目标·PatchResult 经 config_views）
#include "match/policies.h"             // IDamagePolicy / IBotPolicy / HitContext（v1.6 归 match）

struct lua_State;

// lua_Alloc 计数上下文（B4 内存 cap）——须先于 L_ 声明：逆序析构时 lua_close 先于本结构销毁。
struct LuaAllocCtx {
    std::size_t used = 0;
    std::size_t cap = 32u * 1024 * 1024;
};

// RAII：.cpp 内调 lua_close（不完整类型处不能调）
struct LuaStateDeleter {
    void operator()(lua_State* L) const noexcept;
};
using LuaStatePtr = std::unique_ptr<lua_State, LuaStateDeleter>;

namespace adapters {

struct LuaRuntimeConfig {
    std::string scriptDir;       // §3.6 白名单：env > CWD > exe 同级 > 上级（组合根解析后传入）
    std::size_t memoryCapBytes = 32u * 1024 * 1024;   // B4
};

// Lua 运行时：沙箱状态 + 脚本加载 + 函数引用热修（IPatchChannel 实现）。
// 单线程纪律（A1）：只被主循环碰（stdin 命令队列消费点=编排器 OnFrame 开头）。
class LuaRuntime final : public sightline::application::IPatchChannel {
public:
    // E2 部署韧性：状态创建失败返回 nullptr；脚本缺失/坏=告警+内建兜底，不让服务起不来。
    static std::shared_ptr<LuaRuntime> Create(const LuaRuntimeConfig& cfg);
    ~LuaRuntime() override;

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    // §10.2.3 热修三步：编译→沙箱自测→引用 swap（旧闭包 unref=upvalue 彩蛋的正解）。
    // source 空 = 从 scriptDir/<name>.lua 重读（console `patch damage` 流——文件 IO 在适配器，
    // 编排器不碰文件系统）。
    sightline::application::PatchResult Patch(std::string_view name, std::string_view source) override;
    /** ep09 G5：当前生效源（加载时=文件初值；每次成功 patch 更新——聚合回退的恢复点） */
    std::string LastGoodSource(std::string_view name) const override;

    lua_State* L() const { return L_.get(); }
    int Ref(const std::string& name) const;          // 未加载 = -1（LUA_NOREF 同值）
    std::uint64_t Fallbacks() const { return fallbacks_; }   // pcall 失败回退内建计数（验收门 3）
    void CountFallback() { ++fallbacks_; }
    const std::string& Dir() const { return cfg_.scriptDir; }
    std::size_t UsedBytes() const { return alloc_.used; }
    std::size_t Cap() const { return alloc_.cap; }

private:
    explicit LuaRuntime(const LuaRuntimeConfig& cfg);
    bool InitSandbox();                              // lua_newstate + 选择性库 + NilGlobal
    int LoadFunction(const std::string& name);       // scriptDir/name.lua → registry ref
    int* RefSlot(const std::string& name);           // damage / bot_ai 各一（Patch 换这里）

    LuaRuntimeConfig cfg_;
    std::map<std::string, std::string> lastSource_;   // ep09 G5：脚本名→当前生效源
    LuaAllocCtx alloc_;          // 声明先于 L_：析构序 L_ 先关（见文件头注①）
    LuaStatePtr L_;
    int refDamage_ = -1;
    int refBot_ = -1;
    std::uint64_t fallbacks_ = 0;
};

// 伤害策略：sol2 调 damage.lua → 实体层 DamageInvariants::Clamp 双路径共用（§10.2.5）。
// 脚本缺失/ref=-1/pcall 失败/出口越界 → CountFallback + BuiltinDamagePolicy（E2 + §4.4）。
class LuaDamagePolicy final : public sightline::match::IDamagePolicy {
public:
    LuaDamagePolicy(std::shared_ptr<LuaRuntime> rt,
                    const sightline::application::ICombatParams* combat);
    float Apply(float hp, float base, const sightline::match::HitContext& ctx) override;

private:
    std::shared_ptr<LuaRuntime> rt_;
    const sightline::application::ICombatParams* combat_;   // Formula() 每次读（热更即时）
    sightline::match::BuiltinDamagePolicy builtin_;
};

// 决策策略：同签名换实现；ref=-1（bot_ai.lua 缺失）→ 直走内建（E2）；步 8 起默认走 Lua。
class LuaBotPolicy final : public sightline::match::IBotPolicy {
public:
    explicit LuaBotPolicy(std::shared_ptr<LuaRuntime> rt);
    sightline::match::BotIntent Decide(const sightline::match::BotWorldView& w,
                                       sightline::match::BotState& state,
                                       sightline::match::BotMemory& mem,
                                       std::uint64_t nowMs,
                                       const sightline::match::BotTuning& t) override;

private:
    std::shared_ptr<LuaRuntime> rt_;
    sightline::match::BuiltinBotPolicy builtin_;
};

} // namespace adapters
