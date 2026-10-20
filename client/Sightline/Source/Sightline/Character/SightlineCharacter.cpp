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
#include "UObject/ConstructorHelpers.h"

namespace {
    // 输入资产固定路径（蓝图工单③创建；路径即契约，改资产位置必须同步这里）
    // 输入资产固定路径（蓝图工单③创建于 /Game/Code/Input；路径即契约，改资产位置必须同步这里）
    constexpr const TCHAR* kMappingContextPath = TEXT("/Game/Code/Input/IMC_Default.IMC_Default");
    constexpr const TCHAR* kMoveActionPath     = TEXT("/Game/Code/Input/IA_Move.IA_Move");
    constexpr const TCHAR* kLookActionPath     = TEXT("/Game/Code/Input/IA_Look.IA_Look");
    constexpr const TCHAR* kJumpActionPath     = TEXT("/Game/Code/Input/IA_Jump.IA_Jump");

    // 名册动作判定阈值（°/cm 级）：低于此速度归 Idle；方向角只在此之上计算
    constexpr float kSpeedDeadZone = 5.f;
}

ASightlineCharacter::ASightlineCharacter() {
    // strafe 战位：角色朝向 = 控制旋转（FPS 标准），移动不改变朝向（朝向由视角决定）
    bUseControllerRotationYaw = true;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->MaxWalkSpeed = 400.f;   // 行进速度手感（默认 600 偏"竞速"；BS 轴 0~500 范围内）

    // 第一人称相机：头部高度（胶囊半高 88，眼睛在 ~+75），跟随控制旋转
    Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->SetRelativeLocation(FVector(18.f, 0.f, 82.f));   // 眼位：微前移+微抬高——低头不裁肩颈
    Camera->bUsePawnControlRotation = true;

    // 第一人称可见性（2026-10-17 按领导指示调整）：身体对本地可见——低头看得到腿脚，影子由本体自然投射
    // （原"OwnerNoSee 隐藏 + 影子网格"方案注释保留于下方，恢复隐藏时取消注释即可）
    // GetMesh()->SetOwnerNoSee(true);

    // 模型朝向对齐（用户实测校准）：SKM_Manny/Quinn 原生面朝 +Y，胶囊逻辑前向是 +X——
    // Mesh 转 -90 yaw 后视觉朝向 = 逻辑朝向 = 网络朝向
    GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));

    // —— 影子网格组件（原"隐藏+投影"方案，现备用）：恢复隐藏方案时取消本块注释 ——
    // ShadowMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("ShadowMesh"));
    // ShadowMesh->SetupAttachment(GetCapsuleComponent());
    // ShadowMesh->SetRelativeLocation(FVector(0.f, 0.f, -88.f));
    // ShadowMesh->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
    // ShadowMesh->SetHiddenInGame(true);
    // ShadowMesh->SetCastHiddenShadow(true);
    // ShadowMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

    // 输入资产按固定路径加载（工单③已建；缺失仅报日志不崩溃——PIE 无输入，回查路径）
    if (GetWorld() && !IsRunningDedicatedServer()) {
        ConstructorHelpers::FObjectFinder<UInputMappingContext> Mctx(kMappingContextPath);
        if (Mctx.Succeeded()) DefaultMappingContext = Mctx.Object; else UE_LOG(LogTemp, Error, TEXT("[Sightline] 缺 %s —— 检查蓝图工单③"), kMappingContextPath);
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
    // 输入映射只挂在被本地玩家操纵的角色上（M3 远端 Pawn 无 Controller，天然跳过）
    const APlayerController* PC = Cast<APlayerController>(Controller);
    UE_LOG(LogTemp, Log, TEXT("[输入] BeginPlay：Controller=%s IMC=%s"),
           PC ? TEXT("有") : TEXT("无(尚未Possess)"),
           DefaultMappingContext ? TEXT("已加载") : TEXT("为空"));
    if (PC && DefaultMappingContext) {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
            ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer())) {
            Subsystem->AddMappingContext(DefaultMappingContext, 0);
        }
    }
    // M4：本地玩家 30Hz 采样上报（D3-17/D3-08）——远端 Pawn 无 Controller 天然不采样
    if (IsPlayerControlled()) {
        if (USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance()))
            MatchSession = GI->GetMatchSession();
        if (MatchSession.IsValid()) {
            MatchSession->OnSelfCorrected.AddDynamic(this, &ASightlineCharacter::OnSelfCorrected);  // M3b 和解订阅
            GetWorldTimerManager().SetTimer(ReportTimer, this,
                &ASightlineCharacter::SampleLocalState, 1.0f / 30.0f, true);
        }
    }
}

void ASightlineCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) {
    Super::SetupPlayerInputComponent(PlayerInputComponent);
    // IMC 主挂载点：本函数在 possess 建输入组件时才被调——Controller 必然在位。
    // （BeginPlay 早于 Possess 时 Controller 为空会跳过挂载——M6 联调实锤"WASD 无响应"的根因）
    if (DefaultMappingContext) {
        if (const APlayerController* PC2 = Cast<APlayerController>(Controller)) {
            if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
                ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC2->GetLocalPlayer())) {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
                UE_LOG(LogTemp, Log, TEXT("[输入] IMC 挂载（SetupPlayerInput）✓"));
            } else {
                UE_LOG(LogTemp, Error, TEXT("[输入] 无 EnhancedInput 子系统——检查 DefaultInput.ini"));
            }
        }
    } else {
        UE_LOG(LogTemp, Error, TEXT("[输入] DefaultMappingContext 为空——FObjectFinder 加载失败？"));
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
    // Y=前后（W/S），X=左右（D/A）——IMC 里 Swizzle/Negate 的结果，见蓝图方案步骤 2.2~2.5
    AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::X), Axis.Y);
    AddMovementInput(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), Axis.X);
}

void ASightlineCharacter::Look(const FInputActionValue& Value) {
    const FVector2D Axis = Value.Get<FVector2D>();
    AddControllerYawInput(Axis.X);
    AddControllerPitchInput(Axis.Y);   // 抬头方向反了 = 回 IMC 给 Y 加 Negate（蓝图方案步骤 2.6 坑位）
}

void ASightlineCharacter::Tick(float DeltaSeconds) {
    Super::Tick(DeltaSeconds);
    // FAnimInputs 聚合：本地=引擎采集（ABP 禁止自己读引擎，§2.4）
    const FVector Vel = GetVelocity();
    const float Speed = Vel.Size2D();
    AnimInputs.Speed = Speed;
    if (Speed > kSpeedDeadZone) {
        const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
        const FVector Dir = Vel.GetSafeNormal2D();
        float Angle = FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(Fwd, Dir), -1.f, 1.f)));
        if (FVector::DotProduct(GetActorRightVector(), Dir) < 0.f) Angle = -Angle;
        AnimInputs.DirectionAngle = Angle;   // 0=前 负=左 正=右 —— 符号约定由自建 BS_Unarmed_Move 的 Direction 轴定义（-90 左/+90 右），已与 C++ 对表
    } else {
        AnimInputs.DirectionAngle = 0.f;
    }
    AnimInputs.Pitch = GetBaseAimRotation().Pitch;
    AnimInputs.bFalling = GetCharacterMovement()->IsFalling();
    // WeaponType 恒 0（徒手）；30Hz 上报由 ReportTimer → SampleLocalState 承担（D3-17 已接线）
}

void ASightlineCharacter::SampleLocalState() {
    if (!MatchSession.IsValid()) return;
    FSightlineLocalState S;
    S.Pos = GetActorLocation();
    S.Yaw = GetControlRotation().Yaw;
    S.Pitch = AnimInputs.Pitch;
    S.bFalling = AnimInputs.bFalling;
    MatchSession->ReportLocalState(S);                       // 纯结构过界，组帧在会话层（D3-17）
}

void ASightlineCharacter::OnSelfCorrected(const FVector Error, const bool bFullSnap) {
    // M3b 和解落地：瞬移摆正——CMC 以新位置为基准继续本地预测（TeleportPhysics 不带速度惩罚）；
    // 朝向不纠正（服务器存末次上报即权威，无对账分歧）。30Hz 上报下一拍即以修正位入缓冲/发包。
    const FVector Target = bFullSnap ? Error : GetActorLocation() + Error;
    SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
}

void ASightlineCharacter::ApplyState(const FVector& Pos, float Yaw, float PitchDeg, bool bFalling) {
    const double Now = FPlatformTime::Seconds();
    const float Dt = (LastRemoteTime > 0.0) ? static_cast<float>(Now - LastRemoteTime) : 0.f;
    const FVector Prev = GetActorLocation();
    SetActorLocation(Pos);
    SetActorRotation(FRotator(0.f, Yaw, 0.f));               // 远端无 Controller：直接摆朝向

    // 速度/方向 = 位置差分（EMA 低通——D3-02），与本地 Tick 同一套 FAnimInputs 语义：
    // 远端不读引擎速度（直设不产生物理速度，§2.4），差分就是唯一真相
    const FVector Vel = (Dt > 0.001f) ? (Pos - Prev) / Dt : FVector::ZeroVector;
    const float Inst = Vel.Size2D();
    AnimInputs.Speed = FMath::Lerp(AnimInputs.Speed, Inst, 0.5f);
    if (AnimInputs.Speed > kSpeedDeadZone) {
        const FVector Dir = Vel.GetSafeNormal2D();
        const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
        float Angle = FMath::RadiansToDegrees(
            FMath::Acos(FMath::Clamp(FVector::DotProduct(Fwd, Dir), -1.f, 1.f)));
        if (FVector::DotProduct(GetActorRightVector(), Dir) < 0.f) Angle = -Angle;
        AnimInputs.DirectionAngle = Angle;
    }
    AnimInputs.Pitch = PitchDeg;
    AnimInputs.bFalling = bFalling;
    LastRemoteTime = Now;
}
