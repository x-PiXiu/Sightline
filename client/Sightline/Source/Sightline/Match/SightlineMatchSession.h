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

/** ep08 v4.8：30 号运行时参数覆盖层（服务器配置表的唯一客户端副本） */
struct FSightlineWeaponParams {
    int32 Damage = 25;
    int32 Mag = 30;
    int32 ReloadMs = 2500;
    int32 FireIntervalMs = 95;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConfigVersion, const FString&, Version);

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnMatchStarted);        // 15 收到 → GI 切对局关卡
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchEntityLeft, int32, PlayerId);
// M3b 和解动作过界（纯数据，D3-17 同款）：Error=对账误差向量；bFullSnap=true 时 Error 即绝对权威位
// （断档全量回滚）；bInstant=true=传送级分歧（Reconcile.h kInstantSnapCm）→ 施加端瞬发，否则尾款平滑
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnSelfCorrected, FVector, Error, bool, bFullSnap, bool, bInstant);

/** 21 号开火结果（M5 · D5-02v2）——一条消息三方共用的过界纯数据：
 *  ShooterPid=自己 → Hitmarker；VictimPid=自己 → 红闪+血量行；其他 → tracer+远端开火动画 */
USTRUCT(BlueprintType)
struct FSightlineFireResult {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int32 ShooterPid = 0;
    UPROPERTY(BlueprintReadOnly) bool bHit = false;
    UPROPERTY(BlueprintReadOnly) int32 VictimPid = 0;
    UPROPERTY(BlueprintReadOnly) FVector HitPoint = FVector::ZeroVector;   // 命中面/射线终点（tracer 停墙）
    UPROPERTY(BlueprintReadOnly) int32 VictimHp = 0;                       // 命中后余血（HUD 血量行）
    UPROPERTY(BlueprintReadOnly) int32 Gate = 0;                           // 0=正常判定 / 非0=被武器门拒
                                                                           // （2026-11-05 弹匣漂移根治：射手撤销预测扣弹）
    UPROPERTY(BlueprintReadOnly) int32 ReloadRemainingMs = 0;              // v4.8.1：gate=1 时=服务器换弹钟剩余
                                                                           // （客户端据以对齐本地钟·漂移一个回包自愈）
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFireResult, const FSightlineFireResult&, Result);

// ===== ep07 死亡/重生/结算（v4.4）=====
/** 23 号击杀事件（击杀播报+比分源+自己死亡态置位） */
USTRUCT(BlueprintType)
struct FSightlineKill {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int32 VictimPid = 0;
    UPROPERTY(BlueprintReadOnly) int32 KillerPid = 0;
    UPROPERTY(BlueprintReadOnly) int32 RespawnDelayMs = 0;
    UPROPERTY(BlueprintReadOnly) int32 VictimHp = 0;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnKill, const FSightlineKill&, Kill);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnRespawn, int32, Pid, int32, SpawnIndex,
                                               int32, InvulnMs, FVector, SpawnPos);
/** 25 号结算行 */
USTRUCT(BlueprintType)
struct FSightlineScoreRow {
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly) int32 pid = 0;
    UPROPERTY(BlueprintReadOnly) int32 kills = 0;
    UPROPERTY(BlueprintReadOnly) int32 deaths = 0;
};
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnMatchEnd, const TArray<FSightlineScoreRow>&, Rows);

/** 比分板客户端缓存行（2026-11-05 比分板）：23 号事件流本地累加——TCP 可靠有序=事件流等价
 *  状态流（D5-09 血量同款论证）；终局 25 号到时与服务器权威对账（日志） */
struct FSightlineScore {
    int32 Kills = 0;
    int32 Deaths = 0;
};

UCLASS()
class SIGHTLINE_API USightlineMatchSession : public UObject {
    GENERATED_BODY()

public:
    /** GameInstance 在新建连接 / 断开时调用（非拥有指针，同 LobbySession 模式） */
    void BindConnection(ISendPort* InConn);
    void SetRegistry(USightlineEntityRegistry* InRegistry);
    void SetSelfPlayerId(int32 InPid) { SelfPlayerId = InPid; }
    int32 GetSelfPlayerId() const { return SelfPlayerId; }   // ep07：战斗 UI BindSession 用   // 登录回执到位后由 GI 注入
    bool IsInMatch() const { return bInMatch; }                  // 对局激活查询（GI 面板显隐/生命周期判定）
    bool IsSelfPlayer(int32 Pid) const { return Pid == SelfPlayerId; }   // M5：21 号三分流判己用
    /** 链路断开：Registry 清场（对局视角 = 会话结束） */
    void NotifyLinkLost();

    // —— 意图接口（只发包；v3 契约 14：空 payload）——
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void StartMatch();

    /** 16 上报（Character 30Hz 调用——纯结构入参，字节布局在此组装，D3-17） */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void ReportLocalState(const FSightlineLocalState& S);

    // —— M5 开火意图（D5-01：20 号空 payload——方向都不带，判定全用服务器权威状态）——
    /** 开火请求：本地节流/弹匣在 Character 侧（表现层预测），通过后才调这里；服务器四道门兜底 */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendFireRequest();

    // —— 收帧入口（GameInstance.OnFrameReceived 路由 15/17/18/21 + ep07 23/24/25）——
    void HandleFrame(uint16 MsgId, const TArray<uint8>& Payload);
    bool IsSelfDead() const { return bSelfDead; }
    void ClearPredictionInput();                      // ep07：重生落位前清预测底账（❗9）

    // —— ep07 人机席位意图（仅房主；26/27 号——服务器 AddBot/RemoveBot）——
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendBotAdd(int32 Difficulty);                // 难度：0=易 1=中 2=难

    /** 自主换弹（2026-11-05·契约 29 号空 payload）：换弹钟权威在服务器——R 键只发意图 */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendReload();

    /** ep08 v4.8：28 号换枪意图（1B type）；服务器权威状态 + 配置表校验 */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendSwitchWeapon(int32 WeaponType);

    // —— ep08：运行时参数覆盖层（30 号驱动；未收到 30 号时回退编译期默认值）——
    FSightlineWeaponParams WeaponParams(int32 WeaponType) const;
    int32 DamageOf(int32 WeaponType) const { return WeaponParams(WeaponType).Damage; }
    int32 MagOf(int32 WeaponType) const { return WeaponParams(WeaponType).Mag; }
    int32 ReloadMsOf(int32 WeaponType) const { return WeaponParams(WeaponType).ReloadMs; }
    int32 FireIntervalMsOf(int32 WeaponType) const { return WeaponParams(WeaponType).FireIntervalMs; }
    const FString& GetConfigVersion() const { return ConfigVersion; }

    /** 离开房间（契约 10 号·空 payload）：对局结束返回大厅——连接保持，服务器移出房间后广播
     *  ROOM_LIST，GI 既有流程自动"切回大厅世界+显示大厅"（2026-11-05 结算"离开"按钮修） */
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendLeaveRoom();
    UFUNCTION(BlueprintCallable, Category = "Sightline Match")
    void SendBotRemove(int32 BotPid);

    UPROPERTY(BlueprintAssignable) FOnMatchStarted OnMatchStarted;
    UPROPERTY(BlueprintAssignable) FOnMatchEntityLeft OnEntityLeft;
    UPROPERTY(BlueprintAssignable) FOnSelfCorrected OnSelfCorrected;   // M3b：本地 Pawn 订阅（和解落地）
    UPROPERTY(BlueprintAssignable) FOnFireResult OnFireResult;         // M5：21 号三分流（Hitmarker/红闪/tracer/音效/血量行）
    UPROPERTY(BlueprintAssignable) FOnKill OnKill;                     // ep07：23 号（击杀播报+比分源）
    UPROPERTY(BlueprintAssignable) FOnRespawn OnRespawn;               // ep07：24 号（落位+无敌窗）
    UPROPERTY(BlueprintAssignable) FOnMatchEnd OnMatchEnd;             // ep07：25 号（结算面板）
    UPROPERTY(BlueprintAssignable) FOnConfigVersion OnConfigVersion;   // ep08：30 号（HUD 版本行）

    // —— M3c Debug HUD 观测面（GI 每帧取用；AddOnScreenDebugMessage 扩展，D4-08）——
    int32  GetReconcileCount() const { return ReconcileCount; }   // 和解次数（≥ε 触发）
    int32  GetReplayFrames() const { return ReplayFrames; }       // 最近一次回滚重放的输入帧数
    double GetErrCurrent() const { return ErrCurrentCm; }         // 当前预测误差（cm）
    double GetErrPeak() const { return ErrPeakCm; }               // 峰值误差（cm）
    uint32 GetLastAckedSeq() const { return LastAckedSeq; }       // 服务器已确认序号（与本地 InputSeq 的差=管线深度）
    uint32 GetAckGap() const { return InputSeq - LastAckedSeq; }  // 管线深度（已发-已认；uint32 差值回绕安全）

    // —— M5 血量观测面（HUD 血量行；21 号事件驱动，TCP 可靠有序=事件流等价状态流，D5-09）——
    /** 实体当前血量（MatchStart 时全员满血初始化；未知的 pid 返回 DefaultValue） */
    UFUNCTION(BlueprintPure, Category = "Sightline Match")
    int32 GetEntityHp(int32 PlayerId, int32 DefaultValue = 100) const;
    UFUNCTION(BlueprintPure, Category = "Sightline Match")
    TArray<int32> GetEntityIds() const;   // 血量行枚举（血量缓存键序）

    // —— 比分板观测面（2026-11-05：23 号事件流累加·Tab 按住显示）——
    TArray<FSightlineScoreRow> GetScoreboard() const;   // 排序快照（kills 降序→deaths 升→pid 升稳定序）
    FString NameOf(int32 Pid) const;                    // pid→名字（Registry 现查——KillFeed 同款上收公共）

private:
    /** 一次输入的本地存根（环形缓冲行；(seq,pos,yaw)×1s——设计草案 §2.3） */
    struct FInputSample {
        uint32 Seq = 0;
        FVector Pos = FVector::ZeroVector;
        float Yaw = 0.f;
    };
    /** 三方对账（M3b）：权威位 vs 环形缓冲同 seq 样本——ε 内无感 / ≥ ε 回滚重放 / 断档 全量兑底 */
    void ReconcileSelf(uint32 AckedSeq, const FVector& AuthPos);

public:
    /** ep06 幽灵数据源：最近一次服务器权威位（17 号自实体·每快照刷新） */
    UFUNCTION(BlueprintPure, Category = "Sightline Match")
    FVector GetServerAuthPos() const { return LastAuthPos; }
    UFUNCTION(BlueprintPure, Category = "Sightline Match")
    bool HasServerAuthPos() const { return bHasAuthPos; }
    USightlineEntityRegistry* GetRegistry() const { return Registry; }   // ep06：幽灵遍历入口

private:
    FVector LastAuthPos = FVector::ZeroVector;          // ep06：幽灵数据源（最新权威位·随快照刷新）
    bool bHasAuthPos = false;
    ISendPort* Conn = nullptr;                          // 非拥有指针（GI 持有生命周期；D3-18 只认端口）
    UPROPERTY()
    TObjectPtr<USightlineEntityRegistry> Registry;
    int32 SelfPlayerId = 0;                            // 登录回执分配，Registry 以此标 bSelf
    bool bInMatch = false;                             // 对局激活：收到 15 置位 / 自己 18 离场或链路死亡清位——16 上报的唯一合法窗口
    bool bSelfDead = false;                            // ep07：自己死亡态（23 victim=自己置位 / 24 清位）
    uint32 InputSeq = 0;                               // v4：本地预测序号（19 号；M3 预测/重放的历史锚点）
    TArray<FInputSample> InputHistory;                 // M3b：1s 环形输入史（对账/重放的本地底账）
    uint32 LastAckedSeq = 0;                           // 服务器最近确认的序号（对账基准；HUD 差值用）
    uint32 ReconcileFloorSeq = 0;                      // 对账地板（重生/新局清史时刻的 InputSeq）——
                                                       // ackedSeq 低于地板的快照=清史前生成的过期帧，跳过对账
                                                       // （死亡期 ackedSeq 冻结、在途快照权威位=尸体位：
                                                       //  不设地板=全量兜底把人从出生点拽回尸体位=残留拉扯根因）
    int32 ReconcileCount = 0;                          // 和解次数（≥ε 才计；断档兑底也计）
    int32 ReplayFrames = 0;                            // 最近一次回滚重放的输入帧数（断档兑底=0）
    double ErrCurrentCm = 0.0;                         // 当前对账误差（cm；ε 判定的那条）
    double ErrPeakCm = 0.0;                            // 峰值误差（cm）
    TMap<int32, int32> EntityHp;                       // M5：对局期各实体血量（21 号驱动；D5-09 事件流）
    TMap<int32, FSightlineScore> ScoreMap;             // 比分板客户端缓存（23 号事件流累加；15 号新局清账）
    TArray<FSightlineWeaponParams> RuntimeParams;      // ep08：30 号 per-weapon 参数覆盖层
    FString ConfigVersion;                             // ep08：HUD 版本行（v7-a3f2c1d8）
};
