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

struct LoginCommand {
    std::string name;           // 游客=昵称；账号登录=登录名
    std::string pass_hash;      // 口令哈希（空 = 游客/旧客户端兼容路径）
};
struct RegisterCommand {        // D2：账号注册（存储线程异步处理，15 号 02 文档）
    std::string account;
    std::string pass_hash;
};
struct JoinRoomCommand {};
struct MoveCommand  { Vec3 pos; float yaw;                    // 发送者身份由会话上下文提供
                      uint8_t flags = 0;                      // 动画同步：bit0=端枪 bit1=下蹲 bit2=跳跃
                      float aim_pitch = 0.f; };               // 视线俯仰角（度）
struct FireCommand  { Vec3 origin; Vec3 dir; };               // dir 已归一化
struct PingCommand  { uint64_t client_time; };

// ---- D4 大厅：房间列表 / 建房 / 排行榜 / 战绩查询 ----

struct QueryRecordCommand { std::uint64_t account_id = 0; };
struct ListRoomsCommand {};
struct CreateRoomCommand { std::uint8_t mode = 1; };
struct TopKillsCommand {};

using GameCommand = std::variant<LoginCommand, RegisterCommand, JoinRoomCommand,
                                 MoveCommand, FireCommand, PingCommand,
                                 QueryRecordCommand, ListRoomsCommand,
                                 CreateRoomCommand, TopKillsCommand>;

// ============ 服务器 → 客户端（事件）============

struct LoginAckEvent  {
    PlayerId player_id = 0;
    // ---- D2 加尾（旧客户端只读头部 playerId，不受影响）----
    uint8_t  ok       = 1;          // 1=登录成功 0=失败
    bool     is_guest = true;
    std::uint64_t account_id = 0;
    std::string nickname;
    std::uint16_t wins = 0, losses = 0, kills = 0, deaths = 0;
    std::string token;              // 断线重连凭据（W7）
};
struct RoomStartEvent { std::vector<PlayerId> players; };
struct MoveEvent      { PlayerId player_id; Vec3 pos; float yaw;
                        uint8_t flags = 0; float aim_pitch = 0.f; };
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
struct RegisterResultEvent {                   // D2：注册结果（连接级，登录前下发）
    uint8_t ok = 0;
    uint8_t err_code = 0;       // 0=成功 1=重名 2=非法 3=内部错误
};
struct MatchScoreRow {                         // D3：结算面板单行（账号维度，游客=0）
    std::uint64_t account_id = 0;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};
struct MatchEndEvent {                         // D3：对局结算（判胜统一出口广播）
    std::uint64_t match_seq = 0;               // 进程内对局序号
    std::uint64_t winner_account_id = 0;       // 0 = 无胜者（全员离开）
    std::uint16_t duration_sec = 0;
    std::vector<MatchScoreRow> scores;
};
struct ItemSpawnEvent {                       // P2：道具出现（开局布点/冷却重生）
    uint32_t net_id;
    uint8_t type_id;    // domain::ItemTypeId 的数值（两端契约）
    Vec3 pos;
};
struct ItemTakenEvent {                       // P2：道具被拾取（服务端权威判定后广播）
    uint32_t net_id;
    PlayerId picker;
    uint8_t type_id;
    int picker_new_hp;
};

// ---- D4 大厅：房间列表 / 建房确认 / 排行榜 / 战绩列表 ----

struct RecordListRow {
    std::uint64_t match_id = 0;
    std::uint8_t  mode = 1;
    bool          win = false;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};
struct RecordListEvent { std::vector<RecordListRow> records; };

struct RoomBrief {
    std::uint32_t room_id = 0;
    std::uint8_t  mode  = 1;
    std::uint32_t cur_players = 0;
    std::uint32_t max_players = 0;
};
struct RoomListEvent { std::vector<RoomBrief> rooms; };

struct JoinAckEvent {
    std::uint32_t room_id = 0;
    std::uint8_t  ok = 0;
    std::uint8_t  mode = 1;
};

struct TopKillsRow {
    std::uint64_t account_id = 0;
    std::uint16_t kills = 0;
};
struct TopKillsEvent { std::vector<TopKillsRow> rows; };

using GameEvent = std::variant<LoginAckEvent, RoomStartEvent, MoveEvent, HitEvent,
                               RespawnEvent, PlayerLeftEvent, GameOverEvent,
                               PongEvent, KickEvent, RegisterResultEvent,
                               MatchEndEvent, ItemSpawnEvent, ItemTakenEvent,
                               RecordListEvent, RoomListEvent, TopKillsEvent,
                               JoinAckEvent>;

} // namespace sightline::app
