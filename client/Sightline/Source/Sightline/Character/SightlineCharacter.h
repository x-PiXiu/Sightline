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

    /** 动画输入（ABP_Mannequin 经 Cast 读取；本地 Tick 聚合 / 远端 Tick 插值喂） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    FAnimInputs AnimInputs;

    /** 远端角色状态应用（PawnSpawner 经 17 快照调用）：只存目标位姿，插值在 Tick 里做 */
    void ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling);

    /** 覆写引擎速度：远端角色返回插值帧差分速度（驱动 ABP 标准 GetVelocity 节点）；本地角色走 CMC 真速度 */
    virtual FVector GetVelocity() const override {
        return bRemoteActive ? RemoteVelocity : Super::GetVelocity();
    }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /** 第一人称相机：挂头部、跟随控制旋转（无 SpringArm——那是第三人称做法） */
    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<UCameraComponent> Camera;

    /** 影子专用网格（M6 补丁）：备用——恢复 OwnerNoSee 隐藏方案时启用 */
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

    // —— 远端插值（VInterpTo + 帧差分速度，FPS_Game 已验证方案）——
    // 快照只存目标位姿；Tick 每帧 VInterpTo 向目标靠拢，速度 = 插值后帧位移 / 帧时间。
    // 速度天然等于动画系统"看到"的移动速度——同一插值轨迹上的导数，零失配。
    void TickRemoteInterpolation(float DeltaTime);

    TWeakObjectPtr<USightlineMatchSession> MatchSession;   // 上报出口（本地玩家绑定；远端为空不采样）
    FTimerHandle ReportTimer;

    // —— 远端插值状态（ApplyState 存目标，Tick 插值 + 算速度）——
    FVector RemoteTargetPos = FVector::ZeroVector;   // 最新快照位置（插值目标）
    float   RemoteTargetYaw = 0.f;                   // 最新快照朝向（插值目标）
    float   RemotePitchTarget = 0.f;                 // 最新快照俯仰（插值目标）
    bool    bRemoteActive = false;                    // 已收到首个快照（首帧直摆后开始插值）
    FVector RemoteVelocity = FVector::ZeroVector;    // 插值帧差分速度（GetVelocity 覆写返回此值）
};
