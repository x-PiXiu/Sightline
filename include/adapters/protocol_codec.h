// adapters/protocol_codec.h —— 协议编解码：wire 字节 ↔ DTO 的唯一翻译点
// 帧格式（与 UE 客户端《UE蓝图与自研服务器通讯指南》约定一致，小端）：
//   [2B payload_len][2B msg_id][payload]
// 职责边界：只翻译、不含业务判断（谦卑对象）

#pragma once
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>
#include "net/buffer.h"
#include "application/dto.h"

namespace sightline::adapters {

// ---- 消息 ID（两端共同契约，改动需同步 UE 客户端）----
enum class MsgId : uint16_t {
    C2S_Login     = 1,
    S2C_LoginAck  = 2,
    C2S_JoinRoom  = 3,
    S2C_RoomStart = 4,
    C2S_Move      = 5,
    S2C_Move      = 6,
    C2S_Fire      = 7,
    S2C_Hit       = 8,
    S2C_GameOver  = 9,
    C2S_Ping      = 10,
    S2C_Pong      = 11,
    S2C_Kick      = 12,
    S2C_PlayerLeft= 13,
    S2C_Respawn   = 14,
    S2C_ItemSpawn = 15,   // P2：[4B netId][1B typeId][12B pos] 共 17B
    S2C_ItemTaken = 16,   // P2：[4B netId][4B pid][1B typeId][4B newHp] 共 13B
    // ---- D2 账号体系（契约：docs/server/02 第二节）----
    C2S_Register        = 17,  // [2B accountLen][account][32B passHash]
    S2C_RegisterResult  = 18,  // [1B ok][1B errCode]
    S2C_MatchEnd        = 19,  // [8B matchId][8B winnerAccountId][2B durationSec][2B count]×N{[8B accountId][2B kills][2B deaths]}
    C2S_QueryRecord     = 20,  // [8B accountId]
    S2C_RecordList      = 21,  // [2B count]×N{[8B matchId][1B mode][1B win][2B kills][2B deaths]}
    C2S_ListRooms       = 22,  // 空
    S2C_RoomList        = 23,  // [2B count]×N{[4B roomId][1B mode][1B cur][1B max]}
    C2S_CreateRoom      = 24,  // [1B mode]
    S2C_JoinAck         = 25,  // [4B roomId][1B ok][1B mode]
    C2S_TopKills        = 26,  // 空
    S2C_TopKills        = 27,  // [2B count]×N{[8B accountId][2B kills]}
};

class ProtocolCodec {
public:
    // ---- 帧封装 ----

    // 从缓冲区提取一条完整消息；不足一帧返回 nullopt（半包留待下次）
    struct RawPacket { uint16_t msg_id; std::vector<uint8_t> payload; };
    static std::optional<RawPacket> tryExtract(Buffer& buf) {
        constexpr size_t kHeader = 4;   // 2B len + 2B id
        if (buf.readableBytes() < kHeader) return std::nullopt;
        uint16_t payload_len = 0, msg_id = 0;
        std::memcpy(&payload_len, buf.peek(), 2);
        std::memcpy(&msg_id, buf.peek() + 2, 2);
        if (buf.readableBytes() < kHeader + payload_len) return std::nullopt;   // 半包
        RawPacket pkt{msg_id, std::vector<uint8_t>(buf.peek() + kHeader,
                                                   buf.peek() + kHeader + payload_len)};
        buf.retrieve(kHeader + payload_len);
        return pkt;
    }

    static void encodeFrame(uint16_t msg_id, const std::vector<uint8_t>& payload,
                            std::string& out) {
        uint16_t len = static_cast<uint16_t>(payload.size());
        const uint8_t lb[2] = {static_cast<uint8_t>(len & 0xFF),
                               static_cast<uint8_t>(len >> 8)};
        const uint8_t ib[2] = {static_cast<uint8_t>(msg_id & 0xFF),
                               static_cast<uint8_t>(msg_id >> 8)};
        out.append(reinterpret_cast<const char*>(lb), 2);
        out.append(reinterpret_cast<const char*>(ib), 2);
        out.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    }

    // ---- DTO → bytes（事件编码）----

    static std::string encode(const app::GameEvent& ev) {
        using namespace sightline::app;
        std::string wire;
        std::visit([&](const auto& e) { encodeEvent(e, wire); }, ev);
        return wire;
    }

    // ---- bytes → DTO（命令解码；不认识的 msgid 返回 nullopt）----

    static std::optional<app::GameCommand> decode(uint16_t msg_id, const uint8_t* p, size_t n) {
        using namespace sightline::app;
        switch (static_cast<MsgId>(msg_id)) {
            case MsgId::C2S_Login: {
                if (n < 2) return std::nullopt;
                uint16_t len = rd16(p);
                if (n < 2 + len) return std::nullopt;
                LoginCommand c;
                c.name.assign(reinterpret_cast<const char*>(p + 2), len);
                // D2 账号登录（加尾）：name 后随 64B 口令哈希；无尾 = 游客/旧客户端
                if (n >= 2 + len + 64)
                    c.pass_hash.assign(reinterpret_cast<const char*>(p + 2 + len), 64);
                return c;
            }
            case MsgId::C2S_Register: {   // D2：[2B accountLen][account][64B passHash]
                if (n < 2) return std::nullopt;
                uint16_t alen = rd16(p);
                if (n < 2 + static_cast<size_t>(alen) + 64) return std::nullopt;
                RegisterCommand c;
                c.account.assign(reinterpret_cast<const char*>(p + 2), alen);
                c.pass_hash.assign(reinterpret_cast<const char*>(p + 2 + alen), 64);
                return c;
            }
            case MsgId::C2S_JoinRoom: {   // D4 加尾：[4B room_id]（0=自动匹配，旧客户端兼容）
                JoinRoomCommand c;
                if (n >= 4) c.room_id = rd32(p);
                return c;
            }
            case MsgId::C2S_Move: {
                if (n < 16) return std::nullopt;
                MoveCommand c{rdVec3(p), rdF32(p + 12)};
                if (n >= 19) {   // Phase1 动画同步扩展（加尾不改头）：旧客户端回退零值
                    c.flags = p[16];
                    const uint16_t raw = static_cast<uint16_t>(p[17]) | (static_cast<uint16_t>(p[18]) << 8);
                    c.aim_pitch = static_cast<float>(static_cast<int16_t>(raw)) * 0.01f;
                }
                return c;
            }
            case MsgId::C2S_Fire: {
                if (n < 24) return std::nullopt;
                return FireCommand{rdVec3(p), rdVec3(p + 12)};
            }
            case MsgId::C2S_Ping: {
                if (n < 8) return std::nullopt;
                uint64_t t = 0;
                std::memcpy(&t, p, 8);
                return PingCommand{t};
            }
            case MsgId::C2S_QueryRecord: {   // [8B accountId]
                if (n < 8) return std::nullopt;
                QueryRecordCommand c;
                c.account_id = rdU64(p);
                return c;
            }
            case MsgId::C2S_ListRooms:
                return ListRoomsCommand{};
            case MsgId::C2S_CreateRoom: {   // [1B mode]
                if (n < 1) return std::nullopt;
                CreateRoomCommand c; c.mode = p[0];
                return c;
            }
            case MsgId::C2S_TopKills:
                return TopKillsCommand{};
            default:
                return std::nullopt;
        }
    }

private:
    // ---------- 小端基础读写 ----------
    static uint16_t rd16(const uint8_t* p) {
        uint16_t v = 0;
        std::memcpy(&v, p, 2);
        return v;   // x86 天然小端；如需跨端严谨可显式拼字节
    }
    static float rdF32(const uint8_t* p) {
        float v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    static uint32_t rd32(const uint8_t* p) {
        uint32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    static uint64_t rdU64(const uint8_t* p) {
        uint64_t v = 0;
        std::memcpy(&v, p, 8);
        return v;   // x86 天然小端
    }
    static uint64_t rd64(const uint8_t* p) {
        uint64_t v = 0;
        std::memcpy(&v, p, 8);
        return v;
    }
    static app::Vec3 rdVec3(const uint8_t* p) {
        return {rdF32(p), rdF32(p + 4), rdF32(p + 8)};
    }

    class Writer {
    public:
        std::vector<uint8_t> buf;
        void u16(uint16_t v) { const uint8_t b[2] = {uint8_t(v & 0xFF), uint8_t(v >> 8)};
                               buf.insert(buf.end(), b, b + 2); }
        void u32(uint32_t v) { const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8),
                               uint8_t(v >> 16), uint8_t(v >> 24)};
                               buf.insert(buf.end(), b, b + 4); }
        void u64(uint64_t v) { for (int i = 0; i < 8; ++i) buf.push_back(uint8_t(v >> (8 * i))); }
        void u8(uint8_t v) { buf.push_back(v); }
        void f32(float v) { uint32_t bits; std::memcpy(&bits, &v, 4); u32(bits); }
        void vec3(const app::Vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
        void str16(const std::string& s) { u16(static_cast<uint16_t>(s.size()));
                                           buf.insert(buf.end(), s.begin(), s.end()); }
    };

    // ---------- 事件编码 ----------
    static void encodeEvent(const app::LoginAckEvent& e, std::string& wire) {
        Writer w; w.u32(static_cast<uint32_t>(e.player_id));
        // D2 加尾（旧客户端只读头部 playerId）：登录结果 + 账号身份 + 战绩摘要 + token
        w.u8(e.ok);
        w.u8(e.is_guest ? 1 : 0);
        w.u64(e.account_id);
        w.str16(e.nickname);
        w.u16(e.wins); w.u16(e.losses); w.u16(e.kills); w.u16(e.deaths);
        w.str16(e.token);
        finish(MsgId::S2C_LoginAck, w, wire);
    }
    static void encodeEvent(const app::RegisterResultEvent& e, std::string& wire) {
        Writer w; w.u8(e.ok); w.u8(e.err_code);
        finish(MsgId::S2C_RegisterResult, w, wire);
    }
    static void encodeEvent(const app::MatchEndEvent& e, std::string& wire) {
        Writer w;
        w.u64(e.match_seq); w.u64(e.winner_account_id); w.u16(e.duration_sec);
        w.u16(static_cast<uint16_t>(e.scores.size()));
        for (const auto& s : e.scores) {
            w.u64(s.account_id); w.u16(s.kills); w.u16(s.deaths);
        }
        finish(MsgId::S2C_MatchEnd, w, wire);
    }
    static void encodeEvent(const app::RoomListEvent& e, std::string& wire) {
        Writer w; w.u16(static_cast<uint16_t>(e.rooms.size()));
        for (const auto& r : e.rooms) {
            w.u32(r.room_id); w.u8(r.mode);
            w.u8(static_cast<uint8_t>(r.cur_players));
            w.u8(static_cast<uint8_t>(r.max_players));
        }
        finish(MsgId::S2C_RoomList, w, wire);
    }
    static void encodeEvent(const app::RecordListEvent& e, std::string& wire) {
        Writer w; w.u16(static_cast<uint16_t>(e.records.size()));
        for (const auto& r : e.records) {
            w.u64(r.match_id); w.u8(r.mode);
            w.u8(r.win ? 1 : 0); w.u16(r.kills); w.u16(r.deaths);
        }
        finish(MsgId::S2C_RecordList, w, wire);
    }
    static void encodeEvent(const app::TopKillsEvent& e, std::string& wire) {
        Writer w; w.u16(static_cast<uint16_t>(e.rows.size()));
        for (const auto& r : e.rows) {
            w.u64(r.account_id); w.u16(r.kills);
        }
        finish(MsgId::S2C_TopKills, w, wire);
    }
    static void encodeEvent(const app::JoinAckEvent& e, std::string& wire) {
        Writer w; w.u32(e.room_id); w.u8(e.ok); w.u8(e.mode);
        finish(MsgId::S2C_JoinAck, w, wire);
    }
    static void encodeEvent(const app::RoomStartEvent& e, std::string& wire) {
        Writer w; w.u32(static_cast<uint32_t>(e.players.size()));
        for (auto pid : e.players) w.u32(pid);
        finish(MsgId::S2C_RoomStart, w, wire);
    }
    static void encodeEvent(const app::MoveEvent& e, std::string& wire) {
        Writer w; w.u32(e.player_id); w.vec3(e.pos); w.f32(e.yaw);
        w.u8(e.flags);                        // Phase1 动画同步状态位
        const uint16_t q = static_cast<uint16_t>(static_cast<int16_t>(e.aim_pitch * 100.f));
        w.u8(static_cast<uint8_t>(q & 0xFF));
        w.u8(static_cast<uint8_t>(q >> 8));   // pitch × 100 量化（int16 小端）
        finish(MsgId::S2C_Move, w, wire);
    }
    static void encodeEvent(const app::HitEvent& e, std::string& wire) {
        Writer w; w.u32(e.shooter); w.u32(e.victim); w.u32(static_cast<uint32_t>(e.damage));
        w.u32(static_cast<uint32_t>(e.victim_hp)); w.u8(e.victim_dead ? 1 : 0);
        finish(MsgId::S2C_Hit, w, wire);
    }
    static void encodeEvent(const app::RespawnEvent& e, std::string& wire) {
        Writer w; w.u32(e.player_id); w.vec3(e.pos); w.u32(static_cast<uint32_t>(e.hp));
        finish(MsgId::S2C_Respawn, w, wire);
    }
    static void encodeEvent(const app::PlayerLeftEvent& e, std::string& wire) {
        Writer w; w.u32(e.player_id);
        finish(MsgId::S2C_PlayerLeft, w, wire);
    }
    static void encodeEvent(const app::GameOverEvent& e, std::string& wire) {
        Writer w; w.u32(e.winner);
        finish(MsgId::S2C_GameOver, w, wire);
    }
    static void encodeEvent(const app::PongEvent& e, std::string& wire) {
        Writer w; w.u64(e.client_time);
        finish(MsgId::S2C_Pong, w, wire);
    }
    static void encodeEvent(const app::KickEvent& e, std::string& wire) {
        Writer w; w.u8(e.reason);
        finish(MsgId::S2C_Kick, w, wire);
    }
    static void encodeEvent(const app::ItemSpawnEvent& e, std::string& wire) {
        Writer w; w.u32(e.net_id); w.u8(e.type_id); w.vec3(e.pos);
        finish(MsgId::S2C_ItemSpawn, w, wire);
    }
    static void encodeEvent(const app::ItemTakenEvent& e, std::string& wire) {
        Writer w; w.u32(e.net_id); w.u32(e.picker); w.u8(e.type_id);
        w.u32(static_cast<uint32_t>(e.picker_new_hp));
        finish(MsgId::S2C_ItemTaken, w, wire);
    }

    static void finish(MsgId id, const Writer& w, std::string& wire) {
        encodeFrame(static_cast<uint16_t>(id), w.buf, wire);
    }
};

} // namespace sightline::adapters
