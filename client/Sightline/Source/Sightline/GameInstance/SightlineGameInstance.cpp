// USightlineGameInstance 实现——对应心跳时序图 UE 侧（④⑨ 验收画面的产生地）
// 日志规范（LogSightline 分类）：
//   Log     = 关键事件（连接成功/断开/WELCOME/重连/LAG 恢复）
//   Warning = 可恢复异常（连接失败/LAG 进入/写失败）
//   Error   = 不可恢复
//   Verbose = 心跳与收发明细（默认不显示，调试时在日志窗口开 Verbose）
#include "GameInstance/SightlineGameInstance.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Game/SightlineGameMode.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
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

    // 第 3 期：对局会话（Registry 纯态 + MatchSession 用例 + Spawner 效果适配器——D3-16 三层）
    EntityRegistry = NewObject<USightlineEntityRegistry>(this);
    MatchSession = NewObject<USightlineMatchSession>(this);
    MatchSession->SetRegistry(EntityRegistry);
    PawnSpawner = NewObject<USightlinePawnSpawner>(this);
    PawnSpawner->Init(EntityRegistry, nullptr);           // 远端类槽在生成时从 GameMode 现取
    PawnSpawner->BindDelegates();
    MatchSession->OnMatchStarted.AddDynamic(this, &USightlineGameInstance::HandleMatchStarted);

    // UGameInstance 无 Tick 虚函数——FTSTicker 每帧回调（HUD 刷新 + LAG 判定）
    TickerDelegate.BindUObject(this, &USightlineGameInstance::HandleTick);
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TickerDelegate, 0.f);

    // M5 损伤滑块面板改由 HandleTick 对账（对局中挂载/离场隐藏——Init 期视口未就绪，M6 实锤）

    UE_LOG(LogSightline, Log, TEXT("[GI] 初始化完成，发起连接 127.0.0.1:18888"));
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

    // 调试面板对账：对局激活才显示（用户定：进游戏场景后显示），离场隐藏
    SyncDebugPanel();

    // HUD（验收画面 ①②）：覆盖式刷新同一 key
    if (GEngine)
    {
        FString Status = bConnected ? TEXT("Connected") : TEXT("Disconnected");
        if (bConnected && bLag) Status += TEXT("  LAG");
        const FColor Color = bConnected ? (bLag ? FColor::Orange : FColor::Green) : FColor::Red;
        GEngine->AddOnScreenDebugMessage(kHudMessageKey, 0.f, Color,
            FString::Printf(TEXT("%s   ping=%ums   seq=%u"), *Status, LastPingMs, HeartbeatSeq));
    }

    // 第 3 期：切对局关卡后延迟批量生成远端角色（0.6s 守门 = 等 OpenLevel 加载落定，M3 简化；
    //         精确的就绪钩子 = 演进项）——本地 Pawn 由 GameMode 生成，这里只补远端
    if (bPendingArenaSpawn && FPlatformTime::Seconds() >= PendingArenaSpawnAt)
    {
        bPendingArenaSpawn = false;
        if (UWorld* W = GetWorld())
        {
            TSubclassOf<APawn> RemoteClass = nullptr;
            if (const ASightlineGameMode* GM = Cast<ASightlineGameMode>(W->GetAuthGameMode()))
                RemoteClass = GM->QuinnClass;
            if (PawnSpawner) PawnSpawner->SpawnAllRemote(W, RemoteClass);
            UE_LOG(LogSightline, Log, TEXT("[GI] 对局关卡就绪 → 远端角色批量生成"));
        }
    }

    // 自动重连（到点即连）——同心跳，时间戳轮询（跨切图存活）
    if (bReconnectPending && !bShuttingDown && !bConnected
        && MonotonicMs() >= ReconnectAtMonotonicMs)
    {
        bReconnectPending = false;
        Connect();
    }

    // 心跳（1s 节拍）——由本 FTSTicker 按节拍驱动（进程级，跨切图存活）；
    // World 级 TimerManager 上的定时器会随 MATCH_START 切图死亡（M6 联调实锤）
    if (bConnected && !bShuttingDown && LastHeartbeatMonotonicMs > 0.0
        && MonotonicMs() - LastHeartbeatMonotonicMs >= kHeartbeatPeriod * 1000.0f)
    {
        LastHeartbeatMonotonicMs = MonotonicMs();
        HandleHeartbeatTick();
    }

    // LAG = "3s 无任何下行帧"（链路静默），不是"无 ACK"——见 OnFrameReceived 入口注释
    if (bConnected && !bLag && MonotonicMs() - LastAckMonotonicMs > kLagThresholdMs)
    {
        bLag = true;
        LagEnteredMonotonicMs = MonotonicMs();
        UE_LOG(LogSightline, Warning, TEXT("[GI] %.0f ms 无下行帧 → LAG（未确认心跳 %d 个，已收 WELCOME=%d）"),
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

    UE_LOG(LogSightline, Log, TEXT("[GI] 连接 127.0.0.1:18888 ……"));
    Connection = MakeUnique<FSightlineConnection>();
    if (LobbySession) LobbySession->BindConnection(Connection.Get());   // 会话绑定新连接
    if (MatchSession) MatchSession->BindConnection(Connection.Get());   // 对局会话绑定新连接（M3）
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

    if (Connection->Connect(TEXT("127.0.0.1"), 18888))
    {
        UE_LOG(LogSightline, Log, TEXT("[GI] 连接请求已发起（等待 WELCOME 确认）"));
    }
    else
    {
        // 服务器可能后起——3 秒后自动重试
        Connection.Reset();
        if (LobbySession) LobbySession->BindConnection(nullptr);
        if (MatchSession) MatchSession->BindConnection(nullptr);
        UE_LOG(LogSightline, Warning, TEXT("[GI] 连接失败，%.0f 秒后重试"), kReconnectDelay);
        ScheduleReconnect();
    }
}

void USightlineGameInstance::ScheduleReconnect()
{
    if (bShuttingDown) return;   // Shutdown 期间：不调度、不打印重连日志
    // 时间戳轮询承载（HandleTick 驱动，跨切图存活）——World 级 TimerManager 上的定时器
    // 会随 MATCH_START 切图死亡（M6 联调实锤，与心跳同款）；局部 FTimerHandle 还无法取消
    bReconnectPending = true;
    ReconnectAtMonotonicMs = MonotonicMs() + kReconnectDelay * 1000.0;
    UE_LOG(LogSightline, Log, TEXT("[GI] %.0f 秒后自动重连"), kReconnectDelay);
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
    if (MatchSession)
    {
        MatchSession->NotifyLinkLost();                   // Registry 清场（对局随链路死亡）
        MatchSession->BindConnection(nullptr);
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
    if (MatchSession) MatchSession->BindConnection(nullptr);
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
    if (!DebugPanelClass)
        DebugPanelClass = LoadClass<UDebugPanelBase>(nullptr, TEXT("/Game/Code/UI/WBP_DebugPanel.WBP_DebugPanel_C"));
}

void USightlineGameInstance::EnsureUIMouseCursor()
{
    // 纯界面流程（登录/大厅/房间）需要可见鼠标；PIE 默认隐藏
    if (UWorld* World = GetWorld())
        if (APlayerController* PC = World->GetFirstPlayerController())
            PC->bShowMouseCursor = true;
}

void USightlineGameInstance::SetInboundDelayMs(int32 Ms)
{
    if (Connection) Connection->SetInboundDelayMs(Ms);    // 延迟队列本体在连接适配器（D3-11）
    if (CSightlineStatLog.GetValueOnGameThread() != 0)
        UE_LOG(LogSightline, Log, TEXT("[STAT] DELAY t=%.3f ms=%d"), FPlatformTime::Seconds(), Ms);
}

void USightlineGameInstance::SyncDebugPanel()
{
#if !UE_BUILD_SHIPPING
    // 面板是对局工具（损伤滑块服务同步演示）：进游戏场景才显示，登录/大厅期不打扰（用户 2026-10-18 定）。
    // bPendingArenaSpawn 守门：15 刚到但切图未落定时不上屏（避免在旧世界闪一下又被切图销毁）
    const bool bWant = !bShuttingDown && MatchSession && MatchSession->IsInMatch() && !bPendingArenaSpawn;
    if (!bWant)
    {
        if (DebugPanelWidget.IsValid())
        {
            DebugPanelWidget->RemoveFromParent();
            DebugPanelWidget.Reset();
            UE_LOG(LogSightline, Log, TEXT("[UI] 调试面板已隐藏（离场）"));
        }
        return;
    }
    if (DebugPanelWidget.IsValid() && DebugPanelWidget->IsInViewport()) return;   // 已在屏
    // 视口未就绪（Init/切图瞬间）下帧再试；切图销毁了旧 widget → 弱指针失效 → 新世界自动重挂
    const UWorld* World = GetWorld();
    if (!World || !World->GetGameViewport()) return;
    EnsureWidgetClasses();
    if (DebugPanelClass == nullptr)
    {
        static bool bWarned = false;                 // 缺类警告只打一次（每帧对账不许刷屏）
        if (!bWarned) { bWarned = true; UE_LOG(LogSightline, Log, TEXT("[UI] WBP_DebugPanel 未找到——建好后进对局自动挂载")); }
        return;
    }
    if (auto* W = CreateWidget<UDebugPanelBase>(this, DebugPanelClass))
    {
        W->Init(this);
        W->AddToViewport(50);
        DebugPanelWidget = W;
        // 挂载结果如实报告（AddToViewport 失败不上报，IsInViewport 是唯一真值）
        UE_LOG(LogSightline, Log, TEXT("[UI] 调试面板挂载 %s（对局中显示）"),
               W->IsInViewport() ? TEXT("成功") : TEXT("失败"));
    }
#endif
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
        if (MatchSession) W->InitMatch(MatchSession);     // M3：开始对局意图的出口注入（漏接 = 按钮点击空转）
        W->AddToViewport();
        CurrentScreenWidget = W;
    }
}

void USightlineGameInstance::HandleLoginResultForUI(bool bSuccess, int32 PlayerId, const FString& Message)
{
    if (!bSuccess) return;                                // 失败留在登录界面（控件自显错误）
    if (MatchSession) MatchSession->SetSelfPlayerId(PlayerId);   // Registry 标 bSelf 用（名册自识别）
    ShowLobbyScreen();                                    // 登录成功 → 大厅
}

// ===== 第 3 期：对局（MATCH_START → 切对局关卡 → 延迟生成远端角色）=====

void USightlineGameInstance::HandleMatchStarted()
{
    UE_LOG(LogSightline, Log, TEXT("[GI] MATCH_START → 切对局关卡 NewMap"));
    bInArena = true;
    bPendingArenaSpawn = true;
    PendingArenaSpawnAt = FPlatformTime::Seconds() + 0.6;   // 等 OpenLevel 加载落定（M3 简化守门）
    if (APlayerController* PC = GetFirstLocalPlayerController())
        PC->ClientTravel(TEXT("/Game/Code/Map/NewMap"), ETravelType::TRAVEL_Absolute);
}

void USightlineGameInstance::HandleRoomListForUI(const TArray<FRoomInfo>& Rooms)
{
    // 收到列表 = 此刻人在大厅（房内玩家不订阅列表）→ 幂等切大厅
    if (LobbySession && LobbySession->Model.bLoggedIn && LobbySession->Model.CurrentRoomId == 0)
    {
        if (bInArena)                                     // 对局中离开房间 → 切回大厅世界并清场
        {
            bInArena = false;
            if (PawnSpawner) PawnSpawner->DespawnAll();
            if (EntityRegistry) EntityRegistry->Clear();
            if (APlayerController* PC = GetFirstLocalPlayerController())
                PC->ClientTravel(TEXT("/Engine/Maps/Templates/OpenWorld"), ETravelType::TRAVEL_Absolute);
            UE_LOG(LogSightline, Log, TEXT("[GI] 离开对局 → 切回大厅世界"));
        }
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


void USightlineGameInstance::StartHeartbeat()
{
    // 心跳承载于 FTSTicker（进程级，跨切图存活）——World 级 TimerManager 上的定时器
    // 会随 MATCH_START 切图死亡（M6 联调实锤），故改由 HandleTick 按节拍驱动
    LastHeartbeatMonotonicMs = MonotonicMs();
    UE_LOG(LogSightline, Log, TEXT("[GI] 心跳启动（每 %.0f 秒）"), kHeartbeatPeriod);
}

void USightlineGameInstance::StopHeartbeat()
{
    LastHeartbeatMonotonicMs = 0.0;          // 0 = 心跳暂停
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
    // 链路活性判定源：任何下行帧都算（ACK 只负责 ping）——对局期 17 快照 30Hz 在流时
    // 链路明明活着，不能因心跳 ACK 迟到而误判 LAG（PIE 双开卡顿误报，M6 联调实锤）
    LastAckMonotonicMs = MonotonicMs();
    if (bLag)
    {
        bLag = false;
        LagEnteredMonotonicMs = 0.0;
        UE_LOG(LogSightline, Log, TEXT("[GI] 下行帧恢复（msgid=%u）→ LAG 解除"), MsgId);
    }

    // 第 2 期：4~13 号（登录+房间）转发给会话层；1/3（WELCOME/ACK）留在链路层
    if (LobbySession && MsgId >= SightlineProtocol::MSG_C2S_LOGIN && MsgId <= SightlineProtocol::MSG_S2C_KICKED)
    {
        LobbySession->OnFrame(MsgId, Payload);
        return;
    }

    // 第 3 期：15/17/18（对局）转发给对局会话（17 的快照解析 M4 实装，先统一入口喂 Registry）
    if (MatchSession &&
        (MsgId == MSG_S2C_MATCH_START || MsgId == MSG_S2C_ENTITY_STATE || MsgId == MSG_S2C_ENTITY_LEAVE))
    {
        MatchSession->HandleFrame(MsgId, Payload);
        return;
    }

    // 只在游戏线程执行（AsyncTask 保证）——可以安全碰 UE 对象
    switch (MsgId)
    {
    case MSG_S2C_WELCOME:
        bConnected = true;
        bGotWelcome = true;
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
        break;
    }
    default:
        UE_LOG(LogSightline, Verbose, TEXT("[GI] 未知消息 msgid=%u（忽略）"), MsgId);
        break;
    }
}
