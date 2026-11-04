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
        MSG_S2C_ENTITY_STATE  = 17, // S→C v2 [2B 数量 n]{[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags][4B ackedSeq]}×n（29B/实体·不含自己；v4 破例 D4-01）
        MSG_S2C_ENTITY_LEAVE  = 18, // S→C  [4B PlayerId]（成员离开房间/断线 → 对端销毁角色）
        // —— 第 4 期：预测与和解（与 docs/protocol/MSG_IDS.md v4 保持同步）——
        MSG_C2S_PLAYER_INPUT  = 19, // C→S  [4B seq][12B pos][4B yaw][4B pitch][1B flags]=25B（对局期替代 16·R10）
        // —— 第 5 期：开火与命中（与 docs/protocol/MSG_IDS.md v4.2 保持同步）——
        MSG_C2S_FIRE          = 20, // C→S  (空 payload)——判定全用服务器权威状态，方向都不收（D5-01）
        MSG_S2C_FIRE_RESULT   = 21, // S→C  [4B shooterPid][1B result][4B victimPid][12B hitPoint][1B victimHp]=22B（房内全员广播·含射手）
        MSG_C2S_RTT_REPORT    = 22, // C→S  [2B rttMs][2B upKB/s][2B downKB/s]=6B·1Hz——ep06 回溯量源+带宽计数预埋（ep12 消费）
        // —— 第 7 期：死亡/重生/结算 + 人机席位（v4.4）——
        MSG_S2C_KILL           = 23, // S→C  [4B victimPid][4B killerPid][2B respawnDelayMs]=10B——房内全员广播
        MSG_S2C_RESPAWN        = 24, // S→C  [4B pid][1B spawnIndex][2B invulnMs]=7B——服务器选点下发序号
        MSG_S2C_MATCH_END      = 25, // S→C  [1B 原因][1B n]{[4B pid][2B kills][2B deaths]}×n
        MSG_C2S_BOT_ADD        = 26, // C→S  [1B 难度]——仅房主
        MSG_C2S_BOT_REMOVE     = 27, // C→S  [4B botPid]——仅房主
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

    inline float ReadF32(const TArray<uint8>& In, int32 Offset)
    {
        float F;
        FMemory::Memcpy(&F, In.GetData() + Offset, sizeof(float));
        return F;
    }

    // ═══ v4 消息结构体（契约破例 D4-01 已批；与服务端 protocol/messages.h 字节级一致）═══
    // 纪律：Encode/Parse 与常量同居本头——编码解码不再两地分居（双端漂移土壤铲除）

    /** 19 PLAYER_INPUT（25B）：[4B seq][12B pos][4B yaw][4B pitch][1B flags] */
    struct FPlayerInput
    {
        uint32 Seq = 0;
        float X = 0.f, Y = 0.f, Z = 0.f;
        float Yaw = 0.f, Pitch = 0.f;
        uint8 Flags = 0;

        static constexpr int32 Size = 25;

        TArray<uint8> Encode() const
        {
            TArray<uint8> Out;
            Out.Reserve(Size);
            const auto AddU32 = [&Out](uint32 V)
            {
                for (int32 i = 0; i < 4; ++i) Out.Add(static_cast<uint8>((V >> (8 * i)) & 0xFF));
            };
            const auto AddF = [&Out](float F)
            {
                uint32 Bits;
                FMemory::Memcpy(&Bits, &F, sizeof(float));
                for (int32 i = 0; i < 4; ++i) Out.Add(static_cast<uint8>((Bits >> (8 * i)) & 0xFF));
            };
            AddU32(Seq);
            AddF(X); AddF(Y); AddF(Z);
            AddF(Yaw); AddF(Pitch);
            Out.Add(Flags);
            return Out;
        }
    };

    /** 17 号 v2 实体（29B）：[4B PlayerId][12B pos][4B yaw][4B pitch][1B flags][4B ackedSeq] */
    struct FEntityStateV2
    {
        uint32 PlayerId = 0;
        float X = 0.f, Y = 0.f, Z = 0.f;
        float Yaw = 0.f, Pitch = 0.f;
        uint8 Flags = 0;
        uint32 AckedSeq = 0;      // 服务器已确认输入序号（v4·R11；和解对账基准，M3 消费）

        static constexpr int32 Size = 29;

        // 从 Payload 偏移处解析一条；越界返回 false（调用方整包放弃）
        static bool Parse(const TArray<uint8>& Payload, int32 Offset, FEntityStateV2& Out)
        {
            if (Payload.Num() < Offset + Size) return false;
            Out.PlayerId   = ReadU32(Payload, Offset);
            Out.X          = ReadF32(Payload, Offset + 4);
            Out.Y          = ReadF32(Payload, Offset + 8);
            Out.Z          = ReadF32(Payload, Offset + 12);
            Out.Yaw        = ReadF32(Payload, Offset + 16);
            Out.Pitch      = ReadF32(Payload, Offset + 20);
            Out.Flags      = Payload[Offset + 24];
            Out.AckedSeq   = ReadU32(Payload, Offset + 25);
            return true;
        }
    };

    /** 21 FIRE_RESULT（v4.6·23B）：[4B shooterPid][1B result][4B victimPid][12B hitPoint][1B victimHp][1B gate]
     *  房内全员广播（含射手自己）——一条消息三方共用：射手 Hitmarker / 其余 tracer / 受击者红闪。
     *  gate 非 0=被武器门拒（2026-11-05 弹匣漂移根治：回传射手撤销本地预测扣弹——双端弹匣/换弹钟锁相） */
    struct FFireResult
    {
        uint32 ShooterPid = 0;
        uint8 Result = 0;         // 0=未命中 / 1=命中
        uint32 VictimPid = 0;
        float HitX = 0.f, HitY = 0.f, HitZ = 0.f;   // 命中面/射线终点（tracer 停墙）
        uint8 VictimHp = 0;       // 命中后余血（HUD 血量行；ep07 接血条）
        uint8 Gate = 0;           // 0=正常判定 / 非0=被武器门拒（1 换弹 2 弹匣 3 射速 4 死亡）

        static constexpr int32 Size = 23;

        static bool Parse(const TArray<uint8>& Payload, FFireResult& Out)
        {
            if (Payload.Num() < Size) return false;
            Out.ShooterPid = ReadU32(Payload, 0);
            Out.Result     = Payload[4];
            Out.VictimPid  = ReadU32(Payload, 5);
            Out.HitX       = ReadF32(Payload, 9);
            Out.HitY       = ReadF32(Payload, 13);
            Out.HitZ       = ReadF32(Payload, 17);
            Out.VictimHp   = Payload[21];
            Out.Gate       = Payload[22];
            return true;
        }
    };
// ===== 第 7 期：死亡/重生/结算（v4.4）=====
struct FKillMsg {
    std::uint32_t victimPid = 0;
    std::uint32_t killerPid = 0;
    std::uint16_t respawnDelayMs = 0;
    static constexpr int32_t Size = 10;
    static bool Parse(const TArray<uint8_t>& P, FKillMsg& Out)
    {
        if (P.Num() < Size) return false;
        Out.victimPid = ReadU32(P, 0);
        Out.killerPid = ReadU32(P, 4);
        Out.respawnDelayMs = static_cast<uint16>(P[8]) | (static_cast<uint16>(P[9]) << 8);
        return true;
    }
};

struct FRespawnMsg {
    std::uint32_t pid = 0;
    std::uint8_t spawnIndex = 0;       // 服务器选点序号（比分/回放语义保留）
    std::uint16_t invulnMs = 0;
    FVector Pos = FVector::ZeroVector; // v4.5：服务器权威落位坐标（客户端传送用它，不再自查表——
                                       //  FindSpawnStart 关卡实时位置与烘焙合同漂移→锚定握手拒放行→
                                       //  和解拉扯+和解暴增，2026-11-03 实锤）
    float YawDeg = 0.f;
    static constexpr int32_t Size = 23;
    static bool Parse(const TArray<uint8_t>& P, FRespawnMsg& Out)
    {
        if (P.Num() < Size) return false;
        Out.pid = ReadU32(P, 0);
        Out.spawnIndex = P[4];
        Out.invulnMs = static_cast<uint16>(P[5]) | (static_cast<uint16>(P[6]) << 8);
        Out.Pos = FVector(ReadF32(P, 7), ReadF32(P, 11), ReadF32(P, 15));
        Out.YawDeg = ReadF32(P, 19);
        return true;
    }
};

struct FMatchEndRow {
    std::uint32_t pid = 0;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};

struct FMatchEndMsg {
    std::uint8_t reason = 0;           // 1=击杀上限 2=真人全离
    TArray<FMatchEndRow> rows;
    static bool Parse(const TArray<uint8_t>& P, FMatchEndMsg& Out)
    {
        if (P.Num() < 2) return false;
        Out.reason = P[0];
        const int32 N = P[1];
        if (P.Num() < 2 + N * 8) return false;
        for (int32 i = 0; i < N; ++i) {
            const int32 o = 2 + i * 8;
            FMatchEndRow r;
            r.pid = ReadU32(P, o);
            r.kills = static_cast<uint16>(P[o + 4]) | (static_cast<uint16>(P[o + 5]) << 8);
            r.deaths = static_cast<uint16>(P[o + 6]) | (static_cast<uint16>(P[o + 7]) << 8);
            Out.rows.Add(r);
        }
        return true;
    }
};
}
