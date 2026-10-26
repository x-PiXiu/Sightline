#include "Camera/SightlineCameraComponent.h"

USightlineCameraComponent::USightlineCameraComponent() {
    PrimaryComponentTick.bCanEverTick = true;
    // 初始即落在 TPP 偏移上（避免首帧从原点漂移）；挂接由 Character 构造函数 SetupAttachment 完成
    SetRelativeLocation(ThirdPersonOffset);
}

void USightlineCameraComponent::ToggleCameraMode() {
    SetCameraMode(CurrentMode == ESightlineCameraMode::ThirdPerson
                      ? ESightlineCameraMode::FirstPerson
                      : ESightlineCameraMode::ThirdPerson);
}

void USightlineCameraComponent::SetCameraMode(ESightlineCameraMode Mode) {
    if (Mode == CurrentMode) return;
    CurrentMode = Mode;
    OnCameraModeChanged.Broadcast(Mode);     // Character 订阅 → 双枪/身体分层显隐（D5-15）
}

FVector USightlineCameraComponent::TargetOffset() const {
    return CurrentMode == ESightlineCameraMode::FirstPerson ? FirstPersonOffset : ThirdPersonOffset;
}

void USightlineCameraComponent::TickComponent(float DeltaTime, ELevelTick TickType,
                                              FActorComponentTickFunction* ThisTickFunction) {
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    // 偏移插值混合（旋转不掺和——bUsePawnControlRotation 恒跟控制器视角）
    SetRelativeLocation(FMath::VInterpTo(GetRelativeLocation(), TargetOffset(), DeltaTime, BlendSpeed));
}
