// SightlineCharacter 实现 —— M2 段：本地移动/视角/动画聚合（网络上报 M4 接 MatchSession，见 D3-17）
#include "Character/SightlineCharacter.h"

#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameInstance/SightlineGameInstance.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Sightline.h"
#include "UObject/ConstructorHelpers.h"

namespace {
    constexpr const TCHAR* kMappingContextPath = TEXT("/Game/Code/Input/IMC_Default.IMC_Default");
    constexpr const TCHAR* kMoveActionPath     = TEXT("/Game/Code/Input/IA_Move.IA_Move");
    constexpr const TCHAR* kLookActionPath     = TEXT("/Game/Code/Input/IA_Look.IA_Look");
    constexpr const TCHAR* kJumpActionPath     = TEXT("/Game/Code/Input/IA_Jump.IA_Jump");

    constexpr float kSpeedDeadZone = 5.f;

    // —— 恒速插值参数 ——
    constexpr float kInterpDelaySec = 0.025f;  // 渲染延迟 25ms < 快照间隔 33ms——渲染点落在 AB 窗口内真插值
                                               // （100ms 是 300ms 时代遗留：间隔回 33ms 后 Alpha 恒 0=30Hz 跳格）
    constexpr float kRotInterpSpeed = 10.f;    // 朝向/俯仰平滑速度
}

ASightlineCharacter::ASightlineCharacter() {
    bUseControllerRotationYaw = true;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->MaxWalkSpeed = 400.f;

    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(18.f, 0.f, 82.f));
    Camera->bUsePawnControlRotation = true;

    GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));

    if (GetWorld() && !IsRunningDedicatedServer()) {
        ConstructorHelpers::FObjectFinder<UInputMappingContext> Mctx(kMappingContextPath);
        if (Mctx.Succeeded()) DefaultMappingContext = Mctx.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Mact(kMoveActionPath);
        if (Mact.Succeeded()) MoveAction = Mact.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Lact(kLookActionPath);
        if (Lact.Succeeded()) LookAction = Lact.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Jact(kJumpActionPath);
        if (Jact.Succeeded()) JumpAction = Jact.Object;
    }
}

void ASightlineCharacter::BeginPlay() {
    Super::BeginPlay();
    const APlayerController* PC = Cast<APlayerController>(Controller);
    if (PC && DefaultMappingContext) {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer())) {
            Subsystem->AddMappingContext(DefaultMappingContext, 0);
        }
    }
    if (IsPlayerControlled()) {
        if (USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance()))
            MatchSession = GI->GetMatchSession();
        if (MatchSession.IsValid()) {
            MatchSession->OnSelfCorrected.AddDynamic(this, &ASightlineCharacter::OnSelfCorrected);
            GetWorldTimerManager().SetTimer(ReportTimer, this,
                &ASightlineCharacter::SampleLocalState, 1.0f / 30.0f, true);
        }
    } else {
        GetCharacterMovement()->DisableMovement();
        GetCharacterMovement()->GravityScale = 0.f;
    }
}

void ASightlineCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) {
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    if (DefaultMappingContext) {
        if (const APlayerController* PC2 = Cast<APlayerController>(Controller)) {
            if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
                ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC2->GetLocalPlayer())) {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
            }
        }
    }
    UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!Input) return;
    Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ASightlineCharacter::Move);
    Input->BindAction(LookAction, ETriggerEvent::Triggered, this, &ASightlineCharacter::Look);
    Input->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
    Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
}

void ASightlineCharacter::Move(const FInputActionValue& Value) {
    const FVector2D Axis = Value.Get<FVector2D>();
    if (!Controller || Axis.IsNearlyZero()) return;
    const FRotator YawRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
    AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::X), Axis.Y);
    AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), Axis.X);
}

void ASightlineCharacter::Look(const FInputActionValue& Value) {
    const FVector2D Axis = Value.Get<FVector2D>();
    AddControllerYawInput(Axis.X);
    AddControllerPitchInput(Axis.Y);
}

void ASightlineCharacter::Tick(float DeltaSeconds) {
    Super::Tick(DeltaSeconds);
    if (IsPlayerControlled()) {
        const FVector Vel = GetVelocity();
        const float Speed = Vel.Size2D();
        AnimInputs.Speed = Speed;
        if (Speed > kSpeedDeadZone) {
            const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
            const FVector Dir = Vel.GetSafeNormal2D();
            float Angle = FMath::RadiansToDegrees(
                FMath::Acos(FMath::Clamp(FVector::DotProduct(Fwd, Dir), -1.f, 1.f)));
            if (FVector::DotProduct(GetActorRightVector(), Dir) < 0.f) Angle = -Angle;
            AnimInputs.DirectionAngle = Angle;
        } else {
            AnimInputs.DirectionAngle = 0.f;
        }
        AnimInputs.Pitch = GetBaseAimRotation().Pitch;
        AnimInputs.bFalling = GetCharacterMovement()->IsFalling();
        if (CSightlineStatLog.GetValueOnGameThread() != 0 && GFrameCounter % 6 == 0) {
            UE_LOG(LogTemp, Log, TEXT("[ANIM] LOCAL  t=%.3f vel=%.1f spd=%.1f dir=%.1f pitch=%.1f"),
                   FPlatformTime::Seconds(), Vel.Size2D(),
                   AnimInputs.Speed, AnimInputs.DirectionAngle, AnimInputs.Pitch);
        }
    }
    TickRemoteInterpolation(DeltaSeconds);
}

void ASightlineCharacter::SampleLocalState() {
    if (!MatchSession.IsValid()) return;
    FSightlineLocalState S;
    S.Pos = GetActorLocation();
    S.Yaw = GetControlRotation().Yaw;
    S.Pitch = AnimInputs.Pitch;
    S.bFalling = AnimInputs.bFalling;
    MatchSession->ReportLocalState(S);
}

void ASightlineCharacter::OnSelfCorrected(const FVector Error, const bool bFullSnap) {
    const FVector Target = bFullSnap ? Error : GetActorLocation() + Error;
    SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
}

void ASightlineCharacter::ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling, int32 AckedSeq) {
    const double Now = FPlatformTime::Seconds();
    RemotePitchTarget = PitchDeg;
    AnimInputs.bFalling = bFalling;

    if (SnapCount == 0) {
        // 首个快照：直摆到位 + 作为 SnapA
        SnapA = {Now, Pos, Yaw, AckedSeq};
        SnapCount = 1;
        SetActorLocation(Pos);
        SetActorRotation(FRotator(0.f, Yaw, 0.f));
        RemoteVelocity = FVector::ZeroVector;
        bRemoteActive = true;
    } else if (SnapCount == 1) {
        // 第二个快照：入 SnapB，开始插值
        SnapB = {Now, Pos, Yaw, AckedSeq};
        SnapCount = 2;
    } else {
        // 后续快照：A←B，B←新（滑动窗口）
        SnapA = SnapB;
        SnapB = {Now, Pos, Yaw, AckedSeq};
    }

    if (CSightlineStatLog.GetValueOnGameThread() != 0) {
        UE_LOG(LogTemp, Log, TEXT("[ANIM] SNAP   t=%.3f yaw=%.1f"), Now, Yaw);
    }
}

void ASightlineCharacter::TickRemoteInterpolation(float DeltaTime) {
    if (SnapCount < 2) return;

    const double Now = FPlatformTime::Seconds();
    const double RenderTime = Now - kInterpDelaySec;

    // 恒速线性插值 + 超出区间时外推（防冻结跳变）
    // 正常：渲染点落在 [SnapA, SnapB] 窗口内（延迟 25ms < 间隔 33ms）。快照迟到时渲染点
    // 超出 SnapB → Alpha 允许 > 1 线性外推，速度×最大外推时间限幅防过冲。
    const double Span = FMath::Max(1e-6, SnapB.TimeSec - SnapA.TimeSec);
    const double RawAlpha = (RenderTime - SnapA.TimeSec) / Span;
    // 外推限幅：最多按当前速度再走 200ms（防急停时过冲穿墙）
    const float MaxExtrapolation = 0.2f;   // 200ms
    const float MaxAlpha = 1.0f + static_cast<float>(MaxExtrapolation / Span);
    const float Alpha = static_cast<float>(FMath::Clamp(RawAlpha, 0.0, MaxAlpha));

    const FVector OldLoc = GetActorLocation();
    const FVector NewLoc = FMath::Lerp(SnapA.Pos, SnapB.Pos, Alpha);
    SetActorLocation(NewLoc);

    // 恒速插值速度 = 位移 / Δack 拍（发端 30Hz 时钟口径——与动画天然同步且免疫到达抖动）。
    // ⚠ 分母必须用 ack 差：服务器 tick 与客户端输入两个时钟采样撞拍/跳拍（日志实锤 ack 重复、
    //   跳号），接收时距当分母=段速度 0↔800 来回摆=BS Speed 轴直立↔俯冲切换的根因。
    //   Δack=0（同输入被两拍采样）：保持上一拍速度不置零（否则动画闪回 Idle）。
    const int32 DAck = SnapB.Ack - SnapA.Ack;
    if (DAck > 0) {
        const FVector SegVel = (SnapB.Pos - SnapA.Pos) * (30.0f / static_cast<float>(DAck));
        RemoteVelocity = SegVel.GetClampedToMaxSize(450.f);   // 行走上限 400 + 余量，残余毛刺兜底
    }

    // 朝向插值（最短弧）
    const FRotator CurRot = GetActorRotation();
    const float TargetYaw = SnapA.Yaw + FMath::FindDeltaAngleDegrees(SnapA.Yaw, SnapB.Yaw) * Alpha;
    const FRotator NewRot = FMath::RInterpTo(CurRot, FRotator(0.f, TargetYaw, 0.f),
                                             DeltaTime, kRotInterpSpeed);
    SetActorRotation(FRotator(0.f, NewRot.Yaw, 0.f));

    // 写入 FAnimInputs
    AnimInputs.Speed = RemoteVelocity.Size2D();
    if (AnimInputs.Speed > kSpeedDeadZone) {
        const FVector Dir = RemoteVelocity.GetSafeNormal2D();
        const FRotator YawRot(0.f, NewRot.Yaw, 0.f);
        const FVector Fwd = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
        float Angle = FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(Fwd, Dir), -1.f, 1.f)));
        if (FVector::DotProduct(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), Dir) < 0.f) Angle = -Angle;
        AnimInputs.DirectionAngle = Angle;
    } else {
        AnimInputs.DirectionAngle = 0.f;
    }

    AnimInputs.Pitch = FMath::FInterpTo(AnimInputs.Pitch, RemotePitchTarget,
                                        DeltaTime, kRotInterpSpeed);

    // 诊断日志
    if (CSightlineStatLog.GetValueOnGameThread() != 0 && GFrameCounter % 6 == 0) {
        const float Dist = FVector::Dist2D(NewLoc, SnapB.Pos);
        UE_LOG(LogTemp, Log, TEXT("[ANIM] REMOTE t=%.3f vel=%.1f spd=%.1f dir=%.1f dist=%.1f pitch=%.1f"),
               FPlatformTime::Seconds(), RemoteVelocity.Size2D(),
               AnimInputs.Speed, AnimInputs.DirectionAngle, Dist, AnimInputs.Pitch);
    }
}
