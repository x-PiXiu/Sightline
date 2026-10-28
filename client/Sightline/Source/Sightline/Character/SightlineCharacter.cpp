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
#include "Animation/AnimMontage.h"
#include "Game/SightlineBakeLib.h"
#include "Game/SightlineDecalManagerSubsystem.h"
#include "Game/SightlineOccluderComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Match/SightlinePawnSpawner.h"
#include "NiagaraComponent.h"
#include "NiagaraSystem.h"
#include "Sound/SoundBase.h"
#include "Blueprint/UserWidget.h"
#include "Game/SightlineObjectPoolSubsystem.h"
#include "UI/SightlineUIWidgets.h"

namespace {
    constexpr const TCHAR* kMappingContextPath = TEXT("/Game/Code/Input/IMC_Default.IMC_Default");
    constexpr const TCHAR* kMoveActionPath     = TEXT("/Game/Code/Input/IA_Move.IA_Move");
    constexpr const TCHAR* kLookActionPath     = TEXT("/Game/Code/Input/IA_Look.IA_Look");
    constexpr const TCHAR* kJumpActionPath     = TEXT("/Game/Code/Input/IA_Jump.IA_Jump");
    constexpr const TCHAR* kFireActionPath     = TEXT("/Game/Code/Input/IA_Fire.IA_Fire");   // M5：仓库资产→构造期加载（BP 零手填）
    constexpr const TCHAR* kCameraTogglePath   = TEXT("/Game/Code/Input/IA_Camera.IA_Camera"); // M5：视角切换（D5-15，资产未建静默跳过）

    constexpr float kSpeedDeadZone = 5.f;

    // —— 恒速插值参数 ——
    constexpr float kInterpDelaySec = 0.025f;  // 渲染延迟 25ms < 快照间隔 33ms——渲染点落在 AB 窗口内真插值
                                               // （100ms 是 300ms 时代遗留：间隔回 33ms 后 Alpha 恒 0=30Hz 跳格）
    constexpr float kRotInterpSpeed = 10.f;    // 朝向/俯仰平滑速度
    // —— M5 开火（表现层预测常量=服务器合同镜像；D5-14 客户端步进 100ms>服务器容差 95ms）——
    constexpr uint64 kClientFireIntervalMs = 100;
    constexpr int32  kClientMag            = 30;
    constexpr uint64 kClientReloadMs       = 2500;
    // —— M5 弹孔贴花（落点短程探针参数；回收走引擎 LifeSpan 无需 FIFO）——
    constexpr float  kDecalProbeBackCm = 15.f; // 探针起点=落点向射手侧回退（墙面外侧）
    constexpr float  kDecalProbeFwdCm  = 5.f;  // 探针终点=落点向墙内推进（短程：天端点探不到=自动跳过）
}

ASightlineCharacter::ASightlineCharacter() {
    bUseControllerRotationYaw = true;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    GetCharacterMovement()->bOrientRotationToMovement = false;
    GetCharacterMovement()->MaxWalkSpeed = 400.f;

    Camera = CreateDefaultSubobject<USightlineCameraComponent>(TEXT("Camera"));
    Camera->SetupAttachment(GetCapsuleComponent());
    Camera->bUsePawnControlRotation = true;
    // 偏移由组件驱动（D5-15：TPP(-45,35,70)/FPP(18,0,82) 双模式插值）——不再写死 ep03 头部位

    // 模型原生朝 +Y：偏航=MeshYawOffset（-90 对齐胶囊 +X；BP Class Defaults 可调，免编译迭代）
    GetMesh()->SetRelativeRotation(FRotator(0.f, MeshYawOffset, 0.f));

    if (GetWorld() && !IsRunningDedicatedServer()) {
        ConstructorHelpers::FObjectFinder<UInputMappingContext> Mctx(kMappingContextPath);
        if (Mctx.Succeeded()) DefaultMappingContext = Mctx.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Mact(kMoveActionPath);
        if (Mact.Succeeded()) MoveAction = Mact.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Lact(kLookActionPath);
        if (Lact.Succeeded()) LookAction = Lact.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Jact(kJumpActionPath);
        if (Jact.Succeeded()) JumpAction = Jact.Object;
        ConstructorHelpers::FObjectFinder<UInputAction> Fact(kFireActionPath);
        if (Fact.Succeeded()) FireAction = Fact.Object;   // 资产未建时静默跳过（M5-A 步骤 1.1 建完即自动接线）
        ConstructorHelpers::FObjectFinder<UInputAction> CamTact(kCameraTogglePath);
        if (CamTact.Succeeded()) CameraToggleAction = CamTact.Object;
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
            MatchSession->OnFireResult.AddDynamic(this, &ASightlineCharacter::OnFireResult);   // M5：三分流（本地角色处理全房间开火结果）
            GetWorldTimerManager().SetTimer(ReportTimer, this,
                &ASightlineCharacter::SampleLocalState, 1.0f / 30.0f, true);
        }
        if (Camera) {
            // D5-15v3：显隐不再翻转（Lyra 同款——FPP 身体/枪在相机后方自然入镜，不藏任何东西）；
            // OnCameraModeChanged 委托保留在相机组件上，供后续系统（HUD 模式自适应等）订阅
        }
        if (MatchSession->IsInMatch())
            CreateMatchUI();                             // 晚于开局入局（切图后生成）也补建对局 UI
        else
            MatchSession->OnMatchStarted.AddDynamic(this, &ASightlineCharacter::OnMatchStartedHandler);
        // D5-16：枪口火光组件自动发现 + 系统常驻激活（触发由每发 User.Trigger 参数驱动）
        MuzzleFXComp = FindComponentByClass<UNiagaraComponent>();
        if (MuzzleFXComp) MuzzleFXComp->Activate();
        else UE_LOG(LogTemp, Log, TEXT("[开火] 枪口火光组件未找到（特效静默降级；BP 挂 Niagara 组件后重启生效）"));
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
    if (FireAction) {
        Input->BindAction(FireAction, ETriggerEvent::Started, this, &ASightlineCharacter::OnFirePressed);
        Input->BindAction(FireAction, ETriggerEvent::Completed, this, &ASightlineCharacter::OnFireReleased);
    }
    if (CameraToggleAction) {
        Input->BindAction(CameraToggleAction, ETriggerEvent::Started, this, &ASightlineCharacter::OnCameraToggle);
    }
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
        // D5-16v2：Trigger=电平驱动（对局中+按住且有弹=真；登录/大厅/松开/空匣/换弹=假）——
        // 生成节奏由 NS 的 NM_WeaponTrigger（TriggerSpacing）负责；这里只表达"想开火"的电平
        const uint64 NowMs = static_cast<uint64>(FPlatformTime::Seconds() * 1000.0);
        const bool bMatchLive = MatchSession.IsValid() && MatchSession->IsInMatch();   // 对局门：与 TryFire 同源
        if (LocalMag == 0 && NowMs >= LocalReloadUntilMs)
            LocalMag = kClientMag;                   // 到期补满（换弹钟在 TryFire 空匣时上弦——
                                                     // 补弹若只住 TryFire 会死锁：bTrigger 要 LocalMag>0，
                                                     // TryFire 又只有 bTrigger 才被调=按住打空即永久哑火）
        const bool bTrigger = bMatchLive && bFireHeld && LocalMag > 0 && NowMs >= LocalReloadUntilMs;
        if (MuzzleFXComp) {
            if (bTrigger && !MuzzleFXComp->IsActive())
                MuzzleFXComp->Activate(true);        // 系统自完成后复活（寿命型 Niagara 资产兜底）
            MuzzleFXComp->SetVariableBool(FName("User.Trigger"), bTrigger);
            MuzzleFXComp->SetVariableVec3(FName("User.Direction"), GetBaseAimRotation().Vector());
        }
        if (bTrigger) TryFire();   // 节流/蒙太奇/枪声/20 号全在 TryFire（单源）
    }
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
        SmoothedRemoteVel = FVector::ZeroVector;
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
        // 发端上报间隔实测 20~46ms 抖（双 PIE 时间片量化 World 定时器）——单段估计误差比
        // 33.3/间隔 ∈[0.6,1.4]，匀速跑也会出 450/279/67 乱序=BS 轴直立↔俯冲横跳。
        // 非对称低通：升速慢 α0.1（滤抖动）/降速快 α0.25（停步 250ms 落 Idle——
        // 均匀 α 的指数尾巴会让跑动动画停后拖 1.4s，实测"停了还跑几秒"的根源）。
        const float VelAlpha = (SegVel.Size2D() < SmoothedRemoteVel.Size2D()) ? 0.25f : 0.1f;
        SmoothedRemoteVel = FMath::Lerp(SmoothedRemoteVel, SegVel, VelAlpha);
        if (SmoothedRemoteVel.Size2D() < 15.f) SmoothedRemoteVel = FVector::ZeroVector;   // 底部斩断：低速尾巴无物理意义
        RemoteVelocity = SmoothedRemoteVel.GetClampedToMaxSize(420.f);   // 钳到 MaxWalkSpeed——BS 顶格不被误触
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
}

// ===== M5 开火链路（D5 系列；蓝图方案 §0 P1/P3/P5 + M5-C/4.5）=====
void ASightlineCharacter::OnFirePressed() {
    bFireHeld = true;
    TryFire();                                       // 点按=同一通路的第一发
}

void ASightlineCharacter::OnFireReleased() {
    bFireHeld = false;
}

void ASightlineCharacter::TryFire() {
    // 对局门（2026-10-29 用户报：登录页点击出枪声）：未进对局武器系统休眠——客户端镜像服务器
    // 四道门的第一道（在局）；登录页/大厅/断链时点击不再出枪声/蒙太奇/扣弹
    if (!MatchSession.IsValid() || !MatchSession->IsInMatch()) return;
    const uint64 NowMs = static_cast<uint64>(FPlatformTime::Seconds() * 1000.0);
    if (NowMs < NextFireAllowedMs) return;           // 本地节流（表现层预测先行一拍）
    if (LocalMag <= 0) {
        if (NowMs < LocalReloadUntilMs) return;      // 换弹中（镜像服务器状态机，D5-13 热区拆账：无 UI 不纠偏）
        LocalMag = kClientMag;                       // 到期补满
    }
    NextFireAllowedMs = NowMs + kClientFireIntervalMs;
    --LocalMag;
    if (LocalMag == 0)
        LocalReloadUntilMs = NowMs + kClientReloadMs;   // 空匣即上换弹钟（镜像服务器 2.5s；Tick 到期补满）
    UE_LOG(LogSightline, Log, TEXT("[开火] 弹余=%d 姿态 yaw=%.1f pitch=%.1f"),
           LocalMag, GetBaseAimRotation().Yaw, GetBaseAimRotation().Pitch);
    if (FireMontage) PlayAnimMontage(FireMontage);   // AM_MM_Rifle_Fire（路线 A 原包直填）
    if (FireSound != nullptr) UGameplayStatics::PlaySoundAtLocation(this, FireSound.Get(), GetActorLocation());
    if (MatchSession.IsValid()) MatchSession->SendFireRequest();   // D5-01：空 payload，方向都不带
}

void ASightlineCharacter::OnFireResult(const FSightlineFireResult& Result) {
    // 三分流（图=代码 fire_client_flow）：命中音/他人枪声/受击音 + 远端开火动画（D5-10v2）
    if (!Result.bHit) SpawnImpactDecal(Result.HitPoint, Result.ShooterPid);   // 未命中=墙面弹孔（命中人体不出——血效 ep14）
    const bool bSelfShot = MatchSession.IsValid() && MatchSession->IsSelfPlayer(Result.ShooterPid);
    const bool bSelfVictim = MatchSession.IsValid() && MatchSession->IsSelfPlayer(Result.VictimPid);
    UE_LOG(LogSightline, Log, TEXT("[开火] 判定回包 射手=%d 命中=%d 受害=%d 余血=%d 自机射手=%d 自机受击=%d"),
           Result.ShooterPid, Result.bHit ? 1 : 0, Result.VictimPid, Result.VictimHp,
           bSelfShot ? 1 : 0, bSelfVictim ? 1 : 0);   // 自机双旗外显：全 0=SelfPlayerId 未注入（登录链问题）；三分流的判定依据一屏可见
    if (bSelfShot) {
        if (Result.bHit && HitSound != nullptr) UGameplayStatics::PlaySound2D(this, HitSound.Get());
    } else {
        if (RemoteFireSound != nullptr) UGameplayStatics::PlaySound2D(this, RemoteFireSound.Get());   // 2D 缩小版（ep14 空间化）
        if (FireMontage) {
            if (const USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance())) {
                if (USightlinePawnSpawner* Spawner = GI->GetPawnSpawner()) {
                    if (ASightlineCharacter* Remote = Cast<ASightlineCharacter>(Spawner->GetPawnByPid(Result.ShooterPid)))
                        Remote->PlayAnimMontage(FireMontage);   // 远端开火动画（否则枪线凭空出现）
                }
            }
        }
    }
    if (bSelfVictim && Result.bHit && HurtSound != nullptr) UGameplayStatics::PlaySound2D(this, HurtSound.Get());
    if (bSelfShot && Result.bHit) ShowHitmarker();       // D5-16：Hitmarker 池化显示（全 C++）
    if (bSelfVictim && Result.bHit && !HurtFlashWidget.Get()) {
        static bool bWarned = false;
        if (!bWarned) { bWarned = true; UE_LOG(LogSightline, Warning, TEXT("[受击] 红闪未显示：HurtFlashWidget 为空（WBP_HurtFlash 未建或 HurtFlashClass 未填 BP）")); }
    }
    if (bSelfVictim && Result.bHit && HurtFlashWidget.Get()) {
        HurtFlashWidget->SetVisibility(ESlateVisibility::Visible);   // 红闪（常驻实例重播）
        GetWorldTimerManager().SetTimer(HurtFlashTimer, [Weak = TWeakObjectPtr<UUserWidget>(HurtFlashWidget.Get())]() {
            if (Weak.IsValid()) Weak->SetVisibility(ESlateVisibility::Collapsed);
        }, 0.3f, false);
    }
}

void ASightlineCharacter::SrvBakeOccluders() {
    // M5-F 步骤 6.2：控制台入口（PIE 控制台输入 SrvBakeOccluders；编辑器工具按钮走 BakeLib 同函数）
    FString Summary;
    const bool bOk = USightlineBakeLib::BakeServerOccluders(GetWorld(), FString(), Summary);
    UE_LOG(LogTemp, Log, TEXT("[烘焙] %s %s"), bOk ? TEXT("OK") : TEXT("FAIL"), *Summary);
}

// ===== M5 视角系统（D5-15v3：Lyra 同款——只切相机位置，不藏任何东西）=====

void ASightlineCharacter::OnCameraToggle() {
    if (Camera) Camera->ToggleCameraMode();          // FPP(眼高) ↔ TPP(越肩)，偏移由组件 Tick 插值混合
}

// 15 号 → 对局 UI（C++ 全自动建准星/红闪；BP_OnMatchStarted 钩子保留给未来 BP 侧自定义）
void ASightlineCharacter::OnMatchStartedHandler() {
    BP_OnMatchStarted();                                 // BP 钩子（未实现=no-op）
    CreateMatchUI();
}

void ASightlineCharacter::CreateMatchUI() {
    if (CrosshairClass) {
        CrosshairWidget = CreateWidget<UUserWidget>(GetWorld(), CrosshairClass);
        if (CrosshairWidget) CrosshairWidget->AddToViewport(40);
    }
    if (HurtFlashClass) {
        HurtFlashWidget = CreateWidget<UUserWidget>(GetWorld(), HurtFlashClass);
        if (HurtFlashWidget) {
            HurtFlashWidget->AddToViewport(41);
            HurtFlashWidget->SetVisibility(ESlateVisibility::Collapsed);   // 红闪平时隐藏
        }
    }
    UE_LOG(LogTemp, Log, TEXT("[对局UI] 准星=%s 红闪=%s"),
           CrosshairWidget ? TEXT("OK") : TEXT("未配置"),
           HurtFlashWidget ? TEXT("OK") : TEXT("未配置"));
}

// Hitmarker：池 Acquire → 显示 → 0.15s 归池（D5-13 池纪律：Acquire 必配 Release）。
// 池根因已修（Idle 挂 UPROPERTY=GC 可见；跨图条目双侧弃用）——池化两炸史见对象池头文件。
// 单活跃纪律：连发间隔 100ms < 归池时长 150ms——活跃中再次 Acquire 会让上一枚的归池定时器
// 被同名 Handle 覆盖（控件泄漏在视口且永不回池）。活跃中只续时，不重复取件。
void ASightlineCharacter::ShowHitmarker() {
    auto* Subsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<USightlineObjectPoolSubsystem>() : nullptr;
    if (!Subsystem || !HitmarkerClass) {
        static bool bWarned = false;
        if (!bWarned) { bWarned = true; UE_LOG(LogSightline, Warning, TEXT("[命中] Hitmarker 未显示：HitmarkerClass 未配置（BP Character Class Defaults → Sightline|UI）")); }
        return;
    }
    UUserWidget* M = ActiveHitmarker.Get();
    if (!M) {
        M = Cast<UUserWidget>(Subsystem->Acquire(HitmarkerClass));
        ActiveHitmarker = M;
    }
    if (!M) {
        static bool bWarned2 = false;
        if (!bWarned2) { bWarned2 = true; UE_LOG(LogSightline, Warning, TEXT("[命中] Hitmarker 池取件失败（检查对象池/类引用）")); }
        return;
    }
    M->AddToViewport(42);
    M->SetVisibility(ESlateVisibility::Visible);
    TWeakObjectPtr<UUserWidget> Weak(M);
    TWeakObjectPtr<ASightlineCharacter> WeakSelf(this);
    GetWorldTimerManager().SetTimer(HitmarkerTimer, [Weak, WeakSelf]() {
        if (Weak.IsValid() && WeakSelf.IsValid()) WeakSelf->ReleaseHitmarker(Weak.Get());
    }, 0.25f, false);                                // 0.15s 单发闪现太短（用户反馈"没正常显示"因素之一），0.25s 可感知
}

void ASightlineCharacter::ReleaseHitmarker(UUserWidget* Marker) {
    if (!Marker) return;
    if (ActiveHitmarker.Get() == Marker) ActiveHitmarker = nullptr;   // 池保活下 Weak 恒 Valid——活跃判定用显式解除
    Marker->SetVisibility(ESlateVisibility::Collapsed);
    if (auto* Subsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<USightlineObjectPoolSubsystem>() : nullptr)
        Subsystem->Release(Marker);                      // 归池（池内 Collapsed+出视口）
}

// ===== M5 弹孔贴花（21 号 hitPoint 已带落点；朝向=落点短程回溯探针取墙面法线）=====
// 未命中时 hitPoint=墙面交点或 10km 天端点（fire_judge 合同）。法线不进 21 号契约（22B 定稿
// 零改动）——在落点沿射入方向做 15/5cm 短程探针：墙面点必命中该面（拿 Normal）；天端点探空
// 自动跳过。眼位估计与服务器 RayOriginFromEntity 同式（位置+前向18+高82）——探针方向失真极小。
// ===== M5 弹孔贴花（21 号 hitPoint 已带落点；朝向=落点短程回溯探针取墙面法线）=====
// 未命中时 hitPoint=墙面交点或 10km 天端点（fire_judge 合同）。法线不进 21 号契约（22B 定稿
// 零改动）——在落点沿射入方向做 15/5cm 短程探针：墙面点必命中该面（拿 Normal）；天端点探空
// 自动跳过。眼位估计与服务器 RayOriginFromEntity 同式（位置+前向18+高82）——探针方向失真极小。
// 职责边界（整洁架构）：本函数只做"上下文翻译"——眼位/探针是 Character 独有的相机/Pawn 上下文，
// 解析出 FSightlineDecalRequest 纯数据后交管理器；预算/复用/剔除策略全部在 DecalManagerSubsystem。
void ASightlineCharacter::SpawnImpactDecal(const FVector& HitPoint, const int32 ShooterPid) {
    if (!ImpactDecalMaterial) {
        static bool bWarned = false;
        if (!bWarned) { bWarned = true; UE_LOG(LogSightline, Warning, TEXT("[弹孔] 不生成：ImpactDecalMaterial 未配置（迁移 MI_Decal_Concrete 后填 BP）")); }
        return;
    }
    if (!GetWorld()) return;
    FVector Eye(ForceInitToZero);
    if (MatchSession.IsValid() && MatchSession->IsSelfPlayer(ShooterPid)) {
        Eye = Camera ? Camera->GetComponentLocation() : GetActorLocation() + FVector(0, 0, 82);
    } else if (const USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance())) {
        if (const ASightlineCharacter* Remote = Cast<ASightlineCharacter>(GI->GetPawnSpawner()->GetPawnByPid(ShooterPid))) {
            // 服务器 RayOriginFromEntity 的客户端镜像（FPP 相机合同 8/8/62）——探针方向与服务器射线同源
            const float Rad = FMath::DegreesToRadians(Remote->GetActorRotation().Yaw);
            const float Cy = FMath::Cos(Rad), Sy = FMath::Sin(Rad);
            Eye = Remote->GetActorLocation()
                + FVector(Cy, Sy, 0.f) * 8.f
                + FVector(-Sy, Cy, 0.f) * 8.f
                + FVector(0.f, 0.f, 62.f);
        }
    }
    if (Eye.IsZero()) return;                         // 射手已离场（远端 Pawn 销毁）：无方向源，放弃
    const FVector Incoming = (HitPoint - Eye).GetSafeNormal();
    if (Incoming.IsZero()) return;
    FHitResult HR;
    GetWorld()->LineTraceSingleByChannel(HR, HitPoint - Incoming * kDecalProbeBackCm,
                                         HitPoint + Incoming * kDecalProbeFwdCm, ECC_Visibility);
    if (!HR.bBlockingHit) return;                     // 天端点/无面：不贴

    FSightlineDecalRequest R;                         // 纯数据过界（手册 §7.6A：调用方组装，策略归管理器）
    R.Material = ImpactDecalMaterial;
    R.Location = HitPoint;
    R.Rotation = FRotationMatrix::MakeFromX(HR.Normal).Rotator();
    R.Size = FVector(20.f, 24.f, 24.f);               // X=投射深度（跨面 ±10cm 双保险）YZ=弹孔直径
    if (auto* Mgr = GetGameInstance() ? GetGameInstance()->GetSubsystem<USightlineDecalManagerSubsystem>() : nullptr)
        if (Mgr->SpawnImpactDecal(R))
            UE_LOG(LogSightline, Log, TEXT("[弹孔] 落点=(%.0f,%.0f,%.0f) 法线=(%.2f,%.2f,%.2f)"),
                   HitPoint.X, HitPoint.Y, HitPoint.Z, HR.Normal.X, HR.Normal.Y, HR.Normal.Z);
}
