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

using C2SMsg = std::variant<StartMatchMsg, PlayerInputMsg>;   // M1 迁移集；旧消息走 raw 回落（渐进并存）

/** 唯一 C2S switch：msgid → 选 Parse；未迁移/未知一律 nullopt（调用方回落旧路径） */
inline std::optional<C2SMsg> DecodeC2S(std::uint16_t msgid, std::span<const std::uint8_t> payload) {
    switch (msgid) {
    case 14: if (auto m = StartMatchMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
    case 19: if (auto m = PlayerInputMsg::Parse(payload)) return C2SMsg{*m}; return std::nullopt;
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

} // namespace sightline::wire
