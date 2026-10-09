// USightlineGameInstance 实现——对应心跳时序图 UE 侧（④⑨ 验收画面的产生地）
// 日志规范（LogSightline 分类）：
//   Log     = 关键事件（连接成功/断开/WELCOME/重连/LAG 恢复）
//   Warning = 可恢复异常（连接失败/LAG 进入/写失败）
//   Error   = 不可恢复
//   Verbose = 心跳与收发明细（默认不显示，调试时在日志窗口开 Verbose）
#include "GameInstance/SightlineGameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
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
    constexpr double kReconnectAfterLagMs = 5000.0;  // LAG 持续 5s 不恢复 → 主动断线自动重连
    constexpr float  kReconnectDelay  = 3.0f;     // 断开/失败后 3s 自动重连
    inline double MonotonicMs() { return FPlatformTime::Seconds() * 1000.0; }
}

void USightlineGameInstance::Init()
{
    Super::Init();

    // 第 2 期：大厅会话（先于 Connect 创建——Connect 里要绑定连接）
    LobbySession = NewObject<ULobbySession>(this);

    // 界面联动：会话委托 → GI 切屏（登录/大厅/房间/被踢/断线）
    LobbySession->OnLoginResult.AddDynamic(this, &USightlineGameInstance::HandleLoginResultForUI);
    LobbySession->OnRoomListChanged.AddDynamic(this, &USightlineGameInstance::HandleRoomListForUI);
    LobbySession->OnRoomStateChanged.AddDynamic(this, &USightlineGameInstance::HandleRoomStateForUI);
    LobbySession->OnKicked.AddDynamic(this, &USightlineGameInstance::HandleKickedForUI);
    LobbySession->OnLobbyLinkLost.AddDynamic(this, &USightlineGameInstance::HandleLinkLostForUI);

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
    // 第 2 期：视口就绪后展示登录界面（一次性；Init 时视口尚未就绪）
    if (!bInitialScreenShown && GEngine && GEngine->GameViewport)
    {
        bInitialScreenShown = true;
        ShowLoginScreen();
    }

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
        LagEnteredMonotonicMs = MonotonicMs();
        UE_LOG(LogSightline, Warning, TEXT("[GI] %.0f ms 无 ACK → LAG（未确认心跳 %d 个，已收 WELCOME=%d）"),
               kLagThresholdMs, PendingHeartbeats.Num(), bGotWelcome ? 1 : 0);
    }
    // LAG 持续不恢复 → 主动断线（半开连接被动等不到对端死亡，就主动拆）；
    // Disconnect 会经 OnDisconnected → OnConnectionLost → ScheduleReconnect 走既有自愈链
    if (bConnected && bLag && LagEnteredMonotonicMs > 0.0 &&
        MonotonicMs() - LagEnteredMonotonicMs > kReconnectAfterLagMs)
    {
        UE_LOG(LogSightline, Warning, TEXT("[GI] LAG 持续 %.0f ms → 主动断线，自动重连"),
               MonotonicMs() - LagEnteredMonotonicMs);
        Disconnect();
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
    if (LobbySession) LobbySession->BindConnection(Connection.Get());   // 会话绑定新连接
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
        if (LobbySession) LobbySession->BindConnection(nullptr);
        UE_LOG(LogSightline, Warning, TEXT("[GI] 连接失败，%.0f 秒后重试"), kReconnectDelay);
        ScheduleReconnect();
    }
}

void USightlineGameInstance::ScheduleReconnect()
{
    if (bShuttingDown) return;   // Shutdown 期间：不调度、不打印重连日志
    if (FTimerManager* TM = GetTimerManagerSafe())
    {
        FTimerHandle RetryHandle;
        TM->SetTimer(RetryHandle, FTimerDelegate::CreateUObject(this, &USightlineGameInstance::Connect),
                     kReconnectDelay, false);
        UE_LOG(LogSightline, Log, TEXT("[GI] %.0f 秒后自动重连"), kReconnectDelay);
    }
}

void USightlineGameInstance::OnConnectionLost(const FString& Reason)
{
    StopHeartbeat();
    bConnected = false;
    bLag = false;
    LagEnteredMonotonicMs = 0.0;
    if (LobbySession)
    {
        LobbySession->NotifyLinkLost();                   // 会话清空 + 通知 UI（被踢/断线区分开）
        LobbySession->BindConnection(nullptr);
    }
    Connection.Reset();
    if (bShuttingDown)
    {
        UE_LOG(LogSightline, Log, TEXT("[GI] 连接关闭（%s）—— 正在 Shutdown，不重连"), *Reason);
        return;
    }
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
    if (LobbySession) LobbySession->BindConnection(nullptr);   // Shutdown 路径静默解绑
    bConnected = false;
}

// ===== 第 2 期：界面装配与切换 =====

// Widget 类槽兜底：ini 可能被编辑器回写清空/加载时序不齐——路径固定，代码兜底最稳
void USightlineGameInstance::EnsureWidgetClasses()
{
    if (!LoginWidgetClass)
        LoginWidgetClass = LoadClass<ULoginWidgetBase>(nullptr, TEXT("/Game/Code/UI/WBP_Login.WBP_Login_C"));
    if (!LobbyWidgetClass)
        LobbyWidgetClass = LoadClass<ULobbyWidgetBase>(nullptr, TEXT("/Game/Code/UI/WBP_Lobby.WBP_Lobby_C"));
    if (!RoomWidgetClass)
        RoomWidgetClass = LoadClass<URoomWidgetBase>(nullptr, TEXT("/Game/Code/UI/WBP_Room.WBP_Room_C"));
}

void USightlineGameInstance::EnsureUIMouseCursor()
{
    // 纯界面流程（登录/大厅/房间）需要可见鼠标；PIE 默认隐藏
    if (UWorld* World = GetWorld())
        if (APlayerController* PC = World->GetFirstPlayerController())
            PC->bShowMouseCursor = true;
}

void USightlineGameInstance::ShowLoginScreen()
{
    if (bShuttingDown) return;                            // 关服拆卸中不建界面（否则 ensure 崩）
    EnsureWidgetClasses();
    EnsureUIMouseCursor();
    if (CurrentScreenWidget)
    {
        CurrentScreenWidget->RemoveFromParent();
        CurrentScreenWidget = nullptr;
    }
    if (!LoginWidgetClass)
    {
        UE_LOG(LogSightline, Warning, TEXT("[UI] LoginWidgetClass 未配置（工程设置 GameInstance 默认值）"));
        return;
    }
    if (auto* W = CreateWidget<ULoginWidgetBase>(this, LoginWidgetClass))
    {
        if (LobbySession) W->Init(LobbySession);
        W->AddToViewport();
        CurrentScreenWidget = W;
    }
}

void USightlineGameInstance::ShowLobbyScreen()
{
    if (bShuttingDown) return;
    EnsureWidgetClasses();
    EnsureUIMouseCursor();
    if (auto* Cur = Cast<ULobbyWidgetBase>(CurrentScreenWidget.Get()))
    {
        Cur->RefreshFromModel();                          // 已在大厅：幂等刷新（不重建，保滚动位置）
        return;
    }
    if (CurrentScreenWidget)
    {
        CurrentScreenWidget->RemoveFromParent();
        CurrentScreenWidget = nullptr;
    }
    if (!LobbyWidgetClass)
    {
        UE_LOG(LogSightline, Warning, TEXT("[UI] LobbyWidgetClass 未配置"));
        return;
    }
    if (auto* W = CreateWidget<ULobbyWidgetBase>(this, LobbyWidgetClass))
    {
        if (LobbySession) W->Init(LobbySession);
        W->AddToViewport();
        CurrentScreenWidget = W;
    }
}

void USightlineGameInstance::ShowRoomScreen()
{
    if (bShuttingDown) return;
    EnsureWidgetClasses();
    EnsureUIMouseCursor();
    if (auto* Cur = Cast<URoomWidgetBase>(CurrentScreenWidget.Get()))
    {
        return;                                           // 已在房间界面：控件自己随委托刷新
    }
    if (CurrentScreenWidget)
    {
        CurrentScreenWidget->RemoveFromParent();
        CurrentScreenWidget = nullptr;
    }
    if (!RoomWidgetClass)
    {
        UE_LOG(LogSightline, Warning, TEXT("[UI] RoomWidgetClass 未配置"));
        return;
    }
    if (auto* W = CreateWidget<URoomWidgetBase>(this, RoomWidgetClass))
    {
        if (LobbySession) W->Init(LobbySession);
        W->AddToViewport();
        CurrentScreenWidget = W;
    }
}

void USightlineGameInstance::HandleLoginResultForUI(bool bSuccess, int32 PlayerId, const FString& Message)
{
    if (bSuccess) ShowLobbyScreen();                      // 登录成功 → 大厅（失败留在登录界面，控件自显错误）
}

void USightlineGameInstance::HandleRoomListForUI(const TArray<FRoomInfo>& Rooms)
{
    // 收到列表 = 此刻人在大厅（房内玩家不订阅列表）→ 幂等切大厅
    if (LobbySession && LobbySession->Model.bLoggedIn && LobbySession->Model.CurrentRoomId == 0)
    {
        ShowLobbyScreen();
    }
}

void USightlineGameInstance::HandleRoomStateForUI(int32 RoomId, const FString& HostName, const TArray<FString>& Members)
{
    ShowRoomScreen();                                     // 房内状态推送 = 界面切到房间
}

void USightlineGameInstance::HandleKickedForUI(int32 ReasonCode)
{
    ShowLoginScreen();                                    // 被踢/关服 → 回登录界面
}

void USightlineGameInstance::HandleLinkLostForUI()
{
    ShowLoginScreen();                                    // 断线 → 回登录界面
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
    // 第 2 期：4~13 号（登录+房间）转发给会话层；1/3（WELCOME/ACK）留在链路层
    if (LobbySession && MsgId >= SightlineProtocol::MSG_C2S_LOGIN && MsgId <= SightlineProtocol::MSG_S2C_KICKED)
    {
        LobbySession->OnFrame(MsgId, Payload);
        return;
    }

    // 只在游戏线程执行（AsyncTask 保证）——可以安全碰 UE 对象
    switch (MsgId)
    {
    case MSG_S2C_WELCOME:
        bConnected = true;
        bGotWelcome = true;
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
            if (!bGotFirstAck)
            {
                bGotFirstAck = true;
                UE_LOG(LogSightline, Log, TEXT("[GI] 首个 ACK 收到（seq=%u，ping=%ums）—— 下行链路确认"), Seq, LastPingMs);
            }
            UE_LOG(LogSightline, Verbose, TEXT("[GI] ACK seq=%u ping=%ums"), Seq, LastPingMs);
        }
        else
        {
            UE_LOG(LogSightline, Verbose, TEXT("[GI] ACK seq=%u（过期/未知，忽略）"), Seq);
        }
        if (bLag)
        {
            bLag = false;
            LagEnteredMonotonicMs = 0.0;
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
