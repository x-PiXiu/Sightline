// test_reconcile.cpp —— 三方对账纯决策单测（零 UE 依赖，脱离引擎编译）
// 编译：g++ -std=c++20 test_reconcile.cpp -o /tmp/test_reconcile && /tmp/test_reconcile
// 覆盖：断档→FullSnap｜ε 内→None｜ε 外→ErrorTranslate｜边界值｜空历史｜replayFrames 计数
// 位置纪律：住 Source 树外（Tests/）——放 Source 里会被 UBT 扫进模块（重复 main+宏冲突）
#include "../Source/Sightline/Match/SightlineReconcile.h"
#include <cmath>
#include <cstdio>
#include <vector>

static int g_checked = 0;
static int g_stage = 0;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        ++g_checked;                                                                    \
        if (!(cond)) {                                                                  \
            std::printf("✗ 断言失败（第 %d 项）: %s\n", g_checked, #cond);              \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

static void stage(const char* name) {
    ++g_stage;
    std::printf("—— %s ——（已过 %d 项）\n", name, g_checked);
}

static bool NearlyEqual(double a, double b, double eps = 0.01) {
    return std::fabs(a - b) < eps;
}

int main() {
    using namespace sightline::reconcile;

    // 构造输入历史：seq=10~14，位置递增
    const InputSample history[] = {
        {10,  0.f,   0.f, 0.f},
        {11, 10.f,   0.f, 0.f},
        {12, 20.f,   0.f, 0.f},
        {13, 30.f,   0.f, 0.f},
        {14, 40.f,   0.f, 0.f},
    };
    constexpr int kCount = 5;
    constexpr float kEps = 5.f;

    stage("ε 内无感通过：权威位与预测位误差 < 5cm → None");
    {
        auto d = Evaluate(20.3f, 0.1f, 0.f, history, kCount, 12, kEps);
        CHECK(d.action == Decision::Action::None);
        CHECK(NearlyEqual(d.errCm, 0.32, 0.1));
        CHECK(d.replayFrames == 2);   // idx=2，其后 13/14 两帧
    }

    stage("ε 外误差平移：权威位偏移 > 5cm → ErrorTranslate");
    {
        auto d = Evaluate(35.f, 8.f, 0.f, history, kCount, 13, kEps);
        CHECK(d.action == Decision::Action::ErrorTranslate);
        CHECK(NearlyEqual(d.errorX, 5.f));
        CHECK(NearlyEqual(d.errorY, 8.f));
        CHECK(NearlyEqual(d.errCm, 9.43, 0.1));
        CHECK(d.replayFrames == 1);   // idx=3，其后 14 一帧
    }

    stage("断档兜底：ackedSeq 不在历史中 → FullSnap");
    {
        auto d = Evaluate(99.f, 88.f, 77.f, history, kCount, 999, kEps);
        CHECK(d.action == Decision::Action::FullSnap);
        CHECK(NearlyEqual(d.errorX, 99.f));
        CHECK(NearlyEqual(d.errorY, 88.f));
        CHECK(NearlyEqual(d.errorZ, 77.f));
        CHECK(d.replayFrames == 0);
        // errCm = 到最后一条样本(40,0,0)的距离 = sqrt(59²+88²+77²) ≈ 131.4
        CHECK(NearlyEqual(d.errCm, 131.4, 1.0));
    }

    stage("空历史：断档 + errCm=0");
    {
        auto d = Evaluate(50.f, 0.f, 0.f, nullptr, 0, 1, kEps);
        CHECK(d.action == Decision::Action::FullSnap);
        CHECK(NearlyEqual(d.errCm, 0.0));
    }

    stage("边界值：误差恰好等于 ε → ErrorTranslate（≥ε 触发）");
    {
        auto d = Evaluate(25.f, 0.f, 0.f, history, kCount, 12, kEps);  // err=5.0
        CHECK(d.action == Decision::Action::ErrorTranslate);
    }

    stage("边界值：误差恰好 ε-0.01 → None");
    {
        auto d = Evaluate(24.99f, 0.f, 0.f, history, kCount, 12, kEps);  // err=4.99
        CHECK(d.action == Decision::Action::None);
    }

    stage("replayFrames 计数：最早 seq 的重放量 = 条数-1");
    {
        auto d = Evaluate(0.f, 0.f, 0.f, history, kCount, 10, kEps);  // idx=0
        CHECK(d.replayFrames == 4);
    }

    stage("replayFrames 计数：最晚 seq 的重放量 = 0");
    {
        auto d = Evaluate(40.f, 0.f, 0.f, history, kCount, 14, kEps);  // idx=4
        CHECK(d.replayFrames == 0);
    }

    stage("瞬发阈值（拉扯第五轮）：小额 bInstant=false 走尾款平滑 / ≥150cm 或 FullSnap → bInstant=true");
    {
        auto small = Evaluate(35.f, 8.f, 0.f, history, kCount, 13, kEps);        // err≈9.4cm
        CHECK(small.action == Decision::Action::ErrorTranslate);
        CHECK(!small.bInstant);                                                  // 常规残余 → 尾款平滑
        auto big = Evaluate(200.f, 0.f, 0.f, history, kCount, 14, kEps);         // err=160cm
        CHECK(big.action == Decision::Action::ErrorTranslate);
        CHECK(big.bInstant);                                                     // 传送级分歧 → 瞬发
        auto edge = Evaluate(190.f, 0.f, 0.f, history, kCount, 14, kEps);        // err=150.0 恰好
        CHECK(edge.bInstant);                                                    // ≥ 生效
        auto edgeMinus = Evaluate(189.9f, 0.f, 0.f, history, kCount, 14, kEps);  // err=149.9
        CHECK(!edgeMinus.bInstant);
        auto snap = Evaluate(99.f, 88.f, 77.f, history, kCount, 999, kEps);      // 断档
        CHECK(snap.action == Decision::Action::FullSnap && snap.bInstant);       // FullSnap 恒瞬发
    }

    std::printf("全部通过：%d 项断言 ✓（和解纯决策，未起引擎）\n", g_checked);
    return 0;
}
