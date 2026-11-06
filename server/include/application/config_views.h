// config_views.h —— ep08 步骤 2：三张只读视图 + 配置源端口（用例层）
// ISP：各消费者只看自己那一片，改 bot 字段不牵动 30 号广播与审计行（G9）。
// 一律「按值返回」：避免 config_service 的 atomic<shared_ptr<const GameConfig>>
// 原子替换后，调用方持有悬空引用。
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "match/game_config.h"

namespace sightline::application {

using sightline::match::BotTuning;
using sightline::match::EWeaponType;
using sightline::match::FormulaParams;
using sightline::match::WeaponParams;

struct ICombatParams {
    virtual ~ICombatParams() = default;
    virtual std::uint16_t MaxHp() const = 0;
    virtual WeaponParams Weapon(EWeaponType t) const = 0;
    virtual FormulaParams Formula() const = 0;   // §10.2.4 damage.lua 的 params 来源
};

struct IBotTuning {
    virtual ~IBotTuning() = default;
    virtual BotTuning Get() const = 0;
};

struct ConfigWireSnapshot {
    std::string version;                                          // v7-a3f2c1d8
    std::uint8_t weaponCount = 0;
    std::vector<std::pair<EWeaponType, WeaponParams>> weapons;     // 30 号 N 项 × 7B
};

struct IConfigVersion {
    virtual ~IConfigVersion() = default;
    virtual std::string_view Version() const = 0;
    virtual ConfigWireSnapshot Snapshot() const = 0;   // G5：MatchWiring 拼 30 号只读这里
};

// —— 辅助 POD：用例层拥有，适配器只返回/消费（禁止用例层 import 适配器类型）——
struct ReloadResult {
    bool ok = false;
    std::string versionOrError;
};

struct PatchResult {
    bool ok = false;
    std::string versionOrError;
};

struct ConfigChanged {
    std::string version;
    std::string note;
    std::string diffDigest;
    std::uint64_t atUtcMs = 0;
};

struct AuditRecord {
    std::string version;
    std::string note;
    std::string source;
    std::string diffDigest;
    std::uint64_t atUtcMs = 0;
};

struct ConsoleCommand {
    enum class Kind { Reload, Rollback, Version, Help };
    Kind kind = Kind::Help;
    std::string arg;   // reload [note] 的操作动机（§3.6 / v1.4 补齐）
};

struct IConfigSource {
    virtual ~IConfigSource() = default;
    virtual ReloadResult Reload(std::string_view note) = 0;   // 校验→版本→原子替换→发 ConfigChanged
    virtual void Rollback() = 0;                              // 内存快照环 revert，不重读磁盘
};

} // namespace sightline::application
