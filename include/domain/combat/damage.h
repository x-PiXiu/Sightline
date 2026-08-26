// domain/combat/damage.h —— 伤害结算规则（纯函数，可单测）
// 规则与流程分离：Room 负责流程编排，这里只回答"打中了扣多少血、死没死"

#pragma once
#include "domain/types.h"

namespace sightline::domain::combat {

struct DamageOutcome {
    int new_hp = 0;
    bool dead = false;
};

inline DamageOutcome applyDamage(int current_hp, int damage) {
    DamageOutcome o;
    o.new_hp = current_hp - damage;
    if (o.new_hp <= 0) {
        o.new_hp = 0;
        o.dead = true;
    }
    return o;
}

} // namespace sightline::domain::combat
