// Sightline 协议契约（客户端镜像）—— 与 docs/protocol/MSG_IDS.md、服务端 protocol/msg_ids.h 保持一致
// 帧格式：[2B len][2B msgid][payload]，小端；len = 2 + payload.Num()
// 本头文件纯常量与纯函数，零引擎依赖——协议契约层的谦卑对象
#pragma once
#include "CoreMinimal.h"

namespace SightlineProtocol
{
    enum MsgId : uint16
    {
        MSG_S2C_WELCOME       = 1,  // S→C  [8B serverTs]
        MSG_C2S_HEARTBEAT     = 2,  // C→S  [4B seq][8B clientTs]
        MSG_S2C_HEARTBEAT_ACK = 3,  // S→C  [4B seq][8B clientTs][8B serverTs]
        // —— 第 2 期：登录 + 房间（与 docs/protocol/MSG_IDS.md v2 保持同步）——
        MSG_C2S_LOGIN         = 4,  // C→S  [1B 名长][名字]（非空 ≤16B UTF-8）
        MSG_S2C_LOGIN_RESULT  = 5,  // S→C  [1B 结果码][4B PlayerId]（0 成功/1 重名/2 非法）
        MSG_C2S_CREATE_ROOM   = 6,  // C→S  [1B 房名长][房间名]（非空 ≤24B；允许重名）
        MSG_S2C_CREATE_RESULT = 7,  // S→C  [1B 结果码][4B RoomId]（0 成功/1 未登录/2 已在房/3 房名非法）
        MSG_C2S_JOIN_ROOM     = 8,  // C→S  [4B RoomId]
        MSG_S2C_JOIN_RESULT   = 9,  // S→C  [1B 结果码][4B RoomId][1B 人数]（0/1 未登录/2 不存在/3 已满/4 已在房）
        MSG_C2S_LEAVE_ROOM    = 10, // C→S  (空 payload)
        MSG_S2C_ROOM_LIST     = 11, // S→C  [2B 房间数]{[4B RoomId][1B 人数][1B 房名长][房名][1B 房主名长][房主名]}
        MSG_S2C_ROOM_STATE    = 12, // S→C  [4B RoomId][1B 房主名长][房主名][1B 人数]{[1B 名长][名字]}（房主=最早加入者）
        MSG_S2C_KICKED        = 13, // S→C  [1B 原因码]（1 顶号 / 2 登录超时 / 3 服务器关闭；随后断开）
        // —— 第 3 期：朴素移动同步对局（与 docs/protocol/MSG_IDS.md v3 保持同步）——
        MSG_C2S_START_MATCH   = 14, // C→S  (空 payload)　仅房主、在房中
        MSG_S2C_MATCH_START   = 15, // S→C  [4B 玩家数 n]{[4B PlayerId][1B 名长][名字][1B 出生点序号]}×n
        MSG_C2S_MOVE_REPORT   = 16, // C→S  [12B pos xyz f32][4B yaw f32][4B pitch f32][1B flags]（bit0=腾空）
        MSG_S2C_ENTITY_STATE  = 17, // S→C  [2B 数量 n]{[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags]}×n（不含接收者自己）
        MSG_S2C_ENTITY_LEAVE  = 18, // S→C  [4B PlayerId]（成员离开房间/断线 → 对端销毁角色）
    };

    constexpr int32 HeaderSize = 4;   // 2B len + 2B msgid

    struct FFrame
    {
        uint16 MsgId = 0;
        TArray<uint8> Payload;
    };

    // 从字节流头部尝试切一个完整帧；半包返回 false（数据保留）
    // 坏帧（len < 2）：丢弃全部已缓冲数据（防卡死），返回 false —— 与服务器 FrameCodec 行为一致
    // ⚠ Out.Payload 必须先 Reset 再 Append——FFrame 会被调用方跨迭代复用，粘包批次（M4 30Hz 常态）
    //   若不清空，前帧字节会滚进后帧（2026-10-17 M6 联调实锤：幻影人数 5/4 与错位结果码的根因）
    inline bool TryDecode(TArray<uint8>& Buffer, FFrame& Out)
    {
        if (Buffer.Num() < HeaderSize) return false;
        const uint16 Len = static_cast<uint16>(Buffer[0]) | (static_cast<uint16>(Buffer[1]) << 8);
        if (Len < 2)
        {
            Buffer.Empty();
            return false;
        }
        if (Buffer.Num() < Len + 2) return false;

        Out.MsgId = static_cast<uint16>(Buffer[2]) | (static_cast<uint16>(Buffer[3]) << 8);
        Out.Payload.Reset();                        // 修复（M6 联调）：Append 是追加——FFrame 跨迭代复用时
        Out.Payload.Append(Buffer.GetData() + HeaderSize, Len - 2);   // 多帧粘包会把前帧字节滚进后帧（5/4 幻影人数的根因）
        Buffer.RemoveAt(0, Len + 2);
        return true;
    }

    // 打帧：msgid + payload → [len][msgid][payload]
    inline TArray<uint8> Encode(uint16 MsgId, const TArray<uint8>& Payload)
    {
        const uint16 Len = static_cast<uint16>(2 + Payload.Num());
        TArray<uint8> Out;
        Out.Reserve(Len + 2);
        Out.Add(static_cast<uint8>(Len & 0xFF));
        Out.Add(static_cast<uint8>((Len >> 8) & 0xFF));
        Out.Add(static_cast<uint8>(MsgId & 0xFF));
        Out.Add(static_cast<uint8>((MsgId >> 8) & 0xFF));
        Out.Append(Payload);
        return Out;
    }

    inline void AppendU64(TArray<uint8>& Out, uint64 V)
    {
        for (int32 i = 0; i < 8; ++i) Out.Add(static_cast<uint8>((V >> (8 * i)) & 0xFF));
    }

    inline uint64 ReadU64(const TArray<uint8>& In, int32 Offset)
    {
        uint64 V = 0;
        for (int32 i = 0; i < 8; ++i) V |= static_cast<uint64>(In[Offset + i]) << (8 * i);
        return V;
    }

    inline uint32 ReadU32(const TArray<uint8>& In, int32 Offset)
    {
        return static_cast<uint32>(In[Offset]) | (static_cast<uint32>(In[Offset + 1]) << 8)
             | (static_cast<uint32>(In[Offset + 2]) << 16) | (static_cast<uint32>(In[Offset + 3]) << 24);
    }
}
