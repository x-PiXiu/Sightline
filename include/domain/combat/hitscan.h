// domain/combat/hitscan.h —— 射线判定（纯函数，可单测）
// hitscan：开火瞬间沿视线做射线检测，没有弹道飞行时间。
// 数学：slab 法求射线与轴对齐包围盒（AABB）的相交区间。

#pragma once
#include <vector>
#include <limits>
#include <utility>
#include <algorithm>
#include "domain/types.h"

namespace sightline::domain::combat {

// 候选目标：一个玩家抽象成一个 AABB（中心 + 半尺寸）
struct TargetBox {
    PlayerId id = 0;
    Vec3 center;
    Vec3 half{0.4f, 0.9f, 0.4f};   // 人形近似：宽0.8 高1.8（中心在腰部）
};

struct RayHit {
    bool hit = false;
    PlayerId target = 0;     // 命中的玩家
    float distance = 0.f;    // 命中点距离（用于取"最近"目标）
    Vec3 point;              // 命中点
};

// 射线（origin, dir|dir|=1）与 AABB 相交检测；命中时 tNear 写入最近相交距离
// 命中判定条件：tNear >= 0（在射击者身前）且区间非空
inline bool rayIntersectsAABB(const Vec3& origin, const Vec3& dir,
                              const Vec3& center, const Vec3& half, float& tNear) {
    float tMin = 0.f;
    float tMax = std::numeric_limits<float>::max();

    const float o[3] = {origin.x, origin.y, origin.z};
    const float d[3] = {dir.x, dir.y, dir.z};
    const float c[3] = {center.x, center.y, center.z};
    const float h[3] = {half.x, half.y, half.z};

    for (int i = 0; i < 3; ++i) {
        if (d[i] == 0.f) {
            // 射线与该轴平行：起点在包围盒外则永不相交
            if (o[i] < c[i] - h[i] || o[i] > c[i] + h[i]) return false;
            continue;
        }
        float t1 = (c[i] - h[i] - o[i]) / d[i];
        float t2 = (c[i] + h[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tMin = std::max(tMin, t1);
        tMax = std::min(tMax, t2);
        if (tMin > tMax) return false;
    }
    tNear = tMin;
    return true;
}

// 从所有候选中选"最近的被击中者"（子弹不会穿人）
inline RayHit pickNearestTarget(const Vec3& origin, const Vec3& dir,
                                const std::vector<TargetBox>& candidates) {
    RayHit best;
    float bestDist = std::numeric_limits<float>::max();
    for (const auto& t : candidates) {
        float d = 0.f;
        if (rayIntersectsAABB(origin, dir, t.center, t.half, d) && d < bestDist) {
            bestDist = d;
            best.hit = true;
            best.target = t.id;
            best.distance = d;
            best.point = origin + dir * d;
        }
    }
    return best;
}

} // namespace sightline::domain::combat
