// USightlineGameInstance —— 应用层：连接状态机 · 心跳 · ping · HUD（C++ 为主，蓝图零逻辑）
// 生命周期锚点：GameInstance 切关卡不死——连接跟随它走，游戏关了连接才许断
#pragma once
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Connection/SightlineConnection.h"
#include "Lobby/SightlineLobby.h"
#include "UI/SightlineUIWidgets.h"
#include "TimerManager.h"
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

private:
    void StartHeartbeat();
    void StopHeartbeat();
    void HandleHeartbeatTick();
    bool HandleTick(float DeltaTime);
    void ScheduleReconnect();
    void OnConnectionLost(const FString& Reason);
    FTimerManager* GetTimerManagerSafe();
    void OnFrameReceived(uint16 MsgId, const TArray<uint8>& Payload);

    TUniquePtr<FSightlineConnection> Connection;
    FTimerHandle HeartbeatTimerHandle;

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

public:
    // —— 界面装配（混合模式：WBP 蓝图放皮，逻辑在 UI/SightlineUIWidgets）——
    // 三个 WBP 资产的根类分别设为这里指定的 C++ 基类；默认值落在 DefaultGame.ini
    UPROPERTY(EditDefaultsOnly, Config, Category = "Sightline UI")
    TSubclassOf<ULoginWidgetBase> LoginWidgetClass;
    UPROPERTY(EditDefaultsOnly, Config, Category = "Sightline UI")
    TSubclassOf<ULobbyWidgetBase> LobbyWidgetClass;
    UPROPERTY(EditDefaultsOnly, Category = "Sightline UI")
    TSubclassOf<URoomWidgetBase> RoomWidgetClass;

private:
    void ShowLoginScreen();
    void ShowLobbyScreen();
    void ShowRoomScreen();
    void EnsureWidgetClasses();          // 类槽兜底：ini 被回写/加载时序不齐时按固定路径 LoadClass
    void EnsureUIMouseCursor();
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
    UPROPERTY()
    TObjectPtr<UUserWidget> CurrentScreenWidget = nullptr;
    bool bInitialScreenShown = false;

    FTickerDelegate TickerDelegate;          // UGameInstance 无 Tick 虚函数——FTSTicker 每帧回调替代
    FTSTicker::FDelegateHandle TickerHandle;
    bool bShuttingDown = false;              // Shutdown 期间禁止自动重连
};
