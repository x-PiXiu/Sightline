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
                return c;
            }
            case MsgId::C2S_JoinRoom:
                return JoinRoomCommand{};
            case MsgId::C2S_Move: {
                if (n < 16) return std::nullopt;
                return MoveCommand{rdVec3(p), rdF32(p + 12)};
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
    };

    // ---------- 事件编码 ----------
    static void encodeEvent(const app::LoginAckEvent& e, std::string& wire) {
        Writer w; w.u32(e.player_id);
        finish(MsgId::S2C_LoginAck, w, wire);
    }
    static void encodeEvent(const app::RoomStartEvent& e, std::string& wire) {
        Writer w; w.u32(static_cast<uint32_t>(e.players.size()));
        for (auto pid : e.players) w.u32(pid);
        finish(MsgId::S2C_RoomStart, w, wire);
    }
    static void encodeEvent(const app::MoveEvent& e, std::string& wire) {
        Writer w; w.u32(e.player_id); w.vec3(e.pos); w.f32(e.yaw);
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

    static void finish(MsgId id, const Writer& w, std::string& wire) {
        encodeFrame(static_cast<uint16_t>(id), w.buf, wire);
    }
};

} // namespace sightline::adapters
