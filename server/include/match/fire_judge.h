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
// —— FPP 相机镜像合同（2026-10-29 用户令：准星=判定射线——弹孔/命中与准星重合）——
// 服务器眼位必须与客户端 FPP 相机偏移（SightlineCameraComponent FirstPersonOffset=(8,8,62)，胶囊系）逐分量镜像；
// ⚠ 改相机偏移（BP 或代码）必须同步本组常量——这是双端瞄准合同，不是自由参数。
// TPP 模式不重合=TPP 固有视差（行业共识，PUBG 同款），TPP 下的弹着点以服务器射线为准。
inline constexpr float        kEyeForwardOffset   = 20.f;    // 胶囊系前向分量（BP FirstPersonOffset X=20）
inline constexpr float        kEyeRightOffset     = 8.f;     // 胶囊系右向分量（BP Y=+8 的镜像）
inline constexpr float        kEyeHeight          = 63.f;    // 胶囊系高度分量（BP Z=63）
inline constexpr float        kDefaultHitboxRadius = 40.f;   // entityHitbox 烘焙缺失时的兜底
inline constexpr float        kDefaultHitboxHalfH  = 90.f;
inline constexpr std::size_t  kMaxOccluders       = 64;      // 几何合同数量上限（loader 校验）
inline constexpr std::uint64_t kMaxRewindMs       = 1000;    // ep06：回溯硬上限（D6-04·防伪造高 RTT=ep09 预埋；窗口 32 槽 1.07s > 上限）
inline constexpr int           kEntityHistoryCap  = 32;      // ep06：权威位时间线窗（32×1/30s≈1.07s > 上限 1000ms）
inline constexpr std::uint64_t kRespawnMs         = 3000;    // ep07：死亡→重生倒计时（服务器权威）
inline constexpr std::uint64_t kInvulnMs          = 2000;    // ep07：重生无敌（防出生即死·行业惯例）
inline constexpr int           kKillLimit         = 10;      // ep07：先到 10 杀结束（D7-02·ep08 进数值表）
inline constexpr float         kNavCellCm         = 50.f;    // ep07：导航栅格粒度（80×80@4000² 地图）
inline constexpr float         kNavStepCm         = 45.f;    // ep07：可跨越台阶高（低于此的盒不挡路）
inline constexpr float         kNavBodyTopCm      = 176.f;   // ep07：身体高度带顶（88×2——挡路判定用）
inline constexpr float         kNavInflateCm      = 34.f;    // ep07：障碍按命中体半径膨胀（贴墙不卡胶囊）
inline constexpr float        kPi                  = 3.14159265358979323846f;

struct Vec3 {
    float x = 0.f, y = 0.f, z = 0.f;
};

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
struct SpawnPoint {                                    // ep07：出生/重生点（烘焙自 PlayerStart）
    float x = 0.f, y = 0.f, z = 90.f;
    float yawDeg = 0.f;
};
struct MapGeometry {
    OccluderTable occluders;
    EntityHitbox hitbox;
    std::vector<SpawnPoint> spawnPoints;               // ep07：服务器权威重生选点池（缺=回退场心 (0,0,90)）
    std::vector<Vec3> patrolPoints;                    // ep07：人机巡逻点（烘焙自 TargetPoint·可选）
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
/** §4.1 方向重建（合同公式，双端一致；客户端准星=相机 forward=同一 yaw/pitch——天然同源） */
inline Vec3 DirectionFromYawPitch(float yawDeg, float pitchDeg) {
    const float ry = yawDeg * kPi / 180.f;
    const float rp = pitchDeg * kPi / 180.f;
    return {std::cos(rp) * std::cos(ry), std::cos(rp) * std::sin(ry), std::sin(rp)};
}

/** §4.2 射线起点：权威 pos + Rot(yaw)·(20,8,0) + (0,0,63)——FPP 相机 (20,8,63) 的精确镜像（准星=判定射线；
 *  right=(-sinYaw,cosYaw)：UE yaw+ 转向 +Y=右）。pitch 分量前移忽略=取舍记录 */
/** 历史采样（ep06 回溯·ADR-1 单钟纪律）：tMs=服务器接收时刻（steady 派生毫秒），pos=钳后权威位。
 *  ⚠ 时间坐标系单一来源——绝不混入客户端钟（Z 区间混帧 bug 的同族预防） */
struct HistorySample {
    std::uint64_t tMs = 0;
    float x = 0.f, y = 0.f, z = 0.f;
};

/** 历史插值采样（ep06 v2·环形序感知）：judgeAt 落在两采样之间→线性插值；
 *  早于最老/晚于最新→钳边界（不外推——断流者不猜未来）。
 *  ⚠ 环形序：count==cap 时数组已回绕（head=最老槽位），逻辑序=(head+i)%cap——
 *  直接按下标扫描在回绕后时间戳非单调（2026-10-30 审计抓出）。纯函数零依赖。 */
inline std::optional<Vec3> SampleHistoryAt(const HistorySample* h, int count, int head,
                                           std::uint64_t judgeAtMs) {
    if (count <= 0 || h == nullptr) return std::nullopt;
    // 逻辑序起点：未满=0（条目packed在[0,count)）；已满=head（最老槽位，环形覆盖语义）
    const int start = (count == kEntityHistoryCap) ? head : 0;
    const auto at = [&h, &start](int logical) -> const HistorySample& {
        return h[(start + logical) % kEntityHistoryCap];
    };
    if (judgeAtMs <= at(0).tMs) return Vec3{at(0).x, at(0).y, at(0).z};
    if (judgeAtMs >= at(count - 1).tMs) return Vec3{at(count - 1).x, at(count - 1).y, at(count - 1).z};
    for (int i = 0; i + 1 < count; ++i) {
        if (judgeAtMs >= at(i).tMs && judgeAtMs < at(i + 1).tMs) {
            const float span = float(at(i + 1).tMs - at(i).tMs);
            const float a = span > 0.f ? float(judgeAtMs - at(i).tMs) / span : 0.f;
            return Vec3{at(i).x + (at(i + 1).x - at(i).x) * a,
                        at(i).y + (at(i + 1).y - at(i).y) * a,
                        at(i).z + (at(i + 1).z - at(i).z) * a};
        }
    }
    return Vec3{at(count - 1).x, at(count - 1).y, at(count - 1).z};
}

inline Vec3 RayOriginFromEntity(float px, float py, float pz, float yawDeg) {
    const float ry = yawDeg * kPi / 180.f;
    const float cy = std::cos(ry), sy = std::sin(ry);
    return {px + cy * kEyeForwardOffset - sy * kEyeRightOffset,
            py + sy * kEyeForwardOffset + cy * kEyeRightOffset,
            pz + kEyeHeight};
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
        // 修复（2026-10-30 用户实勘"下半身无伤害/头顶有伤害"）：oz 已是相对圆柱中心的坐标，
        // Z 夹层=±halfHeight——此前误写 (cz±halfHeight) 混入绝对中心，等效命中体浮高 ~cz
        // （cz=89.65 时有效区间漂到绝对 [91,267]：腰部以下不可命中、头顶上方 84cm 仍命中）
        if (oz < -halfHeight || oz > halfHeight) return std::nullopt;
    } else {
        float t1 = (-halfHeight - oz) / d.z;
        float t2 = ( halfHeight - oz) / d.z;
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
