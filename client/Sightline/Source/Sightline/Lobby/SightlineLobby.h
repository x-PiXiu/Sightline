// SightlineLobby —— 第 2 期客户端会话层（用例 + 实体）
// 整洁架构位置：UMG（外）→ ULobbySession（本文件）→ ISendPort（发送端口，D3-18——FSightlineConnection 实现）
// 职责边界：意图接口只发包（客户端只发建议）；一切房间状态以服务器推送为准（权威原则）
// 线程约定：全部接口只在游戏线程调用（帧由 AsyncTask 门投递回来）
#pragma once
#include "CoreMinimal.h"
#include "SightlineLobby.generated.h"

class ISendPort;

/** 大厅列表项（服务器 ROOM_LIST 快照的一行） */
USTRUCT(BlueprintType)
struct FRoomInfo {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int32 RoomId = 0;
    UPROPERTY(BlueprintReadOnly) FString RoomName;
    UPROPERTY(BlueprintReadOnly) int32 MemberCount = 0;
    UPROPERTY(BlueprintReadOnly) FString HostName;      // 剩余成员中最早加入者
};

/** 会话模型：纯数据。UI 只读它 + 订阅下面的委托，不直接碰连接 */
USTRUCT(BlueprintType)
struct FLobbyModel {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) bool bLoggedIn = false;
    UPROPERTY(BlueprintReadOnly) int32 PlayerId = 0;
    UPROPERTY(BlueprintReadOnly) FString PlayerName;
    UPROPERTY(BlueprintReadOnly) TArray<FRoomInfo> Rooms;        // 大厅列表（服务器全量快照）
    UPROPERTY(BlueprintReadOnly) int32 CurrentRoomId = 0;        // 0 = 未进房
    UPROPERTY(BlueprintReadOnly) FString CurrentRoomName;
    UPROPERTY(BlueprintReadOnly) TArray<FString> CurrentMembers; // 按加入顺序，首位 = 房主
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnLoginResult, bool, bSuccess, int32, PlayerId, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRoomListChanged, const TArray<FRoomInfo>&, Rooms);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnRoomStateChanged, int32, RoomId, const FString&, HostName, const TArray<FString>&, Members);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnKicked, int32, ReasonCode);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnLobbyLinkLost);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnActionFailed, const FString&, Reason);   // 意图/回执失败的界面反馈

/**
 * 大厅会话：意图接口（C2S）+ 收帧解析（S2C）+ 模型与委托
 * KICKED 原因码：1 顶号 / 2 登录超时 / 3 服务器关闭
 */
UCLASS()
class SIGHTLINE_API ULobbySession : public UObject {
    GENERATED_BODY()

public:
    /** GameInstance 在新建连接 / 断开时调用（可传 nullptr；本类不拥有连接） */
    void BindConnection(ISendPort* InConn);
    /** 链路断开（GI 的 OnConnectionLost 路径）：会话清空并广播 */
    void NotifyLinkLost();

    // —— 意图接口（客户端只发建议）——
    UFUNCTION(BlueprintCallable, Category = "Sightline Lobby")
    void Login(const FString& PlayerName);
    UFUNCTION(BlueprintCallable, Category = "Sightline Lobby")
    void CreateRoom(const FString& RoomName);
    UFUNCTION(BlueprintCallable, Category = "Sightline Lobby")
    void JoinRoom(int32 RoomId);
    UFUNCTION(BlueprintCallable, Category = "Sightline Lobby")
    void LeaveRoom();

    // —— 收帧入口（GameInstance.OnFrameReceived 转发 4~13 号消息）——
    void OnFrame(uint16 MsgId, const TArray<uint8>& Payload);

    UPROPERTY(BlueprintReadOnly) FLobbyModel Model;

    UPROPERTY(BlueprintAssignable) FOnLoginResult OnLoginResult;
    UPROPERTY(BlueprintAssignable) FOnRoomListChanged OnRoomListChanged;
    UPROPERTY(BlueprintAssignable) FOnRoomStateChanged OnRoomStateChanged;
    UPROPERTY(BlueprintAssignable) FOnKicked OnKicked;
    UPROPERTY(BlueprintAssignable) FOnLobbyLinkLost OnLobbyLinkLost;
    UPROPERTY(BlueprintAssignable) FOnActionFailed OnActionFailed;   // 建房/进房/离开失败 + 客户端预检失败

private:
    void SendIntent(uint16 MsgId, const TArray<uint8>& Payload = TArray<uint8>());
    void ResetModel();                               // 断线/被踢：会话清空
    bool SendLenPrefixedName(uint16 MsgId, const FString& Name);  // 登录/建房的 [1B len][utf8]

    ISendPort* Conn = nullptr;                       // 非拥有指针（GI 持有生命周期；D3-18 只认端口）
};
