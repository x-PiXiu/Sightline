// policies.h —— ep08 步骤 2：策略端口（v1.6 归属修正）
// v1.5 曾把本组端口放 include/application/，理由「接口归用例层所有」——但端口的真正拥有者是
// **消费方**（手册 §四 DIP：接口归「高层」=调用方所有）。IDamagePolicy 的调用方是
// MatchSession::Fire、IBotPolicy 的调用方是 BotDriver，两者都在 match/ 适配链内层；
// 若端口住 application/（更高编排层），match → application 即成向上依赖（反向）。
// 故迁回 match/：application/orchestrator 只是**注入方**（依赖方向 application → match ✓），
// 步骤 4 的 Lua* 实现住适配器层（adapters → match ✓），方向全部向内。
// 本头零脚本引擎痕迹；Builtin 实现=测试桩+Lua 故障 fallback（同物两用）。
#pragma once
#include <cstdint>

#include "match/bot_brain.h"
#include "match/fire_judge.h"
#include "match/invariants.h"

namespace sightline::match {

struct HitContext {
    float dist = 0.f;
    bool headshot = false;   // G6：ep08 恒 false；爆头球 ep10+ 兑现
};

struct IDamagePolicy {
    virtual ~IDamagePolicy() = default;
    // 返回扣血后的 hp（不是伤害量）；出口一律过 DamageInvariants::Clamp。
    virtual float Apply(float hp, float base, const HitContext& ctx) = 0;
};

// 用例层默认策略 + 测试桩（不是实体）：等价于现行 DamageApply（钳 0 不死）。
class BuiltinDamagePolicy final : public IDamagePolicy {
public:
    float Apply(float hp, float base, const HitContext&) override {
        return DamageInvariants::Clamp(hp, hp - base);
    }
};

struct IBotPolicy {
    virtual ~IBotPolicy() = default;
    virtual BotIntent Decide(const BotWorldView& w, BotState& state,
                             BotMemory& mem, std::uint64_t nowMs,
                             const BotTuning& t) = 0;
};

class BuiltinBotPolicy final : public IBotPolicy {
public:
    BotIntent Decide(const BotWorldView& w, BotState& state,
                     BotMemory& mem, std::uint64_t nowMs,
                     const BotTuning& t) override {
        // ⚠ 必须限定命名空间：不写 sightline::match:: 会解析成本成员（无限递归）
        return BotIntentSanitizer::Sanitize(
            sightline::match::Decide(w, state, mem, nowMs, t));
    }
};

} // namespace sightline::match
