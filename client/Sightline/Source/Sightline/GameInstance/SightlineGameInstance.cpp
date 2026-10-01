// USightlineGameInstance 实现——对应心跳时序图 UE 侧（④⑨ 验收画面的产生地）
// 日志规范（LogSightline 分类）：
//   Log     = 关键事件（连接成功/断开/WELCOME/重连/LAG 恢复）
//   Warning = 可恢复异常（连接失败/LAG 进入/写失败）
//   Error   = 不可恢复
//   Verbose = 心跳与收发明细（默认不显示，调试时在日志窗口开 Verbose）
#include "GameInstance/SightlineGameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Async/Async.h"
#include "Misc/DateTime.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "Sightline.h"

using namespace SightlineProtocol;

namespace
{
    constexpr int32  kHudMessageKey   = 1;        // 覆盖式刷新（同一 key）
    constexpr float  kHeartbeatPeriod = 1.0f;     // 1s 一跳
    constexpr double kLagThresholdMs  = 3000.0;   // 3s 无 ACK = LAG
    constexpr float  kReconnectDelay  = 3.0f;     // 断开/失败后 3s 自动重连
    inline double MonotonicMs() { return FPlatformTime::Seconds() * 1000.0; }
}

void USightlineGameInstance::Init()
{
    Super::Init();

    // UGameInstance 无 Tick 虚函数——FTSTicker 每帧回调（HUD 刷新 + LAG 判定）
    TickerDelegate.BindUObject(this, &USightlineGameInstance::HandleTick);
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TickerDelegate, 0.f);

    UE_LOG(LogSightline, Log, TEXT("[GI] 初始化完成，发起连接 127.0.0.1:8888"));
    Connect();   // PIE 即连；心跳在收到 WELCOME（连接确认）后才启动
}

void USightlineGameInstance::Shutdown()
{
    bShuttingDown = true;   // Shutdown 触发的断开不允许自动重连
    UE_LOG(LogSightline, Log, TEXT("[GI] Shutdown：断开连接并清理"));
    if (TickerHandle.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
    }
    StopHeartbeat();
    Disconnect();
    Super::Shutdown();
}

bool USightlineGameInstance::HandleTick(float DeltaTime)
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

    // LAG 进入判定：已连接但超过阈值无 ACK
    if (bConnected && !bLag && MonotonicMs() - LastAckMonotonicMs > kLagThresholdMs)
    {
        bLag = true;
        UE_LOG(LogSightline, Warning, TEXT("[GI] %.0f ms 无 ACK → LAG"), kLagThresholdMs);
    }
    return true;   // true = 继续调度
}

void USightlineGameInstance::Connect()
{
    if (bShuttingDown) return;
    if (Connection)
    {
        ScheduleReconnect();   // 上次失败残留对象——走重试而非重建
        return;
    }

    UE_LOG(LogSightline, Log, TEXT("[GI] 连接 127.0.0.1:8888 ……"));
    Connection = MakeUnique<FSightlineConnection>();
    // BindLambda 捕获弱指针：GameInstance 析构后在途 AsyncTask 执行时安全跳过（防悬垂）
    TWeakObjectPtr<USightlineGameInstance> WeakThis(this);
    Connection->OnFrame.BindLambda([WeakThis](uint16 MsgId, const TArray<uint8>& Payload)
    {
        if (USightlineGameInstance* GI = WeakThis.Get())
        {
            GI->OnFrameReceived(MsgId, Payload);
        }
    });
    Connection->OnDisconnected.BindLambda([WeakThis](const FString& Reason)
    {
        if (USightlineGameInstance* GI = WeakThis.Get())
        {
            GI->OnConnectionLost(Reason);
        }
    });

    if (Connection->Connect(TEXT("127.0.0.1"), 8888))
    {
        UE_LOG(LogSightline, Log, TEXT("[GI] 连接请求已发起（等待 WELCOME 确认）"));
    }
    else
    {
        // 服务器可能后起——3 秒后自动重试
        Connection.Reset();
        UE_LOG(LogSightline, Warning, TEXT("[GI] 连接失败，%.0f 秒后重试"), kReconnectDelay);
        ScheduleReconnect();
    }
}

void USightlineGameInstance::ScheduleReconnect()
{
    if (bShuttingDown) return;
    if (FTimerManager* TM = GetTimerManagerSafe())
    {
        FTimerHandle RetryHandle;
        TM->SetTimer(RetryHandle, FTimerDelegate::CreateUObject(this, &USightlineGameInstance::Connect),
                     kReconnectDelay, false);
    }
}

void USightlineGameInstance::OnConnectionLost(const FString& Reason)
{
    // 接收线程退出/异常断开（游戏线程回调）——停心跳 → 状态复位 → 3s 自动重连
    StopHeartbeat();
    bConnected = false;
    bLag = false;
    Connection.Reset();
    UE_LOG(LogSightline, Warning, TEXT("[GI] 连接丢失（%s），%.0f 秒后自动重连"), *Reason, kReconnectDelay);
    ScheduleReconnect();
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
        UE_LOG(LogSightline, Log, TEXT("[GI] 心跳启动（每 %.0f 秒）"), kHeartbeatPeriod);
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
    const uint64 ClientTs = static_cast<uint64>(FDateTime::UtcNow().GetTicks() / ETimespan::TicksPerMillisecond);

    TArray<uint8> Payload;                               // [4B seq][8B clientTs]
    Payload.Add(static_cast<uint8>(HeartbeatSeq & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 8) & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 16) & 0xFF));
    Payload.Add(static_cast<uint8>((HeartbeatSeq >> 24) & 0xFF));
    AppendU64(Payload, ClientTs);

    if (Connection) Connection->Send(MSG_C2S_HEARTBEAT, Payload);
    PendingHeartbeats.Add(HeartbeatSeq, MonotonicMs());
    UE_LOG(LogSightline, Verbose, TEXT("[GI] HEARTBEAT seq=%u 发送"), HeartbeatSeq);
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
        UE_LOG(LogSightline, Log, TEXT("[GI] WELCOME 收到 → Connected（验收画面①）"));
        break;

    case MSG_S2C_HEARTBEAT_ACK:
    {
        if (Payload.Num() < 12)
        {
            UE_LOG(LogSightline, Warning, TEXT("[GI] ACK 长度异常: %d"), Payload.Num());
            break;
        }
        const uint32 Seq = ReadU32(Payload, 0);
        if (double* SendMs = PendingHeartbeats.Find(Seq))
        {
            LastPingMs = static_cast<uint32>(FMath::Max(0.0, MonotonicMs() - *SendMs));
            PendingHeartbeats.Remove(Seq);
            UE_LOG(LogSightline, Verbose, TEXT("[GI] ACK seq=%u ping=%ums"), Seq, LastPingMs);
        }
        else
        {
            UE_LOG(LogSightline, Verbose, TEXT("[GI] ACK seq=%u（过期/未知，忽略）"), Seq);
        }
        if (bLag)
        {
            bLag = false;
            UE_LOG(LogSightline, Log, TEXT("[GI] ACK 恢复 → LAG 解除"));
        }
        LastAckMonotonicMs = MonotonicMs();
        break;
    }
    default:
        UE_LOG(LogSightline, Verbose, TEXT("[GI] 未知消息 msgid=%u（忽略）"), MsgId);
        break;
    }
}
