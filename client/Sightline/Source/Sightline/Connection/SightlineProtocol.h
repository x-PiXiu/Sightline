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
    };

    constexpr int32 HeaderSize = 4;   // 2B len + 2B msgid

    struct FFrame
    {
        uint16 MsgId = 0;
        TArray<uint8> Payload;
    };

    // 从字节流头部尝试切一个完整帧；半包返回 false（数据保留）
    // 坏帧（len < 2）：丢弃全部已缓冲数据（防卡死），返回 false —— 与服务器 FrameCodec 行为一致
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
        Out.Payload.Append(Buffer.GetData() + HeaderSize, Len - 2);
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
