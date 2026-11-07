// messages.h —— 第 4 期契约 v4：消息结构体（variant 编解码，D4 批准）
// 纪律（图=代码一致 protocol_v4）：
//   ① 每消息一个纯数据结构，Parse/Encode 同居一处——编码解码不再两地分居（双端漂移土壤铲除）
//   ② 全工程唯一 switch = DecodeC2S 的 msgid→Parse 选择；长度校验全部进 Parse
//   ③ 本头属 wire 细节（最外层）：lobby / match 业务层禁止 include（验收红线自检）
//   ④ S2C 结构体带 Encode()；C2S 带 Parse()；进不了 variant 的 msgid 在 Decode 层拒绝
#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace sightline::wire {

// ————— C2S —————

/** 14 START_MATCH（空 payload）—— 空结构体占位，visitor 里可类型化处理 */
struct StartMatchMsg {
    static std::optional<StartMatchMsg> Parse(std::span<const std::uint8_t> b) {
        if (!b.empty()) return std::nullopt;               // 契约：空 payload，非空即拒
        return StartMatchMsg{};
    }
};

/** 19 PLAYER_INPUT（v4 · 25B）：[4B seq][12B pos][4B yaw][4B pitch][1B flags] */
struct PlayerInputMsg {
    std::uint32_t seq = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;

    static constexpr std::size_t kSize = 25;

    static std::optional<PlayerInputMsg> Parse(std::span<const std::uint8_t> b) {
        if (b.size() != kSize) return std::nullopt;       // 长度校验收敛在 Parse（25→21 类欠账永绝）
        PlayerInputMsg m;
        const auto rdU32 = [&b](int o) {
            return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
                   (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
        };
        const auto rdF = [&b](int o) { float f; std::memcpy(&f, b.data() + o, sizeof(float)); return f; };
        m.seq = rdU32(0);
        m.x = rdF(4); m.y = rdF(8); m.z = rdF(12);
        m.yaw = rdF(16); m.pitch = rdF(20);
        m.flags = b[24];
        return m;
    }
};

/** 20 FIRE（v4.2 · 空 payload）—— 空结构体占位。空 payload 是设计核心（D5-01）：
 *  方向/位置/命中结果一概不收，判定全用服务器权威状态——客户端连"往哪打"都不让插嘴。
 *  ep08 v4.8 定案：20 号维持空包，第二把武器走 28 号换枪意图 + 服务器当前 weaponType 判定
 *  （ep05 D5-11 预埋按此兑现，不给逐发带 type） */
struct FireMsg {
    static std::optional<FireMsg> Parse(std::span<const std::uint8_t> b) {
        if (!b.empty()) return std::nullopt;               // 契约：空 payload，非空即拒（带方向=宣判苗头）
        return FireMsg{};
    }
};

/** 28 SWITCH_WEAPON（v4.8 · 1B）：[1B weaponType]——0=Rifle / 1=Pistol，其余值整包拒收。
 *  20 号维持空包：服务器按当前 weaponType 判定，逐发不带 type（ep05 D5-11 预埋兑现）。 */
struct SwitchWeaponMsg {
    std::uint8_t weaponType = 0;

    static std::optional<SwitchWeaponMsg> Parse(std::span<const std::uint8_t> b) {
        if (b.size() != 1 || b[0] > 3) return std::nullopt;   // 2bit 容量 = 4 把枪
        return SwitchWeaponMsg{b[0]};
    }
    std::string Encode() const { return std::string{static_cast<char>(weaponType)}; }
};

using C2SMsg = std::variant<StartMatchMsg, PlayerInputMsg, FireMsg, SwitchWeaponMsg>;

/** 唯一 C2S switch：msgid → 选 Parse；未迁移/未知一律 nullopt（调用方回落旧路径） */
inline std::optional<C2SMsg> DecodeC2S(std::uint16_t msgid, std::span<const std::uint8_t> payload) {
    switch (msgid) {
    case 14: if (auto m = StartMatchMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
    case 19: if (auto m = PlayerInputMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
    case 20: if (auto m = FireMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
    case 28: if (auto m = SwitchWeaponMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
    default: return std::nullopt;
    }
}

// ————— S2C —————

/** 17 ENTITY_STATE v2（29B/实体）：[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags][4B ackedSeq]
 *  v4 语义升级：pos/朝向 = 服务器权威推算值；ackedSeq = 已确认输入序号（契约破例 D4-01） */
struct EntityStateV2 {
    std::uint32_t pid = 0;
    float x = 0.f, y = 0.f, z = 0.f;
    float yaw = 0.f, pitch = 0.f;
    std::uint8_t flags = 0;
    std::uint32_t ackedSeq = 0;

    static constexpr std::size_t kSize = 29;

    std::string Encode() const {
        std::string s(kSize, '\0');
        const auto wrU32 = [&s](int o, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) s[o + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        const auto wrF = [&s, &wrU32](int o, float f) {       // wrU32 须入捕获（GCC 实测漏捕即错）
            std::uint32_t bits; std::memcpy(&bits, &f, sizeof(float));
            wrU32(o, bits);
        };
        wrU32(0, pid);
        wrF(4, x); wrF(8, y); wrF(12, z);
        wrF(16, yaw); wrF(20, pitch);
        s[24] = static_cast<char>(flags);
        wrU32(25, ackedSeq);
        return s;
    }
};

/** 15 MATCH_START（roster 列表） */
struct MatchStartMsg {
    struct RosterItem { std::uint32_t pid = 0; std::string name; std::uint8_t spawnIndex = 0; };
    std::vector<RosterItem> roster;

    std::string Encode() const {
        std::string s;
        const auto wrU32 = [&s](std::uint32_t v) {
            for (int i = 0; i < 4; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
        };
        wrU32(static_cast<std::uint32_t>(roster.size()));
        for (const auto& r : roster) {
            wrU32(r.pid);
            s.push_back(static_cast<char>(r.name.size()));
            s += r.name;
            s.push_back(static_cast<char>(r.spawnIndex));
        }
        return s;
    }
};

/** 18 ENTITY_LEAVE */
struct EntityLeaveMsg {
    std::uint32_t pid = 0;
    std::string Encode() const {
        std::string s(4, '\0');
        for (int i = 0; i < 4; ++i) s[i] = static_cast<char>((pid >> (8 * i)) & 0xFF);
        return s;
    }
};

/** 21 FIRE_RESULT（v4.8.1 · 25B）：[4B shooterPid][1B result][4B victimPid][12B hitPoint][1B victimHp][1B gate][2B reloadRemainingMs]
 *  房内全员广播（含射手自己）——一条消息三方共用：射手 Hitmarker / 其余 tracer / 受击者红闪。
 *  result：0=未命中（victimPid/victimHp=0 填充，hitPoint=射线终点）/ 1=命中。
 *  victimHp=命中后余血：第 5 期供 HUD 血量行，第 7 期接血条与死亡判定（契约一次定稿零改动）
 *  reloadRemainingMs（v4.8.1，2026-11-07 连拒根治）：gate=1（换弹中）时=服务器换弹钟剩余毫秒，
 *  客户端据以对齐本地钟——双钟漂移（客户端补满后连发全吃服务器换弹尾·实测 13 连拒）一个回包自愈；
 *  其余 gate/Ok 恒 0 */
struct FireResultMsg {                             // v4.6：+1B gate（2026-11-05 弹匣漂移根治——被拒射击
                                                   //  回传射手本人撤销本地预测扣弹，双端换弹钟锁相）
    std::uint32_t shooterPid = 0;
    std::uint8_t result = 0;
    std::uint32_t victimPid = 0;
    float hitX = 0.f, hitY = 0.f, hitZ = 0.f;
    std::uint8_t victimHp = 0;
    std::uint8_t gate = 0;                         // 0=正常判定 / 非0=被武器门拒（1 换弹 2 弹匣 3 射速 4 死亡）
    std::uint16_t reloadRemainingMs = 0;           // v4.8.1：gate=1 时的换弹钟剩余（其余恒 0）

    static constexpr std::size_t kSize = 25;

    std::string Encode() const {
        std::string s(kSize, '\0');
        const auto wrU32 = [&s](int o, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) s[o + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        const auto wrF = [&s, &wrU32](int o, float f) {
            std::uint32_t bits; std::memcpy(&bits, &f, sizeof(float));
            wrU32(o, bits);
        };
        wrU32(0, shooterPid);
        s[4] = static_cast<char>(result);
        wrU32(5, victimPid);
        wrF(9, hitX); wrF(13, hitY); wrF(17, hitZ);
        s[21] = static_cast<char>(victimHp);
        s[22] = static_cast<char>(gate);
        s[23] = static_cast<char>(reloadRemainingMs & 0xFF);
        s[24] = static_cast<char>((reloadRemainingMs >> 8) & 0xFF);
        return s;
    }
};

// ===== 第 7 期：死亡/重生/结算 + 人机席位（v4.4）=====
struct KillMsg {                                   // 23：[4B victim][4B killer][2B respawnDelayMs]=10B
    std::uint32_t victimPid = 0;
    std::uint32_t killerPid = 0;
    std::uint16_t respawnDelayMs = 0;
    std::string Encode() const {
        std::string s(10, '\0');
        for (int i = 0; i < 4; ++i) s[i] = static_cast<char>((victimPid >> (8 * i)) & 0xFF);
        for (int i = 0; i < 4; ++i) s[4 + i] = static_cast<char>((killerPid >> (8 * i)) & 0xFF);
        s[8] = static_cast<char>(respawnDelayMs & 0xFF);
        s[9] = static_cast<char>((respawnDelayMs >> 8) & 0xFF);
        return s;
    }
};

struct RespawnMsg {                                // v4.5：[4B pid][1B spawnIndex][2B invulnMs][4B x][4B y][4B z][4B yaw]=23B
    std::uint32_t pid = 0;
    std::uint8_t spawnIndex = 0;                   // 服务器选点序号（Spawn_{n+1} 同源·比分/回放语义保留）
    std::uint16_t invulnMs = 0;
    float x = 0.f, y = 0.f, z = 0.f, yawDeg = 0.f; // v4.5：服务器权威落位坐标（客户端不再自查表传送——
                                                   //  FindSpawnStart 关卡实时位置与烘焙合同漂移→锚定握手
                                                   //  拒放行→和解拉扯+和解暴增，2026-11-03 用户实锤）
    static constexpr std::size_t kSize = 23;
    std::string Encode() const {
        std::string s(kSize, '\0');
        const auto wrU32 = [&s](int o, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) s[o + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
        };
        const auto wrF = [&s, &wrU32](int o, float f) {
            std::uint32_t bits; std::memcpy(&bits, &f, sizeof(float));
            wrU32(o, bits);
        };
        wrU32(0, pid);
        s[4] = static_cast<char>(spawnIndex);
        s[5] = static_cast<char>(invulnMs & 0xFF);
        s[6] = static_cast<char>((invulnMs >> 8) & 0xFF);
        wrF(7, x); wrF(11, y); wrF(15, z); wrF(19, yawDeg);
        return s;
    }
    static std::optional<RespawnMsg> Parse(std::span<const std::uint8_t> b) {
        if (b.size() != kSize) return std::nullopt;
        RespawnMsg m;
        const auto rdU32 = [&b](int o) {
            return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
                   (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
        };
        const auto rdF = [&b](int o) { float f; std::memcpy(&f, b.data() + o, sizeof(float)); return f; };
        m.pid = rdU32(0);
        m.spawnIndex = b[4];
        m.invulnMs = static_cast<std::uint16_t>(b[5]) | (static_cast<std::uint16_t>(b[6]) << 8);
        m.x = rdF(7); m.y = rdF(11); m.z = rdF(15); m.yawDeg = rdF(19);
        return m;
    }
};

struct MatchEndMsg {                               // 25：[1B 原因][1B n]{[4B pid][2B kills][2B deaths]}×n
    std::uint8_t reason = 0;                       // 1=击杀上限 2=真人全部离开
    struct Row { std::uint32_t pid = 0; std::uint16_t kills = 0; std::uint16_t deaths = 0; };
    std::vector<Row> rows;
    std::string Encode() const {
        std::string s(2 + rows.size() * 8, '\0');
        s[0] = static_cast<char>(reason);
        s[1] = static_cast<char>(rows.size());
        std::size_t o = 2;
        for (const auto& r : rows) {
            for (int i = 0; i < 4; ++i) s[o + i] = static_cast<char>((r.pid >> (8 * i)) & 0xFF);
            s[o + 4] = static_cast<char>(r.kills & 0xFF);
            s[o + 5] = static_cast<char>((r.kills >> 8) & 0xFF);
            s[o + 6] = static_cast<char>(r.deaths & 0xFF);
            s[o + 7] = static_cast<char>((r.deaths >> 8) & 0xFF);
            o += 8;
        }
        return s;
    }
};

struct BotAddMsg {                                 // 26：[1B 难度]
    std::uint8_t difficulty = 0;
    static std::optional<BotAddMsg> Parse(const std::span<const std::uint8_t>& b) {
        if (b.size() != 1) return std::nullopt;
        return BotAddMsg{b[0]};
    }
};

struct BotRemoveMsg {                              // 27：[4B botPid]
    std::uint32_t botPid = 0;
    static std::optional<BotRemoveMsg> Parse(const std::span<const std::uint8_t>& b) {
        if (b.size() != 4) return std::nullopt;
        BotRemoveMsg m;
        for (int i = 0; i < 4; ++i) m.botPid |= static_cast<std::uint32_t>(b[i]) << (8 * i);
        return m;
    }
};

// ===== 第 8 期：契约 v4.8 =====

/** 17 号 flags bit3~4 = weaponType（wire 长度零增长）。
 *  位分配：bit0 腾空 / bit1 死亡 / bit2 无敌 / bit3~4 武器类型 / bit5~7 保留。 */
inline constexpr std::uint8_t kWeaponTypeMask = 0x18;   // bit3|bit4
inline std::uint8_t PutWeaponType(std::uint8_t flags, std::uint8_t type) {
    return static_cast<std::uint8_t>((flags & ~kWeaponTypeMask) | ((type & 0x03u) << 3));
}
inline std::uint8_t GetWeaponType(std::uint8_t flags) {
    return static_cast<std::uint8_t>((flags & kWeaponTypeMask) >> 3);
}

/** 30 S2C_CONFIG_VERSION（v4.8 · 变长）：
 *  [1B verLen][ver][1B N]{[1B type][1B damage][1B mag][2B reloadMs][2B fireIntervalMs]}×N
 *  每项 7B；damage 供客户端飘字 / 预测显示，权威余血仍以 21 号 victimHp 为准（§6.1 边界 1）。
 *  开局（15 号后）+ 每次 reload / patch 广播。 */
struct ConfigVersionMsg {
    struct WeaponRow {
        std::uint8_t type = 0;
        std::uint8_t damage = 0;
        std::uint8_t mag = 0;
        std::uint16_t reloadMs = 0;
        std::uint16_t fireIntervalMs = 0;
    };
    std::string version;
    std::vector<WeaponRow> weapons;

    static constexpr std::size_t kRowSize = 7;

    std::string Encode() const {
        std::string s;
        s.push_back(static_cast<char>(std::min<std::size_t>(version.size(), 255)));
        s.append(version.data(), std::min<std::size_t>(version.size(), 255));
        s.push_back(static_cast<char>(std::min<std::size_t>(weapons.size(), 255)));
        for (const auto& w : weapons) {
            s.push_back(static_cast<char>(w.type));
            s.push_back(static_cast<char>(w.damage));
            s.push_back(static_cast<char>(w.mag));
            s.push_back(static_cast<char>(w.reloadMs & 0xFF));
            s.push_back(static_cast<char>((w.reloadMs >> 8) & 0xFF));
            s.push_back(static_cast<char>(w.fireIntervalMs & 0xFF));
            s.push_back(static_cast<char>((w.fireIntervalMs >> 8) & 0xFF));
        }
        return s;
    }

    static std::optional<ConfigVersionMsg> Parse(std::span<const std::uint8_t> b) {
        if (b.size() < 2) return std::nullopt;
        const std::size_t verLen = b[0];
        if (verLen + 2 > b.size()) return std::nullopt;
        ConfigVersionMsg m;
        m.version.assign(reinterpret_cast<const char*>(b.data() + 1), verLen);
        const std::uint8_t n = b[1 + verLen];
        const std::size_t body = static_cast<std::size_t>(1 + verLen) + 1;
        if (n > 4 || body + static_cast<std::size_t>(n) * kRowSize != b.size()) return std::nullopt;
        for (std::uint8_t i = 0; i < n; ++i) {
            const std::size_t o = body + static_cast<std::size_t>(i) * kRowSize;
            WeaponRow w;
            w.type = b[o]; w.damage = b[o + 1]; w.mag = b[o + 2];
            w.reloadMs = static_cast<std::uint16_t>(b[o + 3]) |
                         (static_cast<std::uint16_t>(b[o + 4]) << 8);
            w.fireIntervalMs = static_cast<std::uint16_t>(b[o + 5]) |
                               (static_cast<std::uint16_t>(b[o + 6]) << 8);
            m.weapons.push_back(w);
        }
        return m;
    }
};

} // namespace sightline::wire
