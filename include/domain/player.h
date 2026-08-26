// domain/player.h —— 玩家实体：玩家的全部权威状态都在这，且只有这

#pragma once
#include <string>
#include "domain/types.h"

namespace sightline::domain {

struct Player {
    PlayerId id = 0;
    std::string name;
    int hp = 100;
    int kills = 0;
    bool alive = true;
    Vec3 position;      // 服务器权威位置（客户端上报、服务器采纳的"半权威"模型）
    float yaw = 0.f;    // 朝向（弧度）

    void resetHp(int max_hp) { hp = max_hp; alive = true; }
};

} // namespace sightline::domain
