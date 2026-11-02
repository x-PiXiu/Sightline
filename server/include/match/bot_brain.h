// bot_brain.h —— ep07 人机决策（域层实体层·纯函数·零依赖）
// 整洁架构判定（决策树 A#2）：「看见敌人→转身→开火→血少撤退」去掉软件仍成立=业务规则 → 实体层，
// 不是适配器。同输入同输出，脱离网络/引擎纯单测 FSM 全转移（ep07 设计草案 §3.2·D7-07 L0~L2）。
// 四个「像人」旋钮（可读的反应，不是最优的反应——太准=外挂观感）：
//   反应延迟 250ms（driver 感知侧采样 now−reactMs，历史环复用）
//   瞄准误差 4°→1°（随持续瞄准收敛）/ 转身速率 360°/s 上限 / 3s 无视线遗忘。
#pragma once
#include <cmath>
#include <cstdint>
#include <optional>

#include "match/fire_judge.h"   // Vec3

namespace sightline::match {

enum class BotState { Patrol, Engage, Search, TakeCover };

struct BotTuning {
    float reactMs = 250.f;          // 感知延迟（driver 采样侧使用）
    float errStartDeg = 4.f;        // 首见瞄准误差（度）
    float errEndDeg = 1.f;          // 收敛下限
    float errDecayDegPerS = 3.f;    // 收敛速率
    float turnRateDegS = 360.f;     // 转身上限
    float forgetMs = 3000.f;        // 无视线遗忘
    float engageRangeCm = 900.f;    // 交战距离（超出=追近）
    float hpCoverBelow = 30.f;      // 血量低于此→撤掩体
    float moveSpeedCmS = 400.f;     // 与角色 MaxWalkSpeed 一致
};

/** 感知输入（driver 组装·只读 DTO——MatchService 不为人机开门） */
struct BotSelf {
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f;
    float hp = 100.f;
    int magUsed = 0;
    bool reloading = false;
};
struct Vec2 { float x = 0.f, y = 0.f; };
struct SeenEnemy {                       // 感知输出（driver 已按 now−reactMs 采样）
    std::uint32_t pid = 0;
    float x = 0.f, y = 0.f;              // 采样位置（250ms 前=人类反应的具象）
    float dist = 0.f;
};
struct BotWorldView {
    BotSelf self;
    std::optional<SeenEnemy> visible;    // 遮挡过滤后的可见敌（最多取最近）
    std::optional<Vec3> lastKnown;       // 记忆位置（driver 持有·3s 遗忘）
    Vec2 patrolTarget{};                 // 当前巡逻目标（driver 航点轮转）
    float hpMax = 100.f;
};

/** 记忆（driver 持有·跨 tick 状态——纯数据） */
struct BotMemory {
    std::uint64_t firstSeenMs = 0;       // 本次交战起始（误差收敛时钟）
    std::uint64_t lastSeenMs = 0;
    Vec2 lastKnown{};
    bool hasLastKnown = false;
    float aimErrDeg = 0.f;               // 当前瞄准误差
    std::uint64_t nextCoverOkMs = 0;     // 撤退节流
};

struct BotIntent {
    bool hasMove = false;                // true=向 moveTarget 走（driver 换算步长+寻路）
    Vec2 moveTarget{};
    float aimYawDeg = 0.f;               // 期望朝向（driver 按转身上限逼近）
    bool bFire = false;                  // 朝向对准+误差锥内才 true
};

namespace detail {
inline float YawTo(float fromX, float fromY, float toX, float toY) {
    return std::atan2(toY - fromY, toX - fromX) * 180.f / 3.14159265358979323846f;
}
inline float NormDeg(float d) {
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}
} // namespace detail

/** 纯决策：世界视图 + 记忆（可变入参=状态转移）→ 意图。
 *  状态机：Patrol ──看见──► Engage ──血少──► TakeCover ──超时/脱离──► Engage
 *          Engage ──丢视线──► Search ──超时/到达──► Patrol。 */
inline BotIntent Decide(const BotWorldView& w, BotState& state, BotMemory& m, std::uint64_t nowMs,
                        const BotTuning& t = {}) {
    BotIntent out;
    // —— 全局感知落地：看见即记忆（遗忘在无视线分支处理）——
    if (w.visible) {
        // 新交战判定：距上次可见超过遗忘窗（或首次）→ 重置收敛时钟与误差
        if (m.lastSeenMs == 0 || nowMs - m.lastSeenMs > t.forgetMs) {
            m.firstSeenMs = nowMs;
            m.aimErrDeg = t.errStartDeg;
        }
        m.lastSeenMs = nowMs;
        m.lastKnown = Vec2{w.visible->x, w.visible->y};
        m.hasLastKnown = true;
    }

    // —— 血量门：低血且可见敌 → 撤退（TakeCover 简化=背向敌撤退 400cm·D7-07 L2）——
    if (w.visible && w.self.hp < t.hpCoverBelow) {
        if (state != BotState::TakeCover) { m.nextCoverOkMs = nowMs + 1500; }
        state = BotState::TakeCover;
    } else if (state == BotState::TakeCover && (!w.visible || w.self.hp >= t.hpCoverBelow)) {
        state = w.visible ? BotState::Engage : BotState::Search;
    }

    switch (state) {
    case BotState::Patrol: {
        if (w.visible) { state = BotState::Engage; break; }   // 发现 → 交战（误差重置已由感知段处理）
        out.hasMove = true;
        out.moveTarget = w.patrolTarget;
        out.aimYawDeg = detail::YawTo(w.self.x, w.self.y, w.patrolTarget.x, w.patrolTarget.y);
        break;
    }
    case BotState::Engage: {
        if (!w.visible) {                                 // 丢视线：3s 内去最后已知位置找
            if (m.hasLastKnown && nowMs - m.lastSeenMs <= t.forgetMs) {
                state = BotState::Search;
            } else {
                state = BotState::Patrol;
                m.aimErrDeg = 0.f;
            }
            break;
        }
        const SeenEnemy& e = *w.visible;
        const float dist = e.dist;
        // 瞄准：目标方向 + 收敛误差（误差随交战时长衰减）
        const float engagedS = float(nowMs - m.firstSeenMs) / 1000.f;
        m.aimErrDeg = std::max(t.errEndDeg, t.errStartDeg - t.errDecayDegPerS * engagedS);
        const float baseYaw = detail::YawTo(w.self.x, w.self.y, e.x, e.y);
        const float noiseSign = (nowMs / 700) % 2 == 0 ? 1.f : -1.f;   // 慢抖动（非逐帧噪声）
        out.aimYawDeg = baseYaw + m.aimErrDeg * noiseSign;
        // 开火条件：对准（误差锥内）+ 射程内
        const float aimDelta = std::fabs(detail::NormDeg(out.aimYawDeg - w.self.yaw));
        out.bFire = aimDelta <= 6.f && dist <= t.engageRangeCm && !w.self.reloading;
        // 走位：超出交战距离 → 追近（沿视线方向直进——寻路由 driver 栅格兜底）
        if (dist > t.engageRangeCm) {
            out.hasMove = true;
            out.moveTarget = Vec2{e.x, e.y};
        }
        break;
    }
    case BotState::Search: {
        if (w.visible) { state = BotState::Engage; break; }
        if (!m.hasLastKnown || nowMs - m.lastSeenMs > t.forgetMs) {
            state = BotState::Patrol;                     // 遗忘 → 回巡逻
            m.aimErrDeg = 0.f;
            break;
        }
        out.hasMove = true;
        out.moveTarget = m.lastKnown;
        out.aimYawDeg = detail::YawTo(w.self.x, w.self.y, m.lastKnown.x, m.lastKnown.y);
        const float dx = w.self.x - m.lastKnown.x, dy = w.self.y - m.lastKnown.y;
        if (dx * dx + dy * dy < 100.f * 100.f) {          // 到达最后已知位仍未见 → 回巡逻
            state = BotState::Patrol;
            m.hasLastKnown = false;
        }
        break;
    }
    case BotState::TakeCover: {
        if (nowMs >= m.nextCoverOkMs && (!w.visible || w.self.hp >= t.hpCoverBelow)) {
            state = w.visible ? BotState::Engage : BotState::Search;
            break;
        }
        // 背向敌人撤退（有视线=背向；无视线=向记忆点反方向）
        float ex = 0.f, ey = 0.f;
        if (w.visible) { ex = w.visible->x - w.self.x; ey = w.visible->y - w.self.y; }
        else if (m.hasLastKnown) { ex = m.lastKnown.x - w.self.x; ey = m.lastKnown.y - w.self.y; }
        const float len = std::sqrt(ex * ex + ey * ey);
        if (len > 1e-3f) {
            out.hasMove = true;
            out.moveTarget = Vec2{w.self.x - ex / len * 400.f, w.self.y - ey / len * 400.f};
        }
        out.aimYawDeg = detail::YawTo(w.self.x, w.self.y,
                                      w.visible ? w.visible->x : w.self.x + ex,
                                      w.visible ? w.visible->y : w.self.y + ey);
        break;
    }
    }
    return out;
}

} // namespace sightline::match
