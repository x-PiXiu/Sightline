// perception.h —— ep07 人机感知（域层纯函数·零依赖）
// 原则（ep07 设计草案 §3.2 D7-06）：人机看见的 = 子弹打得到的——视线判定复用 fire_judge 的
// 同一套遮挡求交（RayBox/RaySphere），结构上杜绝"透视"（它看不见的就打不着，反之亦然）。
// 反应延迟不用计时器：查目标历史环 (now − 250ms) 的位置——ep06 数据结构直接复用（零新状态）。
#pragma once
#include <optional>

#include "match/fire_judge.h"

namespace sightline::match {

/** 视线判定：人机眼位 → 目标胸口，之间无遮挡原语先于目标命中 = 可见。
 *  眼位复用射线合同（RayOriginFromEntity·与射击同源——所见即所及）。 */
inline bool CanSee(const OccluderTable& occ, const Vec3& fromPos, float fromYaw,
                   const Vec3& targetChest) {
    const Vec3 eye = RayOriginFromEntity(fromPos.x, fromPos.y, fromPos.z, fromYaw);
    Vec3 d{targetChest.x - eye.x, targetChest.y - eye.y, targetChest.z - eye.z};
    const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (dist < 1e-3f) return true;
    d.x /= dist; d.y /= dist; d.z /= dist;
    float wallT = std::numeric_limits<float>::infinity();
    for (const auto& b : occ.boxes)
        if (auto t = RayBox(eye, d, b); t && *t < wallT) wallT = *t;
    for (const auto& s : occ.spheres)
        if (auto t = RaySphere(eye, d, s); t && *t < wallT) wallT = *t;
    return wallT >= dist;                        // 遮挡命中点比目标远 = 中间无墙
}

} // namespace sightline::match
