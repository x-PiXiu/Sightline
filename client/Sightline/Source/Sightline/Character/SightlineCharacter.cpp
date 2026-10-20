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

    // —— 远端插值（D4-07 amend · 用户令 2026-10-20）——
    constexpr float kInterpDelaySec = 0.15f;     // 渲染延迟 150ms≈1.5 个快照间隔（10Hz 拍下留抖动余量）
    constexpr int32 kInterpBufMax = 16;          // 样本环上限（10Hz×1.6s 冗余）
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
    } else {
        // 远端=纯运动学木偶（插值驱动，D4-07 amend）：关 CMC 物理——重力/摩擦不再与每帧摆位打架
        // （首版直设 10Hz 掩盖了这矛盾；插值 60fps 摆位后 CMC 每帧回拉=卡顿感来源之一）
        GetCharacterMovement()->DisableMovement();
        GetCharacterMovement()->GravityScale = 0.f;
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
    // FAnimInputs 聚合：本地=引擎采集（ABP 禁止自己读引擎，§2.4）。
    // ⚠ 只许本地走这条——远端引擎速度恒 0（运动学木偶），每帧覆盖会把动画打回 Idle（首版插值踩坑）
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
            AnimInputs.DirectionAngle = Angle;   // 0=前 负=左 正=右 —— 符号约定由自建 BS_Unarmed_Move 的 Direction 轴定义（-90 左/+90 右），已与 C++ 对表
        } else {
            AnimInputs.DirectionAngle = 0.f;
        }
        AnimInputs.Pitch = GetBaseAimRotation().Pitch;
        AnimInputs.bFalling = GetCharacterMovement()->IsFalling();
    }
    // 远端平滑（D4-07 amend）：快照已入样本环，这里每帧取插值位摆变换 + 用插值帧速度喂动画——
    // 本地自机缓冲恒空天然跳过
    TickRemoteInterpolation();
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
    // 远端路径（D4-07 amend · 2026-10-20）：快照只入样本环，变换与动画输入均由
    // TickRemoteInterpolation 每帧以插值帧速度驱动——60fps 平滑滑动且脚跟着地跑。
    const double Now = FPlatformTime::Seconds();
    AnimInputs.Pitch = PitchDeg;                           // 朝向类输入仍 10Hz（ABP 内部平滑）
    AnimInputs.bFalling = bFalling;

    const bool bFirst = InterpBuffer.Num() == 0;
    InterpBuffer.Add({Now, Pos, Yaw});
    while (InterpBuffer.Num() > kInterpBufMax) InterpBuffer.RemoveAt(0);
    if (bFirst) {                                          // 首样：无历史可插，直摆到位
        SetActorLocation(Pos);
        SetActorRotation(FRotator(0.f, Yaw, 0.f));
        LastInterpPos = Pos;
        LastInterpTime = Now;
    }
}

void ASightlineCharacter::TickRemoteInterpolation() {
    if (InterpBuffer.Num() < 2) return;                    // 无/单样本：无可插（单样本已直摆）
    const double Target = FPlatformTime::Seconds() - kInterpDelaySec;
    const FRemoteSample& Oldest = InterpBuffer[0];
    const FRemoteSample& Newest = InterpBuffer.Last();
    // 两端夹取：追不上（卡顿恢复）贴最老样本；断流（对端停更）贴最新样本——渲染延迟只伸不跳
    const FRemoteSample* A = nullptr;
    const FRemoteSample* B = nullptr;
    if (Target <= Oldest.TimeSec) { A = &Oldest; B = &Newest; }
    else if (Target >= Newest.TimeSec) { A = &Newest; B = &Newest; }
    else {
        for (int32 i = InterpBuffer.Num() - 2; i >= 0; --i) {
            if (Target >= InterpBuffer[i].TimeSec && Target <= InterpBuffer[i + 1].TimeSec) {
                A = &InterpBuffer[i]; B = &InterpBuffer[i + 1]; break;
            }
        }
        if (!A) return;
    }
    const double Span = B->TimeSec - A->TimeSec;
    const float Alpha = (Span > 1e-6) ? static_cast<float>((Target - A->TimeSec) / Span) : 1.f;
    const FVector NewPos = FMath::Lerp(A->Pos, B->Pos, Alpha);
    SetActorLocation(NewPos);
    const float Yaw = A->Yaw + FMath::FindDeltaAngleDegrees(A->Yaw, B->Yaw) * Alpha;   // 最短弧
    SetActorRotation(FRotator(0.f, Yaw, 0.f));

    // 动画输入=插值帧速度（60fps 口径）——远端引擎速度恒 0，插值位移差分是唯一真相（§2.4 同款纪律）
    const double Now = FPlatformTime::Seconds();
    if (LastInterpTime > 0.0) {
        const float FrameDt = static_cast<float>(Now - LastInterpTime);
        const FVector FrameVel = (FrameDt > 0.0005f) ? (NewPos - LastInterpPos) / FrameDt : FVector::ZeroVector;
        AnimInputs.Speed = FrameVel.Size2D();
        if (AnimInputs.Speed > kSpeedDeadZone) {
            const FVector Dir = FrameVel.GetSafeNormal2D();
            const FRotator YawRot(0.f, Yaw, 0.f);
            const FVector Fwd = FRotationMatrix(YawRot).GetUnitAxis(EAxis::X);
            float Angle = FMath::RadiansToDegrees(
                FMath::Acos(FMath::Clamp(FVector::DotProduct(Fwd, Dir), -1.f, 1.f)));
            if (FVector::DotProduct(FRotationMatrix(YawRot).GetUnitAxis(EAxis::Y), Dir) < 0.f) Angle = -Angle;
            AnimInputs.DirectionAngle = Angle;             // 与本地 Tick 同一符号约定
        } else {
            AnimInputs.DirectionAngle = 0.f;
        }
    }
    LastInterpPos = NewPos;
    LastInterpTime = Now;
}
