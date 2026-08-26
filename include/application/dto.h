// application/dto.h —— 用例层的输入输出模型（纯数据）
// 协议字节 ↔ DTO 的翻译在 adapters/protocol_codec，用例层不见字节、不识 msgid

#pragma once
#include <string>
#include <vector>
#include <variant>
#include <cstdint>
#include "domain/types.h"

namespace sightline::app {

using domain::PlayerId;
using domain::RoomId;
using domain::Vec3;

// ============ 客户端 → 服务器（命令）============

struct LoginCommand { std::string name; };
struct JoinRoomCommand {};
struct MoveCommand  { Vec3 pos; float yaw; };                 // 发送者身份由会话上下文提供
struct FireCommand  { Vec3 origin; Vec3 dir; };               // dir 已归一化
struct PingCommand  { uint64_t client_time; };

using GameCommand = std::variant<LoginCommand, JoinRoomCommand,
                                 MoveCommand, FireCommand, PingCommand>;

// ============ 服务器 → 客户端（事件）============

struct LoginAckEvent  { PlayerId player_id; };
struct RoomStartEvent { std::vector<PlayerId> players; };
struct MoveEvent      { PlayerId player_id; Vec3 pos; float yaw; };
struct HitEvent {                 // 命中/未中统一事件：victim=0 表示 miss（用于弹孔/音效表现）
    PlayerId shooter;
    PlayerId victim;
    int damage;
    int victim_hp;
    bool victim_dead;
};
struct RespawnEvent   { PlayerId player_id; Vec3 pos; int hp; };
struct PlayerLeftEvent{ PlayerId player_id; };
struct GameOverEvent  { PlayerId winner; };
struct PongEvent      { uint64_t client_time; };
struct KickEvent      { uint8_t reason; };   // 1=心跳超时

using GameEvent = std::variant<LoginAckEvent, RoomStartEvent, MoveEvent, HitEvent,
                               RespawnEvent, PlayerLeftEvent, GameOverEvent,
                               PongEvent, KickEvent>;

} // namespace sightline::app
