// USightlineGameInstance 实现——对应心跳时序图 UE 侧（④⑨ 验收画面的产生地）
#include "SightlineGameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Async/Async.h"
#include "Misc/DateTime.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

using namespace SightlineProtocol;

namespace
{
    constexpr int32  kHudMessageKey   = 1;        // 覆盖式刷新（同一 key）
    constexpr float  kHeartbeatPeriod = 1.0f;     // 1s 一跳
    constexpr double kLagThresholdMs  = 3000.0;   // 3s 无 ACK = LAG
    inline double MonotonicMs() { return FPlatformTime::Seconds() * 1000.0; }
}

void USightlineGameInstance::Init()
{
    Super::Init();
    Connect();   // PIE 即连（第 1 期最简）；心跳在收到 WELCOME 后启动（连接确认前不发）
}

void USightlineGameInstance::Shutdown()
{
    StopHeartbeat();
    Disconnect();
    Super::Shutdown();
}

void USightlineGameInstance::Tick(float DeltaTime)
{
    // HUD（验收画面 ①②）：覆盖式刷新同一 key
    if (GEngine)
    {
        FString Status = bConnected ? TEXT("Connected") : TEXT("Disconnected");
        if (bConnected && bLag) Status += TEXT("  LAG");
        const FColor Color = bConnected ? (bLag ? FColor::Orange : FColor::Green) : FColor::Red;
        GEngine->AddOnScreenDebugMessage(kHudMessageKey, 0.f, Color,
            FString::Printf(TEXT("%s   ping=%ums   seq=%u"), *Status, LastPingMs, HeartbeatSeq));
    }

    // LAG 判定：已连接但超过阈值无 ACK
    if (bConnected && !bLag && MonotonicMs() - LastAckMonotonicMs > kLagThresholdMs)
    {
        bLag = true;
    }
}

void USightlineGameInstance::Connect()
{
    if (Connection) return;

    Connection = MakeUnique<FSightlineConnection>();
    // 帧回调由 AsyncTask 保证在游戏线程执行——此处可安全碰 UE 对象
    Connection->OnFrame.BindUObject(this, &USightlineGameInstance::OnFrameReceived);
    Connection->OnDisconnected.BindLambda([this](const FString& Reason)
    {
        StopHeartbeat();
        bConnected = false;
        UE_LOG(LogTemp, Warning, TEXT("[Sightline] 断开: %s"), *Reason);
    });

    if (Connection->Connect(TEXT("127.0.0.1"), 8888))
    {
        UE_LOG(LogTemp, Log, TEXT("[Sightline] 连接请求已发起 127.0.0.1:8888（等待 WELCOME）"));
    }
    else
    {
        Connection.Reset();
        UE_LOG(LogTemp, Error, TEXT("[Sightline] 连接失败（服务器未启动？）"));
    }
}

void USightlineGameInstance::Disconnect()
{
    StopHeartbeat();
    if (Connection)
    {
        Connection->Disconnect(TEXT("主动断开"));
        Connection.Reset();
    }
    bConnected = false;
}

FTimerManager* USightlineGameInstance::GetTimerManagerSafe()
{
    // World 可能尚未就绪（Init 阶段）或已销毁（Shutdown 阶段）——调用方判空
    UWorld* World = GetWorld();
    return World ? &World->GetTimerManager() : nullptr;
}

void USightlineGameInstance::StartHeartbeat()
{
    if (FTimerManager* TM = GetTimerManagerSafe())
    {
        TM->SetTimer(HeartbeatTimerHandle,
            FTimerDelegate::CreateUObject(this, &USightlineGameInstance::HandleHeartbeatTick),
            kHeartbeatPeriod, true);
    }
}

void USightlineGameInstance::StopHeartbeat()
{
    if (FTimerManager* TM = GetTimerManagerSafe())
    {
        TM->ClearTimer(HeartbeatTimerHandle);
    }
}

void USightlineGameInstance::HandleHeartbeatTick()
{
    ++HeartbeatSeq;
    const uint64 ClientTs = static_cast<uint64>(FDateTime::UtcNow().ToUnixMsMilliseconds());

    TArray<uint8> Payload;                               // [4B seq][8B clientTs]
    Payload.Add(static_cast<uint8>(HeartbeatSeq & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 8) & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 16) & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 24) & 0xFF));
    AppendU64(Payload, ClientTs);

    if (Connection) Connection->Send(MSG_C2S_HEARTBEAT, Payload);
    PendingHeartbeats.Add(HeartbeatSeq, MonotonicMs());  // 发送时刻（单调时钟——ping 用）
}

void USightlineGameInstance::OnFrameReceived(uint16 MsgId, const TArray<uint8>& Payload)
{
    // 只在游戏线程执行（AsyncTask 保证）——可以安全碰 UE 对象
    switch (MsgId)
    {
    case MSG_S2C_WELCOME:
        bConnected = true;
        bLag = false;
        LastAckMonotonicMs = MonotonicMs();
        StartHeartbeat();                                // 连接确认后才发心跳
        UE_LOG(LogTemp, Log, TEXT("[Sightline] WELCOME 收到 → Connected（验收画面①）"));
        break;

    case MSG_S2C_HEARTBEAT_ACK:
    {
        if (Payload.Num() < 12) break;
        const uint32 Seq = ReadU32(Payload, 0);
        if (double* SendMs = PendingHeartbeats.Find(Seq))
        {
            LastPingMs = static_cast<uint32>(FMath::Max(0.0, MonotonicMs() - *SendMs));
            PendingHeartbeats.Remove(Seq);
        }
        LastAckMonotonicMs = MonotonicMs();
        bLag = false;
        // 验收画面 ②：ping 已更新，HUD 由 Tick 刷新
        break;
    }
    default:
        break;
    }
}
