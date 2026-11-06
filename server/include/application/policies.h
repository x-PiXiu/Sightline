// policies.h —— ep08 步骤 2：策略端口（整洁架构：接口归用例层所有，实现归外层）
// v1.5 修正：旧稿把这两个端口标为「实体层」；按手册 §四「接口归高层（用例）所有」，
// 端口统一住 include/application/。实体层只留纯规则与值对象。
// 本头零脚本引擎痕迹；步骤 4 的 Lua* 实现位于适配器层的唯一翻译单元。
#pragma once
#include <cstdint>

#include "match/bot_brain.h"
#include "match/fire_judge.h"
#include "match/invariants.h"

namespace sightline::application {

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
// 步骤 2 只提供实现、不接线，MatchService::Fire 仍走原路径 => 238 断言行为零变化。
class BuiltinDamagePolicy final : public IDamagePolicy {
public:
    float Apply(float hp, float base, const HitContext&) override {
        return sightline::match::DamageInvariants::Clamp(hp, hp - base);
    }
};

struct IBotPolicy {
    virtual ~IBotPolicy() = default;
    virtual sightline::match::BotIntent Decide(
        const sightline::match::BotWorldView& w,
        sightline::match::BotState& state,
        sightline::match::BotMemory& mem,
        std::uint64_t nowMs,
        const sightline::match::BotTuning& t) = 0;
};

class BuiltinBotPolicy final : public IBotPolicy {
public:
    sightline::match::BotIntent Decide(
        const sightline::match::BotWorldView& w,
        sightline::match::BotState& state,
        sightline::match::BotMemory& mem,
        std::uint64_t nowMs,
        const sightline::match::BotTuning& t) override {
        return sightline::match::BotIntentSanitizer::Sanitize(
            sightline::match::Decide(w, state, mem, nowMs, t));
    }
};

} // namespace sightline::application
