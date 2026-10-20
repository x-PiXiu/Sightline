// USightlineGameInstance —— 应用层：连接状态机 · 心跳 · ping · HUD（C++ 为主，蓝图零逻辑）
// 生命周期锚点：GameInstance 切关卡不死——连接跟随它走，游戏关了连接才许断
#pragma once
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Connection/SightlineConnection.h"
#include "Lobby/SightlineLobby.h"
#include "Match/SightlineEntityRegistry.h"
#include "Match/SightlineMatchSession.h"
#include "Match/SightlinePawnSpawner.h"
#include "UI/SightlineUIWidgets.h"
#include "Containers/Ticker.h"
#include "SightlineGameInstance.generated.h"

UCLASS(Config = Game)   // 三个 Widget 槽的默认值存取 DefaultGame.ini（ini 引用 WBP 的机制基础）
class SIGHTLINE_API USightlineGameInstance : public UGameInstance
{
    GENERATED_BODY()

public:
    virtual void Init() override;
    virtual void Shutdown() override;

    void Connect();
    void Disconnect();

    bool IsConnected() const { return bConnected; }
    uint32 GetLastPingMs() const { return LastPingMs; }
    USightlineMatchSession* GetMatchSession() const { return MatchSession; }   // 角色采样上报绑定用（D3-17）
    USightlineEntityRegistry* GetEntityRegistry() const { return EntityRegistry; }  // GameMode 查自机名册序（P1 出生点）
    void SetInboundDelayMs(int32 Ms);                // M5：损伤滑块透传（连接适配器的延迟队列）

private:
    void StartHeartbeat();
    void StopHeartbeat();
    void HandleHeartbeatTick();
    bool HandleTick(float DeltaTime);
    void ScheduleReconnect();
    void OnConnectionLost(const FString& Reason);
    void OnFrameReceived(uint16 MsgId, const TArray<uint8>& Payload);

    TUniquePtr<FSightlineConnection> Connection;
    double LastHeartbeatMonotonicMs = 0.0;   // 心跳节拍（FTSTicker 承载——World 级 TimerManager 会随切图死亡，M6 联调实锤）
    bool   bReconnectPending = false;        // 重连调度（时间戳轮询——World 级 TimerManager 会随切图死亡，M6 联调实锤）
    double ReconnectAtMonotonicMs = 0.0;     // 到点即连；置回 false 即取消（比局部 FTimerHandle 多出的能力）

    // 连接状态
    bool bConnected = false;
    uint32 HeartbeatSeq = 0;
    uint32 LastPingMs = 0;
    double LastAckMonotonicMs = 0.0;      // 单调时钟：LAG 判定用
    double LagEnteredMonotonicMs = 0.0;   // 单调时钟：LAG 进入时刻（持续超时 → 主动断线重连）
    bool bLag = false;
    bool bGotWelcome = false;     // 诊断：WELCOME 是否已到（LAG 触发时打印）
    bool bGotFirstAck = false;    // 诊断：首个 ACK 是否已到

    // seq → 单调发送时刻（ping = ack 时刻 − 发送时刻）
    TMap<uint32, double> PendingHeartbeats;

    // 第 2 期：大厅会话（GI 持有生命周期，帧 4~13 转发给它）
    UPROPERTY()
    TObjectPtr<ULobbySession> LobbySession;

    // 第 3 期：对局会话（帧 15/17/18 转发；Registry 纯态 + Spawner 效果适配器——D3-16）
    UPROPERTY()
    TObjectPtr<USightlineEntityRegistry> EntityRegistry;
    UPROPERTY()
    TObjectPtr<USightlineMatchSession> MatchSession;
    UPROPERTY()
    TObjectPtr<USightlinePawnSpawner> PawnSpawner;
    bool bInArena = false;                    // 当前在对局关卡（离开房间 → 切回大厅世界）
    bool bPendingArenaSpawn = false;          // 切图后批量生成远端角色（0.6s 守门，M3 简化）
    double PendingArenaSpawnAt = 0.0;
    int32 LastInboundDelayMs = 0;             // M3c：滑块值留底（和解 HUD 显示用；透传连接的同时自存一份）

public:
    // —— 界面装配（混合模式：WBP 蓝图放皮，逻辑在 UI/SightlineUIWidgets）——
    // 三个 WBP 资产的根类分别设为这里指定的 C++ 基类；默认值落在 DefaultGame.ini
    UPROPERTY(EditDefaultsOnly, Config, Category = "Sightline UI")
    TSubclassOf<ULoginWidgetBase> LoginWidgetClass;
    UPROPERTY(EditDefaultsOnly, Config, Category = "Sightline UI")
    TSubclassOf<ULobbyWidgetBase> LobbyWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category = "Sightline UI")
    TSubclassOf<URoomWidgetBase> RoomWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category = "Sightline UI")
    TSubclassOf<UDebugPanelBase> DebugPanelClass;    // M5：损伤滑块面板（WBP_DebugPanel）
    TWeakObjectPtr<UDebugPanelBase> DebugPanelWidget; // 面板实例（对局中显示，离场隐藏——用户定）

private:
    void ShowLoginScreen();
    void ShowLobbyScreen();
    void ShowRoomScreen();
    void EnsureWidgetClasses();          // 类槽兜底：ini 被回写/加载时序不齐时按固定路径 LoadClass
    void EnsureUIMouseCursor();
    void SyncDebugPanel();               // 面板对账：对局激活挂载 / 离场隐藏（每帧幂等）
    UFUNCTION()
    void HandleLoginResultForUI(bool bSuccess, int32 PlayerId, const FString& Message);
    UFUNCTION()
    void HandleRoomListForUI(const TArray<FRoomInfo>& Rooms);
    UFUNCTION()
    void HandleRoomStateForUI(int32 RoomId, const FString& HostName, const TArray<FString>& Members);
    UFUNCTION()
    void HandleKickedForUI(int32 ReasonCode);
    UFUNCTION()
    void HandleLinkLostForUI();
    UFUNCTION()
    void HandleMatchStarted();                       // 15 → 切对局关卡 + 延迟生成远端角色
    UPROPERTY()
    TObjectPtr<UUserWidget> CurrentScreenWidget = nullptr;
    bool bInitialScreenShown = false;

    FTickerDelegate TickerDelegate;          // UGameInstance 无 Tick 虚函数——FTSTicker 每帧回调替代
    FTSTicker::FDelegateHandle TickerHandle;
    bool bShuttingDown = false;              // Shutdown 期间禁止自动重连
};
