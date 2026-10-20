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
    // —— M3b 和解参数（D4-06 批的初值，实测定——军令状不锁阈值）——
    constexpr float kReconcileEpsilonCm = 5.f;          // ε：误差<5cm 无感通过（网络抖动不触发和解）
    constexpr int32 kInputHistoryTarget = 30;           // 1s 输入史 @30Hz 上报
    constexpr int32 kInputHistoryCap = 64;              // 硬顶（防帧率暴涨撑爆）
}

void USightlineMatchSession::BindConnection(ISendPort* InConn) {
    Conn = InConn;
}

void USightlineMatchSession::SetRegistry(USightlineEntityRegistry* InRegistry) {
    Registry = InRegistry;
}

void USightlineMatchSession::NotifyLinkLost() {
    bInMatch = false;                                  // 链路死 = 对局死
    InputHistory.Reset();                              // 底账随会话清场（重连后从零起账）
    if (Registry) Registry->Clear();                   // 会话随链路死亡（重连后重新 LOGIN，名册重建）
}

void USightlineMatchSession::StartMatch() {
    if (!Conn) return;
    TArray<uint8> Empty;
    Conn->Send(SightlineProtocol::MSG_C2S_START_MATCH, Empty);   // v3 契约 14：空 payload
}

void USightlineMatchSession::ReportLocalState(const FSightlineLocalState& S) {
    // v4·R10：对局期上行 = 19 PLAYER_INPUT（25B·带本地预测序号）；未进对局不上报
    if (!Conn || !bInMatch || SelfPlayerId == 0) return;
    SightlineProtocol::FPlayerInput In;                       // Encode 同居协议头（variant 纪律的客户端侧）
    In.Seq = ++InputSeq;                                      // M3：此 seq 即预测历史锚点
    In.X = S.Pos.X; In.Y = S.Pos.Y; In.Z = S.Pos.Z;
    In.Yaw = S.Yaw; In.Pitch = S.Pitch;
    In.Flags = S.bFalling ? 1 : 0;
    Conn->Send(SightlineProtocol::MSG_C2S_PLAYER_INPUT, In.Encode());
    // M3b：入环形缓冲——(seq,pos,yaw)×1s，对账/重放的本地底账（与发出的字节同源同 seq）
    InputHistory.Add({In.Seq, S.Pos, S.Yaw});
    while (InputHistory.Num() > kInputHistoryCap ||
           (InputHistory.Num() > kInputHistoryTarget &&
            static_cast<int32>(In.Seq - InputHistory[0].Seq) > kInputHistoryTarget))
        InputHistory.RemoveAt(0);
    // KPI② 误差日志（发送端真实轨迹；对齐锚 = MATCH_START 行，离线按 t0 对齐）
    if (CSightlineStatLog.GetValueOnGameThread() != 0)
        UE_LOG(LogTemp, Log, TEXT("[STAT] SELF t=%.3f pid=%d seq=%u x=%.2f y=%.2f z=%.2f yaw=%.1f pitch=%.1f f=%d"),
               FPlatformTime::Seconds(), SelfPlayerId, In.Seq, S.Pos.X, S.Pos.Y, S.Pos.Z,
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
        bInMatch = true;                               // 对局激活：19 上报窗口开启（v4·R10）
        InputHistory.Reset();                          // M3b：新对局=新底账（环形缓冲与 HUD 统计归零）
        ReconcileCount = 0; ReplayFrames = 0;
        ErrCurrentCm = 0.0; ErrPeakCm = 0.0; LastAckedSeq = 0;
        if (CSightlineStatLog.GetValueOnGameThread() != 0)   // 误差曲线对齐锚（plot_stat_curve：各端以最后锚为 t0）
            UE_LOG(LogTemp, Log, TEXT("[STAT] MATCH_START t=%.3f"), FPlatformTime::Seconds());
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
            // v4：29B/实体（+4B ackedSeq，契约破例 D4-01）——解析统一走协议头 Parse（Decode 集中化）
            SightlineProtocol::FEntityStateV2 Ent;
            if (!SightlineProtocol::FEntityStateV2::Parse(Payload, Off, Ent)) return;   // 坏帧防御：整包放弃
            Off += SightlineProtocol::FEntityStateV2::Size;
            const int32 Pid = static_cast<int32>(Ent.PlayerId);
            if (Pid == SelfPlayerId) {
                // v4.1：自实体随包下发 = 本地预测的对账基准——走和解，绝不直设本地 Pawn
                ReconcileSelf(Ent.AckedSeq, FVector(Ent.X, Ent.Y, Ent.Z));
                continue;
            }
            Registry->ApplyState(Pid,
                                 FVector(Ent.X, Ent.Y, Ent.Z), Ent.Yaw, Ent.Pitch,
                                 (Ent.Flags & 0x01) != 0, static_cast<int32>(Ent.AckedSeq));
            if (CSightlineStatLog.GetValueOnGameThread() != 0)
                UE_LOG(LogTemp, Log, TEXT("[STAT] PEER t=%.3f pid=%d ack=%u x=%.2f y=%.2f z=%.2f f=%d"),
                       FPlatformTime::Seconds(), Pid, Ent.AckedSeq,
                       Ent.X, Ent.Y, Ent.Z, (Ent.Flags & 0x01) != 0);
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
}

// —— M3b 三方对账（设计草案 §2.3 + reconcile_state 定稿状态机）——
// 权威位（服务器按输入自推+钳制）vs 本地同 seq 预测位（环形缓冲存根）：
//   < ε 无感通过（网络抖动不触发和解）｜≥ ε 回滚+重放｜断档 全量回滚兑底
void USightlineMatchSession::ReconcileSelf(uint32 AckedSeq, const FVector& AuthPos) {
    LastAckedSeq = AckedSeq;
    const int32 Idx = InputHistory.IndexOfByPredicate(
        [AckedSeq](const FInputSample& S) { return S.Seq == AckedSeq; });

    if (Idx == INDEX_NONE) {
        // seq 断档（缓冲中无此序号）→ 朴素兑底：全量回滚到最新权威位（预测弃置，重放 0 帧）
        ErrCurrentCm = InputHistory.Num() > 0 ? (AuthPos - InputHistory.Last().Pos).Size() : 0.0;
        ErrPeakCm = FMath::Max(ErrPeakCm, ErrCurrentCm);
        ++ReconcileCount;
        ReplayFrames = 0;
        OnSelfCorrected.Broadcast(AuthPos, /*bFullSnap=*/true);
        if (CSightlineStatLog.GetValueOnGameThread() != 0)
            UE_LOG(LogTemp, Log, TEXT("[STAT] RECON t=%.3f ack=%u err=%.2f replays=0 full=1"),
                   FPlatformTime::Seconds(), AckedSeq, ErrCurrentCm);
        return;
    }

    const FVector Err = AuthPos - InputHistory[Idx].Pos;   // 对账误差向量（权威 - 本地同 seq 预测位）
    ErrCurrentCm = Err.Size();
    ErrPeakCm = FMath::Max(ErrPeakCm, ErrCurrentCm);
    ReplayFrames = InputHistory.Num() - 1 - Idx;            // 其后本地输入帧数 = 重放量（HUD 可见）
    if (ErrCurrentCm < kReconcileEpsilonCm) return;         // 无感通过：只更新服务器认知，不碰 Pawn

    ++ReconcileCount;
    // 回滚 + 重放（位移空间）：渲染位 += 误差向量——纯平移运动下与"摆回权威位再逐输入重放"
    // 数学等价且不与 CMC 相搏；穿墙/碰撞级的逐帧输入重放不在本期范围（按需后置）
    OnSelfCorrected.Broadcast(Err, /*bFullSnap=*/false);
    if (CSightlineStatLog.GetValueOnGameThread() != 0)
        UE_LOG(LogTemp, Log, TEXT("[STAT] RECON t=%.3f ack=%u err=%.2f replays=%d full=0"),
               FPlatformTime::Seconds(), AckedSeq, ErrCurrentCm, ReplayFrames);
}
