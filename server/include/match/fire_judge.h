// fire_judge.h —— 第 5 期开火命中纯决策头（整洁架构实体层：零框架 import，脱离网络/引擎纯单测）
// 工艺同源：SightlineReconcile.h（ep04 客户端纯决策头）——核心规则住纯函数，用例只做编排
// 内容（ep05 设计草案 §3/§4，图=代码一致 fire_sequence/hitscan_math）：
//   ① 武器状态与门控（②换弹含到期补满 / ③弹匣触发换弹 / ④射速容差 95ms<D5-14）
//   ② 解析几何求交（垂直圆柱实体 / 带姿态盒 OBB / 球——几何合同 v1 的三种原语，D5-08v2）
//   ③ CastRay 总裁定：先遮挡后实体，取最近 t（先墙后人）；伤害应用钳 0 不死（D5-02v2）
// 依赖规则：本头不得 include net/* / wire 协议 / 任何 IO——时间以 nowMs 参数注入（域不持钟）
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>

namespace sightline::match {

// ===== 合同常量（D5-07：数值表化=ep08/ep07 演进，本期住这里）=====
inline constexpr float        kDamagePerShot      = 25.f;    // 命中扣血（100 血 4 枪见底）
inline constexpr float        kMaxHp              = 100.f;
inline constexpr int          kMagCapacity        = 30;      // 弹匣（D5-07）
inline constexpr std::uint64_t kReloadMs          = 2500;    // 打空自动换弹（服务器权威计时）
inline constexpr std::uint64_t kFireMinIntervalMs = 95;      // 服务器阈值 < 客户端步进 100ms（D5-14 容差吸抖动）
inline constexpr float        kMaxRangeUu         = 1000000.f; // 10km=数值护栏（float 精度安全上限）——实际无距离限制（用户裁决 2026-10-27：去除射程限制），任何对局地图不可能触及
inline constexpr float        kEyeForwardOffset   = 18.f;    // 客户端相机 (18,0,82)——origin 对齐 §4.2
inline constexpr float        kEyeHeight          = 82.f;
inline constexpr float        kDefaultHitboxRadius = 40.f;   // entityHitbox 烘焙缺失时的兜底
inline constexpr float        kDefaultHitboxHalfH  = 90.f;
inline constexpr std::size_t  kMaxOccluders       = 64;      // 几何合同数量上限（loader 校验）
inline constexpr float        kPi                  = 3.14159265358979323846f;

// ===== 几何合同（纯数据；MapGeometryLoader 适配器从 JSON 填充，D5-08v2）=====
struct OccluderBox {
    float cx = 0.f, cy = 0.f, cz = 0.f;   // 中心（世界系 uu）
    float hx = 0.f, hy = 0.f, hz = 0.f;   // 半尺寸
    float yawDeg = 0.f;                   // 仅绕 Z 旋转（竖直盒；pitch/roll 不支持——D5-08v2 边界）
};
struct OccluderSphere {
    float cx = 0.f, cy = 0.f, cz = 0.f;
    float r = 0.f;
};
struct OccluderTable {
    std::vector<OccluderBox> boxes;
    std::vector<OccluderSphere> spheres;
};
struct EntityHitbox {
    float radius = kDefaultHitboxRadius;   // 命中体 = 角色胶囊真身（烘焙值，非硬编码）
    float halfHeight = kDefaultHitboxHalfH;
};
struct MapGeometry {
    OccluderTable occluders;
    EntityHitbox hitbox;
};

// ===== 武器状态（挂在 Entity 上；四道门的 ②③④ 作用对象）=====
struct WeaponState {
    float hp = kMaxHp;                                   // 命中扣血钳 0 不死（ep07 才有死亡判定）
    int magUsed = 0;                                     // 0..kMagCapacity
    std::uint64_t lastFireMs = 0;
    std::uint64_t reloadUntilMs = 0;
    bool bHasFired = false;                              // 首发免射速门（lastFireMs=0 是合法时刻）
};

enum class FireGate { Ok, Reloading, MagEmpty, RateLimited };

/** 门 ②③④（消费式：通过即更新状态）。②③=换弹状态机（触发/进行/到期补满，reloadUntil==0 卫哨防
 *  未触发先补满的边角——审计修复①）；④射速容差 95ms<D5-14。
 *  ①"在对局中"由用例层判定（实体查找），不在此。静默语义（无回执）= D5-03，ep08 起违规可见。 */
inline FireGate ConsumeWeaponGate(WeaponState& w, std::uint64_t nowMs) {
    if (w.magUsed >= kMagCapacity) {                               // ②③ 换弹状态机
        if (w.reloadUntilMs == 0) {                                // ③ 弹匣空 → 触发换弹
            w.reloadUntilMs = nowMs + kReloadMs;
            return FireGate::MagEmpty;
        }
        if (nowMs < w.reloadUntilMs) return FireGate::Reloading;   // ② 换弹中
        w.magUsed = 0;                                             // ② 到期就地补满
    }
    if (w.bHasFired && nowMs - w.lastFireMs < kFireMinIntervalMs)  // ④ 射速窗口（容差 95ms）
        return FireGate::RateLimited;
    return FireGate::Ok;
}

inline float DamageApply(float hp) { return std::max(0.f, hp - kDamagePerShot); }   // 钳 0 不死（D5-02v2）

// ===== 解析几何（§4.1/§4.4/§4.5——全部解析解，无物理引擎 D5 叙事）=====
struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

/** §4.1 方向重建（合同公式，双端一致；客户端准星=相机 forward=同一 yaw/pitch——天然同源） */
inline Vec3 DirectionFromYawPitch(float yawDeg, float pitchDeg) {
    const float ry = yawDeg * kPi / 180.f;
    const float rp = pitchDeg * kPi / 180.f;
    return {std::cos(rp) * std::cos(ry), std::cos(rp) * std::sin(ry), std::sin(rp)};
}

/** §4.2 射线起点：权威 pos + Rot(yaw)·(18,0,0) + (0,0,82)（对齐客户端相机位；pitch 分量前移忽略=取舍记录） */
inline Vec3 RayOriginFromEntity(float px, float py, float pz, float yawDeg) {
    const float ry = yawDeg * kPi / 180.f;
    return {px + std::cos(ry) * kEyeForwardOffset, py + std::sin(ry) * kEyeForwardOffset, pz + kEyeHeight};
}

/** 线-竖直圆柱（实体命中体/§4.4）：XY 圆 ∩ Z 区间，取 [0, Lmax] 内最小 t。
 *  逐段解析：圆柱区间与 Z 区间求交——垂直射线（D.xy≈0）单列，防 NaN。 */
inline std::optional<float> RayCylinderVertical(const Vec3& o, const Vec3& d,
                                                float cx, float cy, float cz,
                                                float radius, float halfHeight) {
    const float ox = o.x - cx, oy = o.y - cy, oz = o.z - cz;
    const float a = d.x * d.x + d.y * d.y;
    float cylLo = 0.f, cylHi = -1.f;
    if (a < 1e-12f) {                                   // 近垂直：看原点是否在圆内
        if (ox * ox + oy * oy > radius * radius) return std::nullopt;
        cylLo = 0.f; cylHi = kMaxRangeUu;               // 在圆内：Z 区间即约束
    } else {
        const float b = 2.f * (ox * d.x + oy * d.y);
        const float c = ox * ox + oy * oy - radius * radius;
        const float disc = b * b - 4.f * a * c;
        if (disc < 0.f) return std::nullopt;
        const float sq = std::sqrt(disc);
        const float t1 = (-b - sq) / (2.f * a);
        const float t2 = (-b + sq) / (2.f * a);
        if (t2 < 0.f) return std::nullopt;              // 圆柱在射线背后
        cylLo = t1 > 0.f ? t1 : 0.f;                    // 原点在圆内 → 从 0 起
        cylHi = t2;
    }
    float zLo = 0.f, zHi = kMaxRangeUu;
    if (std::fabs(d.z) < 1e-12f) {
        if (oz < cz - halfHeight || oz > cz + halfHeight) return std::nullopt;
    } else {
        float t1 = (cz - halfHeight - oz) / d.z;
        float t2 = (cz + halfHeight - oz) / d.z;
        if (t1 > t2) std::swap(t1, t2);
        if (t2 < 0.f) return std::nullopt;
        zLo = t1 > 0.f ? t1 : 0.f;
        zHi = t2;
    }
    const float lo = std::max(cylLo, zLo);
    const float hi = std::min(cylHi, zHi);
    if (lo > hi) return std::nullopt;
    return lo;
}

/** 线-带姿态盒（遮挡原语·OBB）：把射线变换进盒局部系（绕 Z 反旋 yaw）→ 复用 slab 法。
 *  旋转过的下载地图 mesh 不再"变胖"（D5-08v2）；pitch/roll 盒不支持（合同边界）。 */
inline std::optional<float> RayBox(const Vec3& o, const Vec3& d, const OccluderBox& b) {
    const float ry = b.yawDeg * kPi / 180.f;
    const float c = std::cos(ry), s = std::sin(ry);
    const float dx = o.x - b.cx, dy = o.y - b.cy, dz = o.z - b.cz;
    const Vec3 lo{ c * dx + s * dy, -s * dx + c * dy, dz };      // Rz(-yaw)·(O-C)
    const Vec3 ld{ c * d.x + s * d.y, -s * d.x + c * d.y, d.z }; // Rz(-yaw)·D
    const float h[3] = {b.hx, b.hy, b.hz};
    const float lp[3] = {lo.x, lo.y, lo.z};
    const float ldv[3] = {ld.x, ld.y, ld.z};
    float tLo = 0.f, tHi = kMaxRangeUu;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(ldv[i]) < 1e-12f) {
            if (lp[i] < -h[i] || lp[i] > h[i]) return std::nullopt;   // 平行且在滑板外
            continue;
        }
        float t1 = (-h[i] - lp[i]) / ldv[i];
        float t2 = ( h[i] - lp[i]) / ldv[i];
        if (t1 > t2) std::swap(t1, t2);
        tLo = std::max(tLo, t1);
        tHi = std::min(tHi, t2);
        if (tLo > tHi) return std::nullopt;
    }
    return tLo;                                              // 原点在盒内 → tLo=0（全遮挡，罕见且安全）
}

/** 线-球（遮挡原语/§4.4 同族一元二次；D 归一化前提下 disc = b² - c） */
inline std::optional<float> RaySphere(const Vec3& o, const Vec3& d, const OccluderSphere& s) {
    const float ox = o.x - s.cx, oy = o.y - s.cy, oz = o.z - s.cz;
    const float bq = 2.f * (ox * d.x + oy * d.y + oz * d.z);
    const float cq = ox * ox + oy * oy + oz * oz - s.r * s.r;
    const float disc = bq * bq - 4.f * cq;
    if (disc < 0.f) return std::nullopt;
    const float sq = std::sqrt(disc);
    float t = (-bq - sq) * 0.5f;
    if (t < 0.f) t = (-bq + sq) * 0.5f;                      // 原点在球内 → 从穿出面起
    if (t < 0.f || t > kMaxRangeUu) return std::nullopt;
    return t;
}

/** 求交候选（域层纯数据视图：用例从 Entity 投影，bHasReport 者才入列） */
struct EntityPose {
    std::uint32_t playerId = 0;
    float x = 0.f, y = 0.f, z = 0.f;
};

struct FireVerdict {
    bool hit = false;
    std::uint32_t victimPid = 0;
    float hitX = 0.f, hitY = 0.f, hitZ = 0.f;
};

/** §4.4/§4.5 总裁定：先遮挡原语（盒/球）后实体圆柱，取最近 t（先墙后人）。
 *  未命中 hitPoint = 射线终点（墙面交点或 Lmax 端点——tracer 停在墙上，视觉正确）。
 *  射手自己由调用方排除（自伤不存在）；18 号已离场者不在候选列。 */
inline FireVerdict CastRay(const Vec3& o, const Vec3& d, const OccluderTable& occ,
                           const std::vector<EntityPose>& others, const EntityHitbox& hb) {
    FireVerdict v;
    float tBest = kMaxRangeUu;
    for (const auto& b : occ.boxes) {
        if (auto t = RayBox(o, d, b); t && *t < tBest) tBest = *t;
    }
    for (const auto& s : occ.spheres) {
        if (auto t = RaySphere(o, d, s); t && *t < tBest) tBest = *t;
    }
    for (const auto& e : others) {
        if (auto t = RayCylinderVertical(o, d, e.x, e.y, e.z, hb.radius, hb.halfHeight);
            t && *t < tBest) {
            tBest = *t;
            v.hit = true;
            v.victimPid = e.playerId;
        }
    }
    v.hitX = o.x + d.x * tBest;
    v.hitY = o.y + d.y * tBest;
    v.hitZ = o.z + d.z * tBest;
    return v;
}

} // namespace sightline::match
