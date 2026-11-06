// config_service.cpp —— ep08 步骤 3：配置适配器实现（首批 src/adapters/*.cpp）
// 纪律：本文件只做「格式转换 + 并发机制 + 生命周期」，业务校验一律调用实体层 GameConfig::Validate()。
// 数据表解析用零依赖的轻量 Lua 表 reader（只认 key = number / key = { ... } 的纯数据子集），
// 不引入 Lua VM —— 公式 / 决策热修才是 Lua 的领地（步骤 4），lite 模式因此零外部依赖。
#include "adapters/config_service.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace adapters {
namespace mc = sightline::match;
namespace app = sightline::application;

namespace {

// ---- 极简 Lua 表读取器：只接受 `return { key = 数字, 组 = { ... } }` 的纯数据子集 ----
struct LuaValue {
    bool isTable = false;
    double number = 0.0;
    std::map<std::string, LuaValue> table;
};

class TableReader {
public:
    explicit TableReader(std::string_view src) : s_(src) {}

    bool Parse(LuaValue& root, std::string& err) {
        Skip();
        const std::string head = Word();
        if (!head.empty() && head != "return") { err = "数据表必须形如 `return { ... }`（读到 " + head + "）"; return false; }
        Skip();
        if (Peek() != '{') { err = "缺少顶层 `{`"; return false; }
        root.isTable = true;
        return ParseTable(root, err);
    }

private:
    char Peek() const { return pos_ < s_.size() ? s_[pos_] : '\0'; }
    void Skip() {
        for (;;) {
            while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' ||
                                        s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_;
            if (pos_ + 1 < s_.size() && s_[pos_] == '-' && s_[pos_ + 1] == '-') {
                while (pos_ < s_.size() && s_[pos_] != '\n') ++pos_;   // 行注释
                continue;
            }
            break;
        }
    }
    bool Eat(char c) { Skip(); if (Peek() != c) return false; ++pos_; return true; }
    std::string Word() {
        Skip();
        const std::size_t b = pos_;
        while (pos_ < s_.size() && (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) ++pos_;
        return std::string(s_.substr(b, pos_ - b));
    }
    bool Number(double& out) {
        Skip();
        const std::string rest(s_.substr(pos_));
        char* end = nullptr;
        out = std::strtod(rest.c_str(), &end);
        if (end == rest.c_str() || !std::isfinite(out)) return false;
        pos_ += static_cast<std::size_t>(end - rest.c_str());
        return true;
    }
    bool ParseTable(LuaValue& out, std::string& err) {
        if (!Eat('{')) { err = "缺少 `{`"; return false; }
        for (;;) {
            Skip();
            if (Peek() == '}') { ++pos_; return true; }
            const std::string key = Word();
            if (key.empty()) { err = "表项缺少键名（或出现非纯数据语法）"; return false; }
            if (!Eat('=')) { err = "键 " + key + " 后缺少 `=`"; return false; }
            Skip();
            LuaValue val;
            if (Peek() == '{') {
                val.isTable = true;
                if (!ParseTable(val, err)) return false;
            } else {
                if (!Number(val.number)) { err = "键 " + key + " 的值不是有限数字"; return false; }
            }
            out.table[key] = std::move(val);
            Skip();
            if (Peek() == ',') { ++pos_; continue; }
            if (Peek() == '}') { ++pos_; return true; }
            err = "表项 " + key + " 后缺少 `,` 或 `}`";
            return false;
        }
    }

    std::string_view s_;
    std::size_t pos_ = 0;
};

const LuaValue* Find(const LuaValue& root, const std::string& path) {
    const LuaValue* cur = &root;
    std::size_t b = 0;
    while (b < path.size()) {
        const std::size_t dot = path.find('.', b);
        const std::string part = path.substr(b, dot == std::string::npos ? std::string::npos : dot - b);
        const auto it = cur->table.find(part);
        if (it == cur->table.end()) return nullptr;
        cur = &it->second;
        if (dot == std::string::npos) break;
        b = dot + 1;
    }
    return cur;
}

bool Num(const LuaValue& root, const std::string& key, double& out, std::string& err) {
    const LuaValue* v = Find(root, key);
    if (!v || v->isTable) { err = "缺键或类型错误: " + key; return false; }
    out = v->number;
    return true;
}

bool UInt(const LuaValue& root, const std::string& key, unsigned max,
          unsigned& out, std::string& err) {
    double v = 0.0;
    if (!Num(root, key, v, err)) return false;
    if (!std::isfinite(v) || v < 0.0 || std::floor(v) != v || v > static_cast<double>(max)) {
        err = key + " 不是 0.." + std::to_string(max) + " 的整数";
        return false;
    }
    out = static_cast<unsigned>(v);
    return true;
}

float Flt(const LuaValue& root, const std::string& key, float fallback, std::string& err) {
    double v = 0.0;
    if (!Num(root, key, v, err)) return fallback;
    return static_cast<float>(v);
}

std::string Digest8(std::string_view s) {
    std::uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
    std::ostringstream os;
    os << std::hex << (h & 0xFFFFFFFFull);
    std::string hex = os.str();
    while (hex.size() < 8) hex.insert(hex.begin(), '0');
    return hex;
}

} // namespace

ConfigService::ConfigService(std::string path)
    : path_(std::move(path)),
      cfg_(std::make_shared<const mc::GameConfig>(mc::GameConfig{})),
      version_("v0-builtin"), digest_("builtin") {
    LoadSnapshot("startup", false);   // 文件不存在 / 非法 → 保留内建默认（与 ep07 常量逐字段一致）
}

std::uint64_t ConfigService::UtcNowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

bool ConfigService::ParseFile(const std::string& path, mc::GameConfig& out,
                              std::string& digest, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f.good()) { err = "配置文件不存在: " + path; return false; }
    const std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    LuaValue root;
    TableReader reader(src);
    if (!reader.Parse(root, err)) return false;

    static const std::vector<std::string> kTopKeys{
        "rules", "deathmatch", "rewind", "bot", "weapons", "formula"};
    for (const auto& [k, v] : root.table)
        if (std::find(kTopKeys.begin(), kTopKeys.end(), k) == kTopKeys.end()) {
            err = "未知顶层键: " + k + "（整表拒收）";
            return false;
        }

    unsigned u = 0;
    mc::GameConfig c;
    if (!UInt(root, "rules.max_hp", 255, u, err)) return false;
    c.maxHp = static_cast<std::uint16_t>(u);
    if (!UInt(root, "deathmatch.respawn_ms", 65535, u, err)) return false;
    c.deathmatch.respawnMs = static_cast<std::uint16_t>(u);
    if (!UInt(root, "deathmatch.invuln_ms", 65535, u, err)) return false;
    c.deathmatch.invulnMs = static_cast<std::uint16_t>(u);
    if (!UInt(root, "deathmatch.kill_limit", 65535, u, err)) return false;
    c.deathmatch.killLimit = static_cast<std::uint16_t>(u);
    if (!UInt(root, "rewind.max_rewind_ms", 65535, u, err)) return false;
    c.rewind.maxRewindMs = static_cast<std::uint16_t>(u);
    c.deathmatch.spawnClearRadiusCm = Flt(root, "deathmatch.spawn_clear_radius_cm", 150.f, err);
    if (!err.empty()) return false;

    c.bot.reactMs            = Flt(root, "bot.react_ms", 250.f, err);            if (!err.empty()) return false;
    c.bot.errStartDeg        = Flt(root, "bot.err_start_deg", 4.f, err);          if (!err.empty()) return false;
    c.bot.errEndDeg          = Flt(root, "bot.err_end_deg", 1.f, err);            if (!err.empty()) return false;
    c.bot.errDecayDegPerS    = Flt(root, "bot.err_decay_deg_per_s", 3.f, err);    if (!err.empty()) return false;
    c.bot.turnRateDegS       = Flt(root, "bot.turn_rate_deg_s", 360.f, err);      if (!err.empty()) return false;
    c.bot.forgetMs           = Flt(root, "bot.forget_ms", 3000.f, err);           if (!err.empty()) return false;
    c.bot.engageRangeCm      = Flt(root, "bot.engage_range_cm", 900.f, err);      if (!err.empty()) return false;
    c.bot.hpCoverBelow       = Flt(root, "bot.hp_cover_below", 30.f, err);        if (!err.empty()) return false;
    c.bot.moveSpeedCmS       = Flt(root, "bot.move_speed_cm_s", 400.f, err);      if (!err.empty()) return false;
    c.bot.fireIntervalMs     = Flt(root, "bot.fire_interval_ms", 400.f, err);     if (!err.empty()) return false;

    c.formula.headshotMultiplier = Flt(root, "formula.headshot_multiplier", 2.f, err); if (!err.empty()) return false;
    c.formula.falloffStartCm     = Flt(root, "formula.falloff_start_cm", 3000.f, err); if (!err.empty()) return false;
    c.formula.falloffFactor      = Flt(root, "formula.falloff_factor", 0.8f, err);    if (!err.empty()) return false;

    const LuaValue* weapons = Find(root, "weapons");
    if (!weapons || !weapons->isTable) { err = "缺 weapons 表（整表拒收）"; return false; }
    bool sawRifle = false;
    unsigned count = 0;
    for (const auto& [name, w] : weapons->table) {
        if (!w.isTable) { err = "武器 " + name + " 必须是表"; return false; }
        mc::EWeaponType type;
        if (name == "rifle")  { type = mc::EWeaponType::Rifle;  sawRifle = true; }
        else if (name == "pistol") { type = mc::EWeaponType::Pistol; }
        else { err = "未知武器键: " + name + "（契约锁只认 rifle / pistol，整表拒收）"; return false; }
        const auto idx = static_cast<std::size_t>(type);
        const std::string p = "weapons." + name + ".";
        if (!UInt(root, p + "damage", 255, u, err)) return false;
        c.weapons[idx].damage = static_cast<std::uint8_t>(u);
        if (!UInt(root, p + "mag", 100, u, err)) return false;
        c.weapons[idx].mag = static_cast<std::uint8_t>(u);
        if (!UInt(root, p + "reload_ms", 65535, u, err)) return false;
        c.weapons[idx].reloadMs = static_cast<std::uint16_t>(u);
        if (!UInt(root, p + "fire_ms", 65535, u, err)) return false;
        c.weapons[idx].fireMs = static_cast<std::uint16_t>(u);
        ++count;
    }
    if (!sawRifle) { err = "weapons 必须含 rifle（EWeaponType::Rifle=0 契约锁）"; return false; }
    if (count == 0 || count > c.weapons.size()) { err = "weapons 数量越界（1..4）"; return false; }
    c.weaponCount = static_cast<std::uint8_t>(count);

    out = c;
    digest = Digest8(src);
    err.clear();
    return true;
}

app::ReloadResult ConfigService::Reload(std::string_view note) {
    return LoadSnapshot(note, true);
}

app::ReloadResult ConfigService::LoadSnapshot(std::string_view note, bool publish) {
    mc::GameConfig parsed;
    std::string digest, err;
    if (!ParseFile(path_, parsed, digest, err)) return {false, err};
    if (const auto invalid = parsed.Validate()) return {false, *invalid};   // G1：实体层裁决

    ++session_;
    const std::string ver = "v" + std::to_string(session_) + "-" + digest;
    const std::string who = note.empty() ? std::string("console@reload") : std::string(note);
    auto sp = std::make_shared<const mc::GameConfig>(parsed);
    cfg_.store(sp);                       // 双缓冲原子替换；旧表由尚存的 shared_ptr 延迟回收
    version_ = ver;
    digest_ = digest;
    SnapshotEntry e{sp, ver, who, digest, UtcNowMs()};
    history_.push_back(e);
    while (history_.size() > 4) history_.pop_front();
    if (publish) Publish(e, who);
    return {true, ver};
}

void ConfigService::Rollback() {
    if (history_.size() < 2) return;      // 无可回退版本：保持现状
    history_.pop_back();                  // 丢弃当前版
    const SnapshotEntry& prev = history_.back();
    cfg_.store(prev.cfg);                 // 内存 revert，不重读磁盘（坏文件可能还在）
    version_ = prev.version;
    digest_ = prev.digest;
    Publish(prev, "rollback（与磁盘分叉）");
}

void ConfigService::Publish(const SnapshotEntry& e, std::string_view note) {
    if (onChanged_)
        onChanged_(app::ConfigChanged{e.version, std::string(note), e.digest, e.atUtcMs});
}

std::uint16_t ConfigService::MaxHp() const {
    return cfg_.load()->maxHp;
}

mc::WeaponParams ConfigService::Weapon(mc::EWeaponType t) const {
    const auto i = static_cast<std::size_t>(t);
    const auto c = cfg_.load();
    if (i >= c->weapons.size()) return mc::kDefaultWeaponParams;
    return c->weapons[i];
}

mc::FormulaParams ConfigService::Formula() const { return cfg_.load()->formula; }
mc::BotTuning ConfigService::Get() const { return cfg_.load()->bot; }
std::string_view ConfigService::Version() const { return version_; }

app::ConfigWireSnapshot ConfigService::Snapshot() const {
    const auto c = cfg_.load();
    app::ConfigWireSnapshot s;
    s.version = version_;
    s.weaponCount = c->weaponCount;
    for (std::uint8_t i = 0; i < c->weaponCount && i < c->weapons.size(); ++i)
        s.weapons.emplace_back(static_cast<mc::EWeaponType>(i), c->weapons[i]);
    return s;
}

std::string ConfigService::StatusLine() const {
    return "version=" + version_ + " path=" + path_ +
           " 快照=" + std::to_string(history_.size()) + "/4 digest=" + digest_;
}

} // namespace adapters
