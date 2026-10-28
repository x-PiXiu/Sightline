// SightlineCameraComponent —— 第 5 期相机模式栈（Lyra 式缩小版 · D5-15）
// Lyra 对照：LyraCameraComponent + LyraCameraMode 栈（多模式权重混合）——缩小为
//            TPP/FPP 双模式 + 偏移插值 + 切换委托；砍掉多模式泛化/pivot 骨骼/碰撞探测（SpringArm ep14）
// 职责：偏移驱动（Tick 插值到目标模式偏移）+ 模式切换广播；旋转仍走 bUsePawnControlRotation（ep03 基准）
// 显隐不在本组件：Character 订阅 OnCameraModeChanged 驱动双枪/身体分层显隐（装备双模型，D5-15）
#pragma once
#include "Camera/CameraComponent.h"
#include "SightlineCameraComponent.generated.h"

UENUM(BlueprintType)
enum class ESightlineCameraMode : uint8
{
    ThirdPerson = 0,    // 越肩（PUBG TPP 同款）：偏移 = 身后右肩
    FirstPerson = 1     // 第一人称：偏移 = 头部位（ep03 基准）
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnSightlineCameraModeChanged, ESightlineCameraMode, NewMode);

UCLASS(ClassGroup = (Sightline), meta = (BlueprintSpawnableComponent),
       DisplayName = "Sightline 相机（模式栈）")
class SIGHTLINE_API USightlineCameraComponent : public UCameraComponent {
    GENERATED_BODY()

public:
    USightlineCameraComponent();

    UFUNCTION(BlueprintCallable, Category = "Sightline|Camera")
    void ToggleCameraMode();

    UFUNCTION(BlueprintCallable, Category = "Sightline|Camera")
    void SetCameraMode(ESightlineCameraMode Mode);

    UFUNCTION(BlueprintPure, Category = "Sightline|Camera")
    ESightlineCameraMode GetCameraMode() const { return CurrentMode; }

    UPROPERTY(BlueprintAssignable)
    FOnSightlineCameraModeChanged OnCameraModeChanged;

    /** TPP 越肩偏移（相对胶囊；X 负=身后，Y 正=右肩，Z=高度） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera")
    FVector ThirdPersonOffset = FVector(-45.f, 35.f, 70.f);

    /** FPP 眼高位（Lyra 同款：GetPivotLocation 用 BaseEyeHeight≈62——枪/手臂经持枪姿势自然入镜，无悬浮枪）。
     *  Y 右移让枪身向屏幕中线收（枪口指向准星感）；细调在 BP Character Class Defaults 里的同名属性 */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera")
    FVector FirstPersonOffset = FVector(8.f, 14.f, 62.f);

    /** FPP 近裁剪面（cm）：相机前此距离内不渲染——低头/抬头手臂躯干怼脸穿模的解法
     *  （用户 2026-10-28 报穿模；引擎默认 10cm 不够）。贴脸枪身距相机约 25~40cm，>20 会切枪尾；
     *  ≤0=回到引擎默认 */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera", meta = (ClampMin = "0"))
    float NearClipPlaneCm = 15.f;

    /** 模式切换混合速度（uu/s，VInterpTo 常量速度——约 0.3s 走完 80uu 全程） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera")
    float BlendSpeed = 300.f;

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

    /** 把近裁剪面注进视图（UCameraComponent 只暴露 Ortho 系，透视近面在 FMinimalViewInfo 上——
     *  引擎默认走 GNearClippingPlane=10cm，不可调；覆写本函数是我们唯一的注入点） */
    virtual void GetCameraView(float DeltaTime, FMinimalViewInfo& DesiredView) override;

private:
    FVector TargetOffset() const;

    ESightlineCameraMode CurrentMode = ESightlineCameraMode::FirstPerson;   // 默认 FPP（用户裁决 2026-10-27）
};
