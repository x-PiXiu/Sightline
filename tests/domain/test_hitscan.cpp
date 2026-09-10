// tests/domain/test_hitscan.cpp —— 射线判定单测（零网络、零 IO，g++ 直接编译即跑）

#include <cstdio>
#include <cmath>
#include "domain/combat/hitscan.h"

using namespace sightline::domain;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

int main() {
    using sightline::domain::combat::TargetBox;
    using sightline::domain::combat::pickNearestTarget;
    using sightline::domain::combat::rayIntersectsAABB;

    Vec3 origin{0, 0, 0};

    // 1. 正面命中：目标在正前方 10 米
    {
        float t = -1;
        CHECK(rayIntersectsAABB(origin, Vec3{1, 0, 0}, Vec3{10, 0, 0}, Vec3{0.4f, 0.9f, 0.4f}, t));
        CHECK(t > 9.0f && t < 10.0f);   // 命中距离 ≈ 10 - 0.4
    }

    // 2. 平行擦过：射线偏离，未命中
    {
        float t = -1;
        CHECK(!rayIntersectsAABB(origin, Vec3{1, 0, 0}, Vec3{10, 5, 0}, Vec3{0.4f, 0.9f, 0.4f}, t));
    }

    // 3. 目标在身后：tNear < 0，不应命中（向 +x 射击，目标在 -x）
    {
        std::vector<TargetBox> boxes{{2, Vec3{-10, 0, 0}, Vec3{0.4f, 0.9f, 0.4f}}};
        auto hit = pickNearestTarget(origin, Vec3{1, 0, 0}, boxes);
        CHECK(!hit.hit);
    }

    // 4. 多目标取最近
    {
        std::vector<TargetBox> boxes{
            {7, Vec3{20, 0, 0}, Vec3{0.4f, 0.9f, 0.4f}},   // 远
            {8, Vec3{5, 0, 0}, Vec3{0.4f, 0.9f, 0.4f}},    // 近
        };
        auto hit = pickNearestTarget(origin, Vec3{1, 0, 0}, boxes);
        CHECK(hit.hit);
        CHECK(hit.target == 8);
        CHECK(hit.distance > 4.0f && hit.distance < 6.0f);
    }

    // 5. 斜向命中（45 度）
    {
        std::vector<TargetBox> boxes{{9, Vec3{10, 10, 0}, Vec3{0.4f, 0.9f, 0.4f}}};
        Vec3 dir{1, 1, 0};
        auto hit = pickNearestTarget(origin, dir, boxes);   // 方向未归一化也应正确
        CHECK(hit.hit);
        CHECK(hit.target == 9);
    }

    // 6. 生产尺度回归（UE 厘米 + 默认半尺寸）：重现"贴脸脱靶"bug
    //    场景：眼睛 (0,0,165) 水平射击，目标盒中心 (-200,0,90)（UE 厘米）
    //    修复前 half={0.4,0.9,0.4}（米）→ 命中盒只有 1.8cm 高，水平瞄准也必脱靶
    {
        std::vector<TargetBox> boxes{{1, Vec3{-200, 0, 90}}};   // 默认 half = 厘米人形
        Vec3 eye{0, 0, 165};
        Vec3 to_target = boxes[0].center - eye;                 // (-200, 0, -75)
        Vec3 dir = to_target * (1.0f / std::sqrt(to_target.x * to_target.x +
                                                   to_target.y * to_target.y +
                                                   to_target.z * to_target.z));
        auto hit = pickNearestTarget(eye, dir, boxes);
        CHECK(hit.hit);
        CHECK(hit.target == 1);
    }

    if (g_failures == 0) std::printf("test_hitscan: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
