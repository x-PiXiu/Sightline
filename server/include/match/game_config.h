// game_config.h —— ep08 步骤 2：实体层配置值对象（零框架 import / 零 IO）
// 字段与 assets/scripts/game_config.lua 一一对应；字符串键 → EWeaponType 索引的映射归
// config_service.cpp（适配器）。G1：校验规则是「去掉软件仍成立」的企业级规则，故为实体方法。
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "match/bot_brain.h"    // BotTuning（实体层，向内）
#include "match/fire_judge.h"   // EWeaponType / WeaponParams / kDefaultWeaponParams

namespace sightline::match {

struct FormulaParams {
    float headshotMultiplier = 2.0f;
    float falloffStartCm = 3000.0f;
    float falloffFactor = 0.8f;
};

struct DeathmatchRules {
    std::uint16_t respawnMs = 3000;
    std::uint16_t invulnMs = 2000;
    std::uint16_t killLimit = 10;
    float spawnClearRadiusCm = 150.0f;
};

struct RewindRules {
    std::uint16_t maxRewindMs = 1000;
};

struct GameConfig {
    std::uint16_t maxHp = 100;                 // rules.max_hp
    std::uint8_t weaponCount = 2;              // 30 号 payload 的 N；与 17 号 bit3~4 容量对齐
    std::array<WeaponParams, 4> weapons{
        kDefaultWeaponParams,                                  // Rifle = 0
        WeaponParams{/*damage=*/20, /*mag=*/12, /*reloadMs=*/1800, /*fireMs=*/220}, // Pistol = 1
        WeaponParams{},
        WeaponParams{},
    };
    FormulaParams formula{};
    DeathmatchRules deathmatch{};
    RewindRules rewind{};
    BotTuning bot{};

    // 任一不过 => 整表拒收、保留旧表 + 红字告警（§3.4）。
    // config_service.cpp 只调用本方法 + 做格式转换，不在适配器里重写规则。
    std::optional<std::string> Validate() const {
        if (maxHp == 0 || maxHp > 255) return "rules.max_hp 越界（1..255）";
        if (weaponCount == 0 || weaponCount > weapons.size())
            return "weapons 数量越界（1..4）";
        for (std::uint8_t i = 0; i < weaponCount; ++i) {
            const WeaponParams& w = weapons[static_cast<std::size_t>(i)];
            if (w.damage < 1) return "weapons[].damage 越界（1..255，30 号 1B）";
            if (w.mag < 1 || w.mag > 100) return "weapons[].mag 越界（1..100）";
            if (w.reloadMs == 0 || w.reloadMs >= 10000) return "weapons[].reload_ms 越界";
            if (w.fireMs == 0) return "weapons[].fire_ms 必须 > 0";
            if (bot.fireIntervalMs < static_cast<float>(w.fireMs))
                return "bot.fire_interval_ms 不得快于任何枪的射速门";
        }
        if (bot.errEndDeg > bot.errStartDeg) return "bot.err_end_deg 不得大于 err_start_deg";
        if (deathmatch.invulnMs >= deathmatch.respawnMs) return "invuln_ms 必须小于 respawn_ms";
        if (formula.headshotMultiplier <= 0.0f) return "headshot_multiplier 必须 > 0";
        if (formula.falloffFactor <= 0.0f || formula.falloffFactor > 1.0f)
            return "falloff_factor 越界（0..1]";
        return std::nullopt;
    }
};

} // namespace sightline::match
