// SightlineMatchSession —— 第 3 期客户端对局会话（用例层，与 ULobbySession 同构同层）
// 整洁架构位置：UMG/PawnSpawner（外）→ 本类（用例）→ ISendPort（发送端口，D3-18）
// 职责边界：意图接口只发包（StartMatch）；收帧解析 15/17/18 → 更新 Registry（纯态+委托，D3-16）；
//          17 的实体快照解析 M4 直发同步到位（本段已预留路由）
// 客户端镜像 D3-15：MatchSession 持有对局意图（StartMatch），与 LobbySession 的登录/房间意图并列
#pragma once
#include "CoreMinimal.h"
#include "Match/SightlineEntityRegistry.h"
#include "SightlineMatchSession.generated.h"

class ISendPort;

/** 本地状态样本（Character 30Hz 产出——纯结构入参，组帧在会话层，D3-17） */
USTRUCT(BlueprintType)
struct FSightlineLocalState {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FVector Pos = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) float Yaw = 0.f;
    UPROPERTY(BlueprintReadOnly) float Pitch = 0.f;
    UPROPERTY(BlueprintReadOnly) bool bFalling = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMatchStarted);        // 15 收到 → GI 切对局关卡
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchEntityLeft, int32, PlayerId);
// M3b 和解动作过界（纯数据，D3-17 同款）：Error=对账误差向量；bFullSnap=true 时 Error 即绝对权威位（断档全量回滚）
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnSelfCorrected, FVector, Error, bool, bFullSnap);

UCLASS()
class SIGHTLINE_API USightlineMatchSession : public UObject {
    GENERATED_BODY()

public:
    /** GameInstance 在新建连接 / 断开时调用（非拥有指针，同 LobbySession 模式） */
    void BindConnection(ISendPort* InConn);
    void SetRegistry(USightlineEntityRegistry* InRegistry);
    void SetSelfPlayerId(int32 InPid) { SelfPlayerId = InPid; }   // 登录回执到位后由 GI 注入
    bool IsInMatch() const { return bInMatch; }                  // 对局激活查询（GI 面板显隐/生命周期判定）
    /** 链路断开：Registry 清场（对局视角 = 会话结束） */
    void NotifyLinkLost();

    // —— 意图接口（只发包；v3 契约 14：空 payload）——
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void StartMatch();

    /** 16 上报（Character 30Hz 调用——纯结构入参，字节布局在此组装，D3-17） */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void ReportLocalState(const FSightlineLocalState& S);

    // —— 收帧入口（GameInstance.OnFrameReceived 路由 15/17/18）——
    void HandleFrame(uint16 MsgId, const TArray<uint8>& Payload);

    UPROPERTY(BlueprintAssignable) FOnMatchStarted OnMatchStarted;
    UPROPERTY(BlueprintAssignable) FOnMatchEntityLeft OnEntityLeft;
    UPROPERTY(BlueprintAssignable) FOnSelfCorrected OnSelfCorrected;   // M3b：本地 Pawn 订阅（和解落地）

    // —— M3c Debug HUD 观测面（GI 每帧取用；AddOnScreenDebugMessage 扩展，D4-08）——
    int32  GetReconcileCount() const { return ReconcileCount; }   // 和解次数（≥ε 触发）
    int32  GetReplayFrames() const { return ReplayFrames; }       // 最近一次回滚重放的输入帧数
    double GetErrCurrent() const { return ErrCurrentCm; }         // 当前预测误差（cm）
    double GetErrPeak() const { return ErrPeakCm; }               // 峰值误差（cm）
    uint32 GetLastAckedSeq() const { return LastAckedSeq; }       // 服务器已确认序号（与本地 InputSeq 的差=管线深度）
    uint32 GetAckGap() const { return InputSeq - LastAckedSeq; }  // 管线深度（已发-已认；uint32 差值回绕安全）

private:
    /** 一次输入的本地存根（环形缓冲行；(seq,pos,yaw)×1s——设计草案 §2.3） */
    struct FInputSample {
        uint32 Seq = 0;
        FVector Pos = FVector::ZeroVector;
        float Yaw = 0.f;
    };
    /** 三方对账（M3b）：权威位 vs 环形缓冲同 seq 样本——ε 内无感 / 超 ε 回滚重放 / 断档全量兑底 */
    void ReconcileSelf(uint32 AckedSeq, const FVector& AuthPos);

    ISendPort* Conn = nullptr;                          // 非拥有指针（GI 持有生命周期；D3-18 只认端口）
    UPROPERTY()
    TObjectPtr<USightlineEntityRegistry> Registry;
    int32 SelfPlayerId = 0;                            // 登录回执分配，Registry 以此标 bSelf
    bool bInMatch = false;                             // 对局激活：收到 15 置位 / 自己 18 离场或链路死亡清位——16 上报的唯一合法窗口
    uint32 InputSeq = 0;                               // v4：本地预测序号（19 号；M3 预测/重放的历史锚点）
    TArray<FInputSample> InputHistory;                 // M3b：1s 环形输入史（对账/重放的本地底账）
    uint32 LastAckedSeq = 0;                           // 服务器最近确认的序号（对账基准；HUD 差值用）
    int32 ReconcileCount = 0;                          // 和解次数（≥ε 才计；断档兑底也计）
    int32 ReplayFrames = 0;                            // 最近一次回滚重放的输入帧数（断档兑底=0）
    double ErrCurrentCm = 0.0;                         // 当前对账误差（cm；ε 判定的那条）
    double ErrPeakCm = 0.0;                            // 峰值误差（cm）
};
