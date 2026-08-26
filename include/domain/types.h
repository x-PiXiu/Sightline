// domain/types.h —— 实体层基础类型
// 红线：本层只允许 include C++ 标准库（不依赖 logger/net/任何外部世界）
// 若此文件出现业务/网络 include，架构即已腐化，见 scripts/check_dependencies.sh

#pragma once
#include <cstdint>

namespace sightline::domain {

using PlayerId = uint32_t;   // 0 = 无效玩家
using RoomId = uint32_t;     // 0 = 无效房间

struct Vec3 {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;

    Vec3 operator-(const Vec3& r) const { return {x - r.x, y - r.y, z - r.z}; }
    Vec3 operator+(const Vec3& r) const { return {x + r.x, y + r.y, z + r.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    float dot(const Vec3& r) const { return x * r.x + y * r.y + z * r.z; }
};

} // namespace sightline::domain
