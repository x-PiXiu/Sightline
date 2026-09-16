// domain/item.h —— 道具实体：房间内可拾取物（血包/弹药箱）
// 纯 C++ 头文件，零依赖（红线同 domain/types.h）。
// ItemTypeId 数值是两端契约：客户端 SightlineProto.h 按同值解码，改值需两端同步。

#pragma once
#include <cstdint>
#include "domain/types.h"

namespace sightline::domain {

// 道具类型（网络标识，与字节协议里的 1B typeId 对应）
enum class ItemTypeId : uint8_t {
    HealthPack = 1,   // 血包：服务端权威回血（hp = min(max_hp, hp+heal)）
    AmmoBox    = 2,   // 弹药箱：弹药客户端本地非权威，服务端只标记拾取
};

struct Item {
    uint32_t net_id = 0;       // 房间内自增 NetID（不是全局唯一；重开一局重新从 1 计）
    ItemTypeId type_id = ItemTypeId::HealthPack;
    Vec3 pos;                  // 脚底位置（y=0 地面；判定用水平距离）
    bool taken = false;        // 已被拾取（处于重生冷却中）
    double taken_at_ms = 0.0;  // 被拾取的时刻（单调钟毫秒，惰性重生基准）
};

} // namespace sightline::domain
