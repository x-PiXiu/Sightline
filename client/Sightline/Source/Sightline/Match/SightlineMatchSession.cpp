// SightlineMatchSession 实现 —— 意图发包 + 15/17/18 解析喂 Registry
#include "Match/SightlineMatchSession.h"
#include "Sightline.h"
#include <cstring>
#include "Connection/SightlineSendPort.h"
#include "Connection/SightlineProtocol.h"

namespace {
    // v3 契约小端读取（与 SightlineProtocol 编码镜像；调用方保证边界）
    uint32 ReadU32(const TArray<uint8>& P, const int32 Off) {
        return static_cast<uint32>(P[Off]) |
               (static_cast<uint32>(P[Off + 1]) << 8) |
               (static_cast<uint32>(P[Off + 2]) << 16) |
               (static_cast<uint32>(P[Off + 3]) << 24);
    }
}

void USightlineMatchSession::BindConnection(ISendPort* InConn) {
    Conn = InConn;
}

void USightlineMatchSession::SetRegistry(USightlineEntityRegistry* InRegistry) {
    Registry = InRegistry;
}

void USightlineMatchSession::NotifyLinkLost() {
    bInMatch = false;                                  // 链路死 = 对局死
    if (Registry) Registry->Clear();                   // 会话随链路死亡（重连后重新 LOGIN，名册重建）
}

void USightlineMatchSession::StartMatch() {
    if (!Conn) return;
    TArray<uint8> Empty;
    Conn->Send(SightlineProtocol::MSG_C2S_START_MATCH, Empty);   // v3 契约 14：空 payload
}

void USightlineMatchSession::ReportLocalState(const FSightlineLocalState& S) {
    // 16 是对局期消息：未进对局（PIE 直开对局图等场景）不上报——pid=0 空转包被服务器忽略但白占带宽
    if (!Conn || !bInMatch || SelfPlayerId == 0) return;
    TArray<uint8> P;                                            // v3 契约 16：[12B pos][4B yaw][4B pitch][1B flags]
    const auto F32 = [&P](float V) {
        uint32 Bits = 0;
        static_assert(sizeof(float) == sizeof(uint32_t), "f32 布局假设");
        std::memcpy(&Bits, &V, sizeof(uint32_t));
        for (int i = 0; i < 4; ++i) P.Add(static_cast<uint8>((Bits >> (8 * i)) & 0xFF));
    };
    F32(S.Pos.X); F32(S.Pos.Y); F32(S.Pos.Z);
    F32(S.Yaw); F32(S.Pitch);
    P.Add(S.bFalling ? 1 : 0);
    Conn->Send(SightlineProtocol::MSG_C2S_MOVE_REPORT, P);
    // KPI② 误差日志（发送端真实轨迹；对齐锚 = MATCH_START 行，离线按 t0 对齐）
    if (CSightlineStatLog.GetValueOnGameThread() != 0)
        UE_LOG(LogTemp, Log, TEXT("[STAT] SELF t=%.3f pid=%d x=%.2f y=%.2f z=%.2f yaw=%.1f pitch=%.1f f=%d"),
               FPlatformTime::Seconds(), SelfPlayerId, S.Pos.X, S.Pos.Y, S.Pos.Z,
               S.Yaw, S.Pitch, S.bFalling ? 1 : 0);
}

void USightlineMatchSession::HandleFrame(const uint16 MsgId, const TArray<uint8>& Payload) {
    if (!Registry) return;

    if (MsgId == SightlineProtocol::MSG_S2C_MATCH_START) {
        if (Payload.Num() < 4) return;
        const int32 Count = static_cast<int32>(ReadU32(Payload, 0));
        TArray<FSightlineEntityState> Roster;
        int32 Off = 4;
        for (int32 i = 0; i < Count; ++i) {
            if (Payload.Num() < Off + 5) return;       // 坏帧防御：整包放弃
            FSightlineEntityState S;
            S.PlayerId = static_cast<int32>(ReadU32(Payload, Off));
            Off += 4;
            const uint8 NameLen = Payload[Off]; ++Off;
            if (Payload.Num() < Off + NameLen + 1) return;
            FUTF8ToTCHAR ToTchar(reinterpret_cast<const ANSICHAR*>(&Payload[Off]), NameLen);
            S.Name = FString(ToTchar.Get(), ToTchar.Length());
            Off += NameLen;
            S.SpawnIndex = Payload[Off]; ++Off;
            Roster.Add(S);
        }
        bInMatch = true;                               // 对局激活：16 上报窗口开启
        Registry->ApplyMatchStart(SelfPlayerId, Roster);
        OnMatchStarted.Broadcast();                    // GI 切对局关卡（生成由 PawnSpawner 延迟执行）
        return;
    }

    if (MsgId == SightlineProtocol::MSG_S2C_ENTITY_STATE) {
        if (Payload.Num() < 2) return;
        const int16 SignedCount = static_cast<int16>(Payload[0] | (Payload[1] << 8));
        const int32 Count = SignedCount;
        int32 Off = 2;
        for (int32 i = 0; i < Count; ++i) {
            if (Payload.Num() < Off + 25) return;               // 坏帧防御：整包放弃
            const int32 Pid = static_cast<int32>(ReadU32(Payload, Off));
            const auto rdF = [&Payload](int O) {
                float F;
                std::memcpy(&F, &Payload[O], sizeof(float));
                return F;
            };
            // 布局 [4B pid][12B pos][4B yaw][4B pitch][1B flags]——ReadU32(pid) 后字段必须 +4
            // （缺 +4 = 全字段错位：x 吃 pid 字节、z 吃 y、yaw 吃 z——远端被摆到垃圾坐标，2026-10-17 实锤）
            const FVector Pos(rdF(Off + 4), rdF(Off + 8), rdF(Off + 12));
            const float Yaw = rdF(Off + 16), Pitch = rdF(Off + 20);
            const bool bFalling = (Payload[Off + 24] & 0x01) != 0;
            Off += 25;
            Registry->ApplyState(Pid, Pos, Yaw, Pitch, bFalling);
            if (CSightlineStatLog.GetValueOnGameThread() != 0)
                UE_LOG(LogTemp, Log, TEXT("[STAT] PEER t=%.3f pid=%d x=%.2f y=%.2f z=%.2f f=%d"),
                       FPlatformTime::Seconds(), Pid, Pos.X, Pos.Y, Pos.Z, bFalling ? 1 : 0);
        }
        return;
    }

    if (MsgId == SightlineProtocol::MSG_S2C_ENTITY_LEAVE) {
        if (Payload.Num() < 4) return;
        const int32 Pid = static_cast<int32>(ReadU32(Payload, 0));
        if (Pid == SelfPlayerId) bInMatch = false;     // 自己离场：上报窗口关闭
        Registry->ApplyLeave(Pid);
        OnEntityLeft.Broadcast(Pid);
        return;
    }

    // 17 ENTITY_STATE：M4 直发同步实装（ApplyState → OnEntityUpdated → PawnSpawner 摆姿势）
}
