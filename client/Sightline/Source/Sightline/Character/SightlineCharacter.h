// SightlineCharacter —— 第 3 期客户端对局层角色（M2 段：本地可玩基线）
// 设计依据：docs/episodes/ep03/设计草案.md §5 + ui_wireframe/动画系统图
// 分工铁律：逻辑全 C++（输入/相机/FAnimInputs 聚合），ABP 只读聚合体（蓝图=表现层）
// 关键约束（§2.4）：ABP 不读 GetVelocity/IsFalling——本类替它采集并聚合进 FAnimInputs；
//                  本地角色=输入直读，远端角色(M3)=ApplyState 网络喂——同一 ABP 两种数据源
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Match/SightlineMatchSession.h"
#include "SightlineCharacter.generated.h"

class UCameraComponent;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;

/** 动画输入聚合体：ABP 的唯一数据入口（跨界 DTO——本地/远端两源在此归一） */
USTRUCT(BlueprintType)
struct FAnimInputs {
    GENERATED_BODY()

    /** 水平速度标量（cm/s） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    float Speed = 0.f;

    /** 移动方向相对角色朝向的夹角（度）：0=前，负=左，正=右（符号约定 = 自建 BS_Unarmed_Move 的 Direction 轴） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    float DirectionAngle = 0.f;

    /** 视角俯仰（度）：本地=控制旋转，远端=网络喂（AimOffset 输入） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    float Pitch = 0.f;

    /** 腾空（本地=移动组件判定；远端必须走网络 flags——直设位置不产生物理状态） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    bool bFalling = false;

    /** 武器类型：0=徒手（ep05 武器表起用） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    uint8 WeaponType = 0;
};

UCLASS()
class SIGHTLINE_API ASightlineCharacter : public ACharacter {
    GENERATED_BODY()

public:
    ASightlineCharacter();

    /** 动画输入（ABP_Mannequin 经 Cast 读取；本地 Tick 聚合 / 远端 ApplyState 喂） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    FAnimInputs AnimInputs;

    /** 远端角色状态应用（PawnSpawner 经 17 快照调用）：直设位置/朝向 + 差分速度喂 FAnimInputs（M4） */
    void ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling);

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /** 第一人称相机：挂头部、跟随控制旋转（无 SpringArm——那是第三人称做法） */
    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<UCameraComponent> Camera;

    /** 影子专用网格（M6 补丁）：OwnerNoSee 连影子一起藏，第一人称失去地面参照——
     *  双组件方案：本体对他人可见（OwnerNoSee 对本地隐藏），影子组件只对本地投影（本体不可见）。
     *  资产/动画在 BeginPlay 从 Mesh 拷贝——BP 子类零额外步骤 */
    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<USkeletalMeshComponent> ShadowMesh;

    /** 输入资产：构造函数按固定路径加载（/Game/Code/Input/...，蓝图工单③创建）；皮肤类槽留蓝图层 */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputMappingContext> DefaultMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> JumpAction;

private:
    void Move(const FInputActionValue& Value);   // WASD 相对视角方向（strafe 战位：角色恒朝视角）
    void Look(const FInputActionValue& Value);   // 鼠标 → Yaw/Pitch 控制旋转
    void SampleLocalState();                     // 30Hz：聚合 → 纯结构 → 会话（D3-17，M4）
    /** M3b 和解落地（订阅会话 OnSelfCorrected）：bFullSnap=true 时 Error 即绝对权威位（断档全量回滚）；
     *  否则 Error=对账误差向量，渲染位平移——本地预测的后续位移原样保留（位移空间重放） */
    UFUNCTION() void OnSelfCorrected(FVector Error, bool bFullSnap);

    // —— 远端插值缓冲（用户令 2026-10-20：ep04 必须交付平滑感——D4-07「远端直设」裁决 amend）——
    // 快照只入缓冲，变换每帧在 (now-插值延迟) 处两点夹取插值；TCP 有序到达=无需乱序/丢包处理。
    // 本地自机不经 ApplyState（v4.1 自实体走对账），缓冲恒空——插值天然只作用于远端。
    struct FRemoteSample {
        double TimeSec = 0.0;                          // 本地接收时刻（插值基准钟）
        FVector Pos = FVector::ZeroVector;
        float Yaw = 0.f;
    };
    /** 每帧远端插值：目标时刻=now-延迟 → 缓冲夹取 → 位置线性 + 朝向最短弧 */
    void TickRemoteInterpolation();

    TWeakObjectPtr<USightlineMatchSession> MatchSession;   // 上报出口（本地玩家绑定；远端为空不采样）
    FTimerHandle ReportTimer;
    TArray<FRemoteSample> InterpBuffer;          // 远端样本环（快照入缓冲，Tick 插值取样）
    double LastInterpTime = 0.0;                 // 上一帧插值时刻（俯仰平滑的 dt 基准）
    FVector SmoothedFrameVel = FVector::ZeroVector; // 路径解析速度的单极低通（到达抖动残差过滤）
    float RemotePitchTarget = 0.f;               // 远端俯仰目标（10Hz 快照值，Tick 内平滑跟进防台阶）
};
