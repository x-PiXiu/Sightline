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

    /** 远端角色状态应用（PawnSpawner 经 17 快照调用）：快照入缓冲，插值在 Tick 里做。
     *  AckedSeq=服务器确认序号——速度的发端时间基准（Δack×输入拍），免疫到达抖动 */
    void ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling, int32 AckedSeq = 0);

    /** 覆写引擎速度：远端角色返回插值速度（驱动 ABP 标准 GetVelocity 节点）；本地角色走 CMC 真速度 */
    virtual FVector GetVelocity() const override {
        return bRemoteActive ? RemoteVelocity : Super::GetVelocity();
    }

protected:
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<UCameraComponent> Camera;

    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<USkeletalMeshComponent> ShadowMesh;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputMappingContext> DefaultMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> JumpAction;

private:
    void Move(const FInputActionValue& Value);
    void Look(const FInputActionValue& Value);
    void SampleLocalState();
    UFUNCTION() void OnSelfCorrected(FVector Error, bool bFullSnap);

    // —— 远端插值：快照缓冲 + 恒速线性插值 ——
    // 两个带时间戳的快照，在 (Now - 延迟) 处线性插值。速度=位移/Δack 拍（发端时钟口径）。
    // ⚠ 速度分母用 ack 差不用接收时距——服务器 tick 与客户端输入两个时钟的采样撞拍/跳拍
    //   （日志实锤 ack 重复 1864→1864、跳号 1855→1857），接收时距当分母=速度 0↔800 来回摆
    //   =BS Speed 轴直立↔俯冲切换的根因。
    struct FSnapshot {
        double TimeSec = 0.0;
        FVector Pos = FVector::ZeroVector;
        float Yaw = 0.f;
        int32 Ack = 0;                             // 该快照的 ackedSeq（速度时间基准用）
    };
    void TickRemoteInterpolation(float DeltaTime);

    TWeakObjectPtr<USightlineMatchSession> MatchSession;
    FTimerHandle ReportTimer;

    // —— 远端插值状态 ——
    FSnapshot SnapA, SnapB;                       // 两点快照（A=旧，B=新）
    int32 SnapCount = 0;                          // 已收快照数（0/1/2+）
    float RemotePitchTarget = 0.f;
    bool  bRemoteActive = false;
    FVector RemoteVelocity = FVector::ZeroVector; // 恒速插值速度（GetVelocity 覆写返回此值）
};
