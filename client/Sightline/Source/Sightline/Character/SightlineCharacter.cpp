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
    // 输入资产固定路径（蓝图工单③创建；路径即契约，改资产位置必须同步这里）
    constexpr const TCHAR* kMappingContextPath = TEXT("/Game/Code/Input/IMC_Default.IMC_Default");
    constexpr const TCHAR* kMoveActionPath     = TEXT("/Game/Code/Input/IA_Move.IA_Move");
    constexpr const TCHAR* kLookActionPath     = TEXT("/Game/Code/Input/IA_Look.IA_Look");
    constexpr const TCHAR* kJumpActionPath     = TEXT("/Game/Code/Input/IA_Jump.IA_Jump");

    // 名册动作判定阈值（°/cm 级）：低于此速度归 Idle；方向角只在此之上计算
    constexpr float kSpeedDeadZone = 5.f;

    // —— 远端插值参数（VInterpTo + 帧差分速度）——
    constexpr float kRemoteInterpSpeed = 10.f;     // VInterpTo 指数插值速度（时间常数≈100ms，够吸收 10Hz 网络抖动）
    constexpr float kRemoteRotSpeed   = 10.f;      // 朝向/俯仰插值速度
}

ASightlineCharacter::ASightlineCharacter() {
    // strafe 战位：角色朝向 = 控制旋转（FPS 标准），移动不改变朝向（朝向由视角决定）
    bUseControllerRotationYaw = true;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->MaxWalkSpeed = 400.f;

    // 第一人称相机：头部高度，跟随控制旋转
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(18.f, 0.f, 82.f));
    Camera->bUsePawnControlRotation = true;

    // 模型朝向对齐：SKM_Manny/Quinn 原生面朝 +Y，胶囊逻辑前向是 +X
    GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));

    // 输入资产按固定路径加载（缺失仅报日志不崩溃）
    if (GetWorld() && !IsRunningDedicatedServer()) {
        ConstructorHelpers::FObjectFinder<UInputMappingContext> Mctx(kMappingContextPath);
        if (Mctx.Succeeded()) DefaultMappingContext = Mctx.Object; else UE_LOG(LogTemp, Error, TEXT("[Sightline] 缺 %s"), kMappingContextPath);
        ConstructorHelpers::FObjectFinder<UInputAction> Mact(kMoveActionPath);
        if (Mact.Succeeded()) MoveAction = Mact.Object; else UE_LOG(LogTemp, Error, TEXT("[Sightline] 缺 %s"), kMoveActionPath);
        ConstructorHelpers::FObjectFinder<UInputAction> Lact(kLookActionPath);
        if (Lact.Succeeded()) LookAction = Lact.Object; else UE_LOG(LogTemp, Error, TEXT("[Sightline] 缺 %s"), kLookActionPath);
        ConstructorHelpers::FObjectFinder<UInputAction> Jact(kJumpActionPath);
        if (Jact.Succeeded()) JumpAction = Jact.Object; else UE_LOG(LogTemp, Error, TEXT("[Sightline] 缺 %s"), kJumpActionPath);
    }
}

void ASightlineCharacter::BeginPlay() {
    Super::BeginPlay();
    // 输入映射只挂在被本地玩家操纵的角色上（远端 Pawn 无 Controller，天然跳过）
    const APlayerController* PC = Cast<APlayerController>(Controller);
    if (PC && DefaultMappingContext) {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer())) {
            Subsystem->AddMappingContext(DefaultMappingContext, 0);
        }
    }
    // 本地玩家 30Hz 采样上报——远端 Pawn 无 Controller 天然不采样
    if (IsPlayerControlled()) {
        if (USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance()))
            MatchSession = GI->GetMatchSession();
        if (MatchSession.IsValid()) {
            MatchSession->OnSelfCorrected.AddDynamic(this, &ASightlineCharacter::OnSelfCorrected);
            GetWorldTimerManager().SetTimer(ReportTimer, this,
                &ASightlineCharacter::SampleLocalState, 1.0f / 30.0f, true);
        }
    } else {
        // 远端=纯运动学木偶：关 CMC 物理，避免重力/摩擦与插值摆位打架
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
    // 本地角色：引擎速度直采（ABP 禁止自己读引擎，§2.4）
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
        // 本地对照日志（s.Sightline.AnimDebug 1 开启）——与 [ANIM] REMOTE 同格式，对比速度/方向角是否一致
        if (CSightlineStatLog.GetValueOnGameThread() != 0) {
            UE_LOG(LogTemp, Log, TEXT("[ANIM] LOCAL  t=%.3f vel=%.1f spd=%.1f dir=%.1f pitch=%.1f"),
                   FPlatformTime::Seconds(), Vel.Size2D(),
                   AnimInputs.Speed, AnimInputs.DirectionAngle, AnimInputs.Pitch);
        }
    }
    // 远端角色：VInterpTo 插值 + 帧差分速度喂动画
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

void ASightlineCharacter::ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling) {
    // 远端路径：快照只存目标位姿，插值与动画在 TickRemoteInterpolation 里做
    // 诊断：快照到达间隔与目标跳跃幅度（s.Sightline.AnimDebug 1 开启）
    if (CSightlineStatLog.GetValueOnGameThread() != 0 && bRemoteActive) {
        const float Jump = FVector::Dist(Pos, RemoteTargetPos);
        UE_LOG(LogTemp, Log, TEXT("[ANIM] SNAP   t=%.3f jump=%.1f yaw=%.1f"),
               FPlatformTime::Seconds(), Jump, Yaw);
    }
    RemoteTargetPos = Pos;
    RemoteTargetYaw = Yaw;
    RemotePitchTarget = PitchDeg;
    AnimInputs.bFalling = bFalling;

    if (!bRemoteActive) {
        // 首个快照：直摆到位（此时无历史可插，直接吸附避免从出生点"走"过来）
        SetActorLocation(Pos);
        SetActorRotation(FRotator(0.f, Yaw, 0.f));
        RemoteVelocity = FVector::ZeroVector;
        bRemoteActive = true;
    }
}

void ASightlineCharacter::TickRemoteInterpolation(float DeltaTime) {
    if (!bRemoteActive) return;

    // —— 位置插值：VInterpTo 指数衰减向目标靠拢（FPS_Game 已验证方案）——
    // 速度 = 插值后的真实帧位移 / 帧时间——动画播什么速度、角色移多快，是同一组数字，零失配。
    const FVector OldLoc = GetActorLocation();
    const FVector NewLoc = FMath::VInterpTo(OldLoc, RemoteTargetPos, DeltaTime, kRemoteInterpSpeed);
    SetActorLocation(NewLoc);

    // ★ 帧差分速度：驱动动画的核心（覆写 GetVelocity 也返回此值）
    RemoteVelocity = (DeltaTime > KINDA_SMALL_NUMBER)
        ? (NewLoc - OldLoc) / DeltaTime
        : FVector::ZeroVector;

    // —— 朝向插值（最短弧）——
    const FRotator CurRot = GetActorRotation();
    const FRotator NewRot = FMath::RInterpTo(CurRot, FRotator(0.f, RemoteTargetYaw, 0.f),
                                             DeltaTime, kRemoteRotSpeed);
    SetActorRotation(FRotator(0.f, NewRot.Yaw, 0.f));

    // —— 写入 FAnimInputs（ABP 读这个聚合体）——
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

    // 俯仰平滑跟进（10Hz 快照值 → 连续无台阶）
    AnimInputs.Pitch = FMath::FInterpTo(AnimInputs.Pitch, RemotePitchTarget,
                                        DeltaTime, kRemoteRotSpeed);

    // —— 诊断日志（s.Sightline.AnimDebug 1 开启）——
    // 判读方法：
    //   位置问题 → DistToTarget 不收敛（持续 >10cm）或 Vel 毛刺尖峰
    //   动画问题 → Vel/Speed 平滑但 Dir 跳变（±180 翻转=BS 轴突变=俯冲/抽搐根因）
    if (CSightlineStatLog.GetValueOnGameThread() != 0) {
        const float Dist = FVector::Dist2D(NewLoc, RemoteTargetPos);
        UE_LOG(LogTemp, Log, TEXT("[ANIM] REMOTE t=%.3f vel=%.1f spd=%.1f dir=%.1f dist=%.1f pitch=%.1f"),
               FPlatformTime::Seconds(), RemoteVelocity.Size2D(),
               AnimInputs.Speed, AnimInputs.DirectionAngle, Dist, AnimInputs.Pitch);
    }
}
