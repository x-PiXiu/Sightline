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

    /** FPP 头部位（ep03 相机基准 (18,0,82)） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera")
    FVector FirstPersonOffset = FVector(18.f, 0.f, 82.f);

    /** 模式切换混合速度（uu/s，VInterpTo 常量速度——约 0.3s 走完 80uu 全程） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera")
    float BlendSpeed = 300.f;

    virtual void TickComponent(float DeltaTime, ELevelTick TickType,
                               FActorComponentTickFunction* ThisTickFunction) override;

private:
    FVector TargetOffset() const;

    ESightlineCameraMode CurrentMode = ESightlineCameraMode::ThirdPerson;
};
