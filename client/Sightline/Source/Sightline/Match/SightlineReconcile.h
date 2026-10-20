// SightlineReconcile.h —— 三方对账纯决策逻辑（零 UE 依赖，可脱离引擎单测）
// 职责：给定权威位 + 输入历史 + ε，输出和解动作（None / FullSnap / ErrorTranslate）。
//       状态机三条分支的判定收归此文件——MatchSession 只做副作用（委托广播/统计/日志）。
// 测试：tests/test_reconcile.cpp 独立编译（g++ -std=c++20），与服务端 test_match 同风格。
#pragma once
#include <cmath>
#include <cstdint>

namespace sightline::reconcile {

/** 输入存根（与 MatchSession::FInputSample 同构——纯数据，无引擎类型） */
struct InputSample {
    std::uint32_t seq = 0;
    float x = 0.f, y = 0.f, z = 0.f;
};

/** 和解决策 */
struct Decision {
    enum class Action : std::uint8_t {
        None,           // ε 内无感通过（只更新服务器认知，不碰 Pawn）
        FullSnap,       // 断档：全量回滚到权威位（预测弃置，重放 0 帧）
        ErrorTranslate  // ε 外：误差向量平移（位移空间重放）
    };
    Action action = Action::None;
    float errorX = 0.f, errorY = 0.f, errorZ = 0.f;  // ErrorTranslate: 误差向量；FullSnap: 权威位
    int replayFrames = 0;                             // 其后本地输入帧数（ErrorTranslate 有意义）
    double errCm = 0.0;                               // 对账误差模长（cm）
};

/**
 * 纯逻辑三方对账：权威位 vs 输入历史同 seq 样本。
 * @param authX/Y/Z   服务器权威位（self 实体快照）
 * @param history     输入历史环形缓冲（按 seq 升序）
 * @param historyCount 历史条数
 * @param ackedSeq    服务器确认的最新输入序号
 * @param epsilonCm   无感阈值（cm）
 */
inline Decision Evaluate(
    float authX, float authY, float authZ,
    const InputSample* history, int historyCount,
    std::uint32_t ackedSeq,
    float epsilonCm)
{
    Decision d;

    // 线性查找 ackedSeq（≤64 条，O(n) 足够）
    int idx = -1;
    for (int i = 0; i < historyCount; ++i) {
        if (history[i].seq == ackedSeq) { idx = i; break; }
    }

    if (idx < 0) {
        // 断档：全量回滚到权威位（预测弃置）
        d.action = Decision::Action::FullSnap;
        d.errorX = authX; d.errorY = authY; d.errorZ = authZ;
        d.replayFrames = 0;
        d.errCm = historyCount > 0
            ? std::sqrt(
                  (authX - history[historyCount - 1].x) * (authX - history[historyCount - 1].x) +
                  (authY - history[historyCount - 1].y) * (authY - history[historyCount - 1].y) +
                  (authZ - history[historyCount - 1].z) * (authZ - history[historyCount - 1].z))
            : 0.0;
        return d;
    }

    // 对账误差 = 权威位 - 本地同 seq 预测位
    const float ex = authX - history[idx].x;
    const float ey = authY - history[idx].y;
    const float ez = authZ - history[idx].z;
    d.errCm = std::sqrt(ex * ex + ey * ey + ez * ez);
    d.replayFrames = historyCount - 1 - idx;

    if (d.errCm < epsilonCm) {
        d.action = Decision::Action::None;
        return d;
    }

    d.action = Decision::Action::ErrorTranslate;
    d.errorX = ex; d.errorY = ey; d.errorZ = ez;
    return d;
}

} // namespace sightline::reconcile
