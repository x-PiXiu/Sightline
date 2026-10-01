// USightlineGameInstance —— 应用层：连接状态机 · 心跳 · ping · HUD（C++ 为主，蓝图零逻辑）
// 生命周期锚点：GameInstance 切关卡不死——连接跟随它走，游戏关了连接才许断
#pragma once
#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Connection/SightlineConnection.h"
#include "TimerManager.h"
#include "Containers/Ticker.h"
#include "SightlineGameInstance.generated.h"

UCLASS()
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
    bool bLag = false;
    bool bGotWelcome = false;     // 诊断：WELCOME 是否已到（LAG 触发时打印）
    bool bGotFirstAck = false;    // 诊断：首个 ACK 是否已到

    // seq → 单调发送时刻（ping = ack 时刻 − 发送时刻）
    TMap<uint32, double> PendingHeartbeats;

    FTickerDelegate TickerDelegate;          // UGameInstance 无 Tick 虚函数——FTSTicker 每帧回调替代
    FTSTicker::FDelegateHandle TickerHandle;
    bool bShuttingDown = false;              // Shutdown 期间禁止自动重连
};
