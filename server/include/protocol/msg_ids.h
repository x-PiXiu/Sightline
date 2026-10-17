// 协议消息枚举 —— 与 docs/protocol/MSG_IDS.md 保持同步（服务端 ⇆ UE 共同契约）
// 帧格式：[2B len][2B msgid][payload]，小端；len = 2 + payload.size()
#pragma once
#include <cstdint>

namespace protocol {

    enum MsgId : uint16_t {
        MSG_S2C_WELCOME        = 1,  // S→C  [8B serverNowMs]
        MSG_C2S_HEARTBEAT      = 2,  // C→S  [4B seq][8B clientTs]
        MSG_S2C_HEARTBEAT_ACK  = 3,  // S→C  [4B seq][8B clientTs][8B serverTs]
        // —— 第 2 期：登录 + 房间（与 docs/protocol/MSG_IDS.md v2 保持同步）——
        MSG_C2S_LOGIN          = 4,  // C→S  [1B 名长][名字]
        MSG_S2C_LOGIN_RESULT   = 5,  // S→C  [1B 结果码][4B PlayerId]
        MSG_C2S_CREATE_ROOM    = 6,  // C→S  [1B 房名长][房间名]
        MSG_S2C_CREATE_RESULT  = 7,  // S→C  [1B 结果码][4B RoomId]
        MSG_C2S_JOIN_ROOM      = 8,  // C→S  [4B RoomId]
        MSG_S2C_JOIN_RESULT    = 9,  // S→C  [1B 结果码][4B RoomId][1B 人数]
        MSG_C2S_LEAVE_ROOM     = 10, // C→S  (空)
        MSG_S2C_ROOM_LIST      = 11, // S→C  [2B 房间数]{[4B RoomId][1B 人数][1B 房名长][房名][1B 房主名长][房主名]}
        MSG_S2C_ROOM_STATE     = 12, // S→C  [4B RoomId][1B 房主名长][房主名][1B 人数]{[1B 名长][名字]}
        MSG_S2C_KICKED         = 13, // S→C  [1B 原因码]（1 顶号 / 2 登录超时 / 3 服务器关闭）
        // —— 第 3 期：朴素移动同步对局（与 docs/protocol/MSG_IDS.md v3 保持同步）——
        MSG_C2S_START_MATCH    = 14, // C→S  (空 payload)　仅房主、在房中
        MSG_S2C_MATCH_START    = 15, // S→C  [4B 玩家数 n]{[4B PlayerId][1B 名长][名字][1B 出生点序号]}×n
        MSG_C2S_MOVE_REPORT    = 16, // C→S  [12B pos xyz f32][4B yaw f32][4B pitch f32][1B flags]（bit0=腾空）
        MSG_S2C_ENTITY_STATE   = 17, // S→C  [2B 数量 n]{[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags]}×n（不含接收者自己）
        MSG_S2C_ENTITY_LEAVE   = 18, // S→C  [4B PlayerId]（成员离开房间/断线 → 对端销毁角色）
    };

} // namespace protocol
