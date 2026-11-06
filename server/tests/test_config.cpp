// test_config.cpp —— ep08 步骤 3：配置体系零 IO 单测
// 覆盖：启动默认 / 好表生效 / 坏表整表拒收 / 跨字段校验 / 4 版快照环 / rollback / 域事件 / 命令解析。
// 纪律：只写 /tmp 临时文件，不连 Lua VM、不连 DB、不起线程（命令解析走静态 Parse）。
#include "adapters/config_service.h"
#include "adapters/stdin_command_source.h"

#include <cstdio>
#include <fstream>
#include <string>

using namespace sightline;
namespace app = sightline::application;
namespace mc = sightline::match;

static int g_checked = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);   \
            return 1;                                                   \
        }                                                               \
    } while (0)

static const std::string kPath = "/tmp/sightline_test_game_config.lua";

static void Write(const std::string& s) {
    std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
    f << s;
}

static std::string Table(int damage, int respawnMs = 3000, int invulnMs = 2000,
                         const std::string& weaponKey = "rifle") {
    return "return {\n"
           "  rules = { max_hp = 100 },\n"
           "  deathmatch = { respawn_ms = " + std::to_string(respawnMs) +
           ", invuln_ms = " + std::to_string(invulnMs) + ", kill_limit = 10,\n"
           "                 spawn_clear_radius_cm = 150 },\n"
           "  rewind = { max_rewind_ms = 1000 },\n"
           "  bot = { react_ms = 250, err_start_deg = 4, err_end_deg = 1,\n"
           "          err_decay_deg_per_s = 3, turn_rate_deg_s = 360,\n"
           "          forget_ms = 3000, engage_range_cm = 900,\n"
           "          hp_cover_below = 30, move_speed_cm_s = 400,\n"
           "          fire_interval_ms = 400 },\n"
           "  weapons = { " + weaponKey + " = { damage = " + std::to_string(damage) +
           ", mag = 30, reload_ms = 2500, fire_ms = 95 },\n"
           "              pistol = { damage = 20, mag = 12, reload_ms = 1800, fire_ms = 220 } },\n"
           "  formula = { headshot_multiplier = 2.0, falloff_start_cm = 3000,\n"
           "              falloff_factor = 0.8 },\n"
           "}\n";
}

int main() {
    // ---- 1) 文件不存在 → 内建默认，逐字段等于 ep07 常量 ----
    std::remove(kPath.c_str());
    {
        adapters::ConfigService cfg(kPath);
        CHECK(cfg.Version() == "v0-builtin");
        CHECK(cfg.MaxHp() == 100);
        const auto w = cfg.Weapon(mc::EWeaponType::Rifle);
        CHECK(w.damage == 25 && w.mag == 30 && w.reloadMs == 2500 && w.fireMs == 95);
        CHECK(cfg.HistorySize() == 0);
    }

    // ---- 2) 好表 → 生效 + 版本 + 域事件 ----
    Write(Table(25));
    std::string eventVersion, eventNote;
    int eventCount = 0;
    {
        adapters::ConfigService cfg(kPath);
        cfg.Subscribe([&](const app::ConfigChanged& e) {
            ++eventCount; eventVersion = e.version; eventNote = e.note;
        });
        CHECK(eventCount == 0);                       // startup 加载不发域事件（订阅者尚未接线）
        CHECK(cfg.HistorySize() == 1);
        CHECK(cfg.Version().rfind("v1-", 0) == 0);
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 25);
        CHECK(cfg.Weapon(mc::EWeaponType::Pistol).damage == 20);
        CHECK(cfg.Snapshot().weaponCount == 2);
        CHECK(cfg.Snapshot().weapons.size() == 2);
        CHECK(cfg.Snapshot().weapons[0].second.damage == 25);
        CHECK(cfg.Formula().falloffFactor == 0.8f);
        CHECK(cfg.Get().fireIntervalMs == 400.f);

        // 演示第一拍的数据侧：改 25→50，reload 后视图立刻变
        Write(Table(50));
        const auto r = cfg.Reload("手枪过强 25→50");
        CHECK(r.ok);
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 50);
        CHECK(eventCount == 1);
        CHECK(eventNote == "手枪过强 25→50");
        CHECK(eventVersion == std::string(cfg.Version()));

        // ---- 3) 坏表整表拒收，旧表保留 ----
        Write(Table(0));                              // damage 越界
        CHECK(!cfg.Reload("bad-damage").ok);
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 50);
        Write(Table(25, 3000, 4000));                 // 跨字段 invuln >= respawn
        CHECK(!cfg.Reload("bad-cross").ok);
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 50);
        Write(Table(25, 3000, 2000, "shotgun"));      // 未知武器键
        CHECK(!cfg.Reload("bad-key").ok);
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 50);
        CHECK(eventCount == 1);                       // 拒收不发域事件

        // ---- 4) rollback = 内存回退上一版，不重读磁盘 ----
        Write("return { this is broken");              // 磁盘已坏
        cfg.Rollback();
        CHECK(cfg.Weapon(mc::EWeaponType::Rifle).damage == 25);
        CHECK(cfg.Version().rfind("v1-", 0) == 0);
        CHECK(eventCount == 2);
        CHECK(eventNote.find("rollback") != std::string::npos);

        // ---- 5) 4 版快照环 ----
        for (int i = 0; i < 6; ++i) { Write(Table(25 + i)); CHECK(cfg.Reload("").ok); }
        CHECK(cfg.HistorySize() == 4);
        CHECK(eventVersion == std::string(cfg.Version()));
    }

    // ---- 6) 命令解析 ----
    {
        using C = app::ConsoleCommand;
        auto a = adapters::StdinCommandSource::Parse("  reload   \"手枪过强 25→50\"  ");
        CHECK(a.has_value() && a->kind == C::Kind::Reload);
        CHECK(a->arg == "手枪过强 25→50");
        CHECK(adapters::StdinCommandSource::Parse("reload")->arg.empty());
        CHECK(adapters::StdinCommandSource::Parse("rollback")->kind == C::Kind::Rollback);
        // 步骤 4：patch <name> 解析（arg=脚本名·源码由运行时自读 scriptDir）
        const auto pc = adapters::StdinCommandSource::Parse("patch damage");
        CHECK(pc && pc->kind == C::Kind::Patch && pc->arg == "damage");
        CHECK(adapters::StdinCommandSource::Parse("patch")->arg.empty());   // 缺名=空 arg（Dispatch 会拒收）
        CHECK(adapters::StdinCommandSource::Parse("version")->kind == C::Kind::Version);
        CHECK(adapters::StdinCommandSource::Parse("help")->kind == C::Kind::Help);
        CHECK(!adapters::StdinCommandSource::Parse("   ").has_value());
        CHECK(adapters::StdinCommandSource::Parse("wat")->arg.rfind("未知命令", 0) == 0);
    }

    Write(Table(25));
    std::printf("全部通过：%d 项断言 OK（ep08 步骤 3 配置体系 / 命令解析，零 Lua VM / 零 DB）\n", g_checked);
    return 0;
}
