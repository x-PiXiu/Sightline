// invariants.h —— ep08 步骤 2：实体层域不变式（零框架 import / 零 IO）
// §4.3 / §4.7 第二层：C++ 内建策略与 Lua 策略**两条路径出适配器前共用**，
// 避免「不变式只保护 Lua 一条路径」，也让热修门禁有唯一实现。
#pragma once
#include <algorithm>
#include <cmath>

#include "match/bot_brain.h"    // BotIntent / Vec2
#include "match/fire_judge.h"   // kPlayAreaHalfUu

namespace sightline::match {

struct DamageInvariants {
    // 输入 hp（当前血量）与 hpAfter（策略返回的**扣血后余血**，不是伤害量），
    // 产出被钳制后的余血：有限、∈ [0,hp]、对 hp 单调不增。
    // NaN / +Inf / -Inf 一律视作「非有限 → 不掉血」= 返回 hp；调用方另计告警并回退内建策略（§4.4）。
    // 注意 -Inf 不是「负余血」而是非法值，不按负数钳到 0（否则会静默把脚本 bug 变成击杀）。
    static float Clamp(float hp, float hpAfter) {
        const float upper = std::max(0.0f, hp);
        if (!std::isfinite(hpAfter)) return upper;
        if (hpAfter > upper) return upper;
        if (hpAfter < 0.0f) return 0.0f;
        return hpAfter;
    }
};

struct BotIntentSanitizer {
    // 坐标 clamp 到活动区 / aimYawDeg NaN 兜底；枚举越界由调用方 switch default 兜底。
    static BotIntent Sanitize(BotIntent in, float playHalfUu = kPlayAreaHalfUu) {
        const auto clampAxis = [playHalfUu](float v) {
            if (!std::isfinite(v)) return 0.0f;
            return std::clamp(v, -playHalfUu, playHalfUu);
        };
        in.moveTarget.x = clampAxis(in.moveTarget.x);
        in.moveTarget.y = clampAxis(in.moveTarget.y);
        if (!std::isfinite(in.aimYawDeg)) in.aimYawDeg = 0.0f;
        return in;
    }
};

} // namespace sightline::match
