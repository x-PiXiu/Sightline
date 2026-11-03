// SightlineCharacter —— 第 3 期客户端对局层角色（M2 段：本地可玩基线）
// 设计依据：docs/episodes/ep03/设计草案.md §5 + ui_wireframe/动画系统图
// 分工铁律：逻辑全 C++（输入/相机/FAnimInputs 聚合），ABP 只读聚合体（蓝图=表现层）
// 关键约束（§2.4）：ABP 不读 GetVelocity/IsFalling——本类替它采集并聚合进 FAnimInputs；
//                  本地角色=输入直读，远端角色(M3)=ApplyState 网络喂——同一 ABP 两种数据源
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "Camera/SightlineCameraComponent.h"
#include "Match/SightlineMatchSession.h"
#include "SightlineCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class UAnimMontage;
class UNiagaraComponent;
class USoundBase;
class UUserWidget;
class USightlineObjectPoolSubsystem;
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

    // —— ep06 表现层：左手 IK 吸附（动画定风格，IK 修末端——消除素材姿势与枪的毫米级偏差穿模）——
    /** 左手 IK 目标：枪上左手护握 socket 的【角色 mesh 组件空间】变换（TwoBoneIK Target Space=Component 直喂；
     *  枪挂在手 socket 下有层级差，C++ 已做 世界→角色组件空间 换算） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    FTransform LH_IK_Target = FTransform::Identity;

    /** 左手 IK 权重：持枪=1；换弹/拔枪蒙太奇播放中=0（蒙太奇里左手要离开枪做动作，IK 必须让位） */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    float LH_IK_Weight = 0.f;

    /** 肘部引导点（组件空间）：Joint Target=(0,0,0) 会把弯曲平面引向脚下→肘部从背后绕（实锤）；
     *  动态=目标位置+左下偏移，肘弯向身体左前下=持枪自然姿态 */
    UPROPERTY(BlueprintReadOnly, Category = "Sightline|Anim")
    FVector LH_IK_Joint = FVector::ZeroVector;
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

    // —— M5 视角 UI（D5-16：对局 UI 挂载/开火反馈由 C++ 派发，BP 只做表现——零 MatchSession 访问）——
    UFUNCTION()
    void OnMatchStartedHandler();                    // 15 号 → BP_OnMatchStarted
    UFUNCTION(BlueprintImplementableEvent, Category = "Sightline|Fire")
    void BP_OnMatchStarted();                        // BP 实现：创建准星/红闪挂进视口
    UFUNCTION(BlueprintImplementableEvent, Category = "Sightline|Fire")
    void BP_OnFireFeedback(bool bSelfShot, bool bHit, bool bSelfVictim, int32 VictimPid, int32 VictimHp);
                                                     // BP 实现：Hitmarker（bSelfShot&&bHit）/红闪（bSelfVictim）

    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<USightlineCameraComponent> Camera;

    UPROPERTY(VisibleAnywhere, Category = "Sightline|Camera")
    TObjectPtr<USkeletalMeshComponent> ShadowMesh;

    /** mesh 相对胶囊的偏航（模型原生朝 +Y；-90=对齐 +X）。
     *  向心收枪口用负向加大（-100=向左 10°，-80=向右 10°）。
     *  ⚠ BP 级旋转=本机与远端同转，远端眼里的你同样斜；终稿向心斜置应走 1P viewmodel */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Camera", meta = (ClampMin = "-180", ClampMax = "180"))
    float MeshYawOffset = -80.f;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputMappingContext> DefaultMappingContext;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> MoveAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> LookAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> JumpAction;

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> FireAction;                 // M5：IA_Fire（Digital·Down——按住全自动走 C++ 节流循环，D5-14）

    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Input")
    TObjectPtr<UInputAction> CameraToggleAction;         // M5：IA_Camera（V 键·TPP/FPP 切换，D5-15）

    // —— M5 开火资产（资产引用分层：软引用=可能缺席静默降级；蓝图 4.5 填入，C++ 零资产路径红线）——
    /** 开火蒙太奇（AM_MM_Rifle_Fire 原包直填——蓝图 3.2 路线 A 定稿；远端开火动画同款复用） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<UAnimMontage> FireMontage;

    // —— ep06 表现层：换弹（本地动作+音效；换弹钟 2.5s 是权威，蒙太奇只是表现层随行）——
    /** 换弹蒙太奇（AM_MM_Rifle_Reload——与 Fire 同族同槽声明；打空上弦瞬间播，钟到期补满） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<UAnimMontage> ReloadMontage;
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<USoundBase> ReloadSound;          // 换弹音（Lyra Rifle_Load01）

    /** 拔枪蒙太奇（AM_MM_Rifle_Equip——对局出生播一次，入场仪式感；大厅不播） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<UAnimMontage> EquipMontage;
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<UAnimMontage> DeathMontage;      // ep07：死亡蒙太奇（AM_MM_Death_Front_01·Slot FullBody）

    // —— ep06 表现层：左手 IK 吸附（枪上左手护握 socket 为目标；ABP TwoBoneIK 消费 LH_IK_*）——
    /** 枪网格组件引用（BP Character Class Defaults 填——左手 IK 目标的宿主；换枪=换引用） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Anim")
    TObjectPtr<USceneComponent> WeaponMeshComp;
    /** 左手护握 socket 名（枪 mesh 上自建——位置=护木下沿；换枪统一命名则代码零改动） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Anim")
    FName LeftHandSocketName = TEXT("HandGuard_L");

    virtual void PossessedBy(AController* NewController) override;   // ep06：俯仰钳 ±60（AO 素材范围）

    // —— M5 视角（D5-15v3 终稿：Lyra 同款——FPP=眼高相机+3P 姿势借入，不藏身体不建 1P 枪）——
    // 沿革：双枪模（悬浮枪，用户否）→ FPP 藏身体（背离 Lyra 实现作废）→ 终稿=纯相机位置差异。
    // Lyra 实勘（完整工程）：CM_ThirdPerson 是唯一相机资产，无 FPP 相机模式类、无 1P 手臂网格。

    // 四音=硬引用（TObjectPtr）：核心反馈资产必须常驻——曾用 TSoftObjectPtr 软引用不持资产，
    // 首次 GC 即卸载 → IsValid() 转假 → 开火中途永久静默（2026-10-28 实锤）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<USoundBase> FireSound;            // 射手自己：本地即时（表现层预测零延迟）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<USoundBase> HitSound;             // 命中音：21 号 shooter=自己
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<USoundBase> HurtSound;            // 受击音：21 号 victim=自己
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<USoundBase> RemoteFireSound;      // 他人枪声：21 号 shooter≠自己（2D 缩小版）

    // —— M5 对局 UI（D5-16：全 C++ 驱动——BP 侧只在 Class Defaults 填 3 个类引用，零事件图）——
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> CrosshairClass;     // 准星（WBP_Crosshair）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> HurtFlashClass;     // 红闪（WBP_HurtFlash）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> HitmarkerClass;     // Hitmarker（WBP_Hitmarker·对象池管理）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> CombatHudClass;      // 对局 HUD（血条=首个控件·后续弹药/枪械名同面板扩·根类 UCombatHudWidgetBase·WBP_CombatHud）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> KillFeedClass;       // 击杀播报（根类 UKillFeedWidgetBase·WBP_KillFeed）
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|UI")
    TSubclassOf<UUserWidget> ResultClass;         // 结算面板（根类 UResultWidgetBase·WBP_Result）

    // —— M5 枪口火光（D5-16v2：NS 为参数驱动——每发写 User.Trigger 上升沿 + User.Direction；
    //     组件由 BP 挂在 WeaponRifle 的 Muzzle 插槽下，C++ BeginPlay 按类自动发现；
    //     组件缺失时=nullptr 静默降级）——
    UPROPERTY(VisibleAnywhere, Category = "Sightline|Fire")
    TObjectPtr<UNiagaraComponent> MuzzleFXComp = nullptr;

    // —— M5 弹孔贴花（2026-10-28 用户令：21 号 hitPoint 已带落点，C++ 补朝向与渲染）——
    /** 弹孔贴花材质（Lyra 迁移 M_Decal_Base 系；未配置=静默降级零开销） */
    UPROPERTY(EditDefaultsOnly, Category = "Sightline|Fire")
    TObjectPtr<UMaterialInterface> ImpactDecalMaterial = nullptr;

    UFUNCTION(Exec) void SrvBakeOccluders();     // 控制台烘焙入口（M5-F 步骤 6.2）

private:
    void Move(const FInputActionValue& Value);
    void Look(const FInputActionValue& Value);
    void SampleLocalState();
    UFUNCTION() void OnSelfCorrected(FVector Error, bool bFullSnap, bool bInstant);

    // —— M5 开火（D5-14：按住=全自动节流连发；本地扣弹=表现层预测，服务器四道门兜底，D5-04 不纠偏）——
    void OnFirePressed();
    void OnFireReleased();
    void TryFire();                              // 本地节流（100ms/30 发/2.5s 镜像服务器常量）+ 蒙太奇 + 枪声 + 20 号
    UFUNCTION() void OnFireResult(const FSightlineFireResult& Result);   // 21 号三分流：命中音/受击音/远端枪声+远端动画
    UFUNCTION() void OnKill(const FSightlineKill& Kill);               // ep07：23 号（自己死亡表现+播报）
    UFUNCTION() void OnRespawn(int32 Pid, int32 SpawnIndex, int32 InvulnMs, FVector SpawnPos);  // ep07：24 号（落位/插值重置）
    void OnCameraToggle();                       // D5-15：V 键切 TPP/FPP
    void CreateMatchUI();                        // D5-16：开局建准星+红闪常驻件
    void ShowHitmarker();                        // Hitmarker 池 Acquire→显示→0.25s 隐藏（池根因修复后回归）
    void ReleaseHitmarker(UUserWidget* Marker);
    void SpawnImpactDecal(const FVector& HitPoint, int32 ShooterPid);   // 未命中墙面弹孔（落点短程探针取法线·引擎原生 Spawn）
    void WarmupFireChain();
    void ResetRemoteOnRespawn(FVector SpawnPos);   // ep07：远端重生（插值重置+幽灵环清+落位——❗10）                      // ep06 首枪预热：对局开始后静音热一遍开火链（火光/枪声/弹孔首次加载）
    bool bFireHeld = false;

    // —— 和解尾款（2026-11-04 拉扯第五轮：纠正施加从"30Hz 硬瞬移"改"小额误差逐帧平滑归还"）——
    /** 未还完的对账误差向量：OnSelfCorrected 小额分支入账，Tick 按指数衰减归还（kCorrectionRate）；
     *  FullSnap/瞬发/死亡定身/重生传送时清零。平滑期上报的就是混合位→服务器采纳→对账归零，无反馈环 */
    FVector PendingCorrection = FVector::ZeroVector;

    // —— M5 对局 UI 实例（C++ 全自动创建；离场随 Pawn 销毁）——
    UPROPERTY() TObjectPtr<UUserWidget> CrosshairWidget = nullptr;
    UPROPERTY() TObjectPtr<UUserWidget> HurtFlashWidget = nullptr;
    UPROPERTY() TObjectPtr<UUserWidget> CombatHudWidget = nullptr;   // ep07：血条
    UPROPERTY() TObjectPtr<UUserWidget> KillFeedWidget = nullptr;    // ep07：击杀播报
    UPROPERTY() TObjectPtr<UUserWidget> ResultWidget = nullptr;      // ep07：结算面板
    FTimerHandle HitmarkerTimer;
    TWeakObjectPtr<UUserWidget> ActiveHitmarker;   // 单活跃 Hitmarker（连发 100ms<归池 150ms：重复取件=定时器覆盖泄漏）
    FTimerHandle HurtFlashTimer;
    std::uint64_t NextFireAllowedMs = 0;         // 客户端步进 100ms（服务器 95ms 容差吸抖动）
    int32 LocalMag = 30;                         // 本地弹匣（不上 UI——D5-04；漂了不纠偏，换弹自动对齐）
    std::uint64_t LocalReloadUntilMs = 0;
    float LastReportedYaw = 0.f;                 // 诊断镜像：最近一次 19 号上报的姿态——与弹余行的准星
    float LastReportedPitch = 0.f;               // 姿态并排打印，"上报值 vs 准星值"恒差一眼定案

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
    FVector SmoothedRemoteVel = FVector::ZeroVector; // 段速度低通态（发端间隔抖动过滤，τ≈160ms）

    // —— ep06 幽灵：权威位历史环（客户端推演服务器判定世界）——
    // ApplyState 每快照 push（30Hz×64 槽 ≈ 2.1s 窗 > 回溯上限 1s）；SampleAuthorityAt 线性插值
    // 查询"now−rtt"时刻位=服务器 judgeAt 查的历史位（客户端钟近似，同 ep05 全链 RTT 单钟口径）
    struct FAuthoritySample { double TimeSec = 0.0; FVector Pos = FVector::ZeroVector; };
    static constexpr int32 kAuthorityCap = 64;
    FAuthoritySample AuthorityHist[kAuthorityCap];
    int32 AuthCount = 0, AuthHead = 0;

    /** 查询 T 时刻的权威位（环内线性插值；空环/早于最老=最老位/晚于最新=最新位）——幽灵橙球数据源 */
    FVector SampleAuthorityAt(double TimeSec) const;
};
