// SightlineWeaponDef.h —— ep08 步骤 7（§7.3）：武器数据资产——动画表落地
//
// 表驱动论点（验收门）：表 ≥2 行（Rifle/Pistol），换枪**不改 ABP 一个节点**。
//   ABP 三处直连 Rifle 资产的节点（AO 姿势 / Locomotion 混合空间 / 开火蒙太奇播放）
//   改从本资产查表取值；换枪 = 换 Character 的 WeaponDefs 引用，资产与代码零改动。
//
// 架构裁决（§4.6 防双源漂移）：数值（伤害/弹匣/换弹/射速）**不进本资产**——
//   30 号 per-weapon 参数（MatchSession::RuntimeParams）是唯一真相源；
//   本资产只装**表现资产**（动画集/音效）。文档 §7.2 表格中的"弹匣/换弹/射速"
//   条目按此纪律收编，两处不得同时可写。
//
// 兜底链（资产迁移期红线）：查表结果可空 → Character 侧回退旧直连成员
//   （FireMontage 等）——DA 未建齐/未接线时行为零变化，迁移完再退役旧成员。

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Animation/BlendSpace.h"
#include "Animation/AnimMontage.h"
#include "Sound/SoundBase.h"
#include "SightlineWeaponDef.generated.h"

/** 角色侧动画集（ABP 三处改绑的数据源；一把枪一份，进 USightlineWeaponDefinition） */
USTRUCT(BlueprintType)
struct FSightlineWeaponAnimSet
{
	GENERATED_BODY()

	/** AO 姿势混合空间（ABP 的 BlendSpace Player 动态采样——AO_MM_* 是 AimOffset=UBlendSpace 子类，
	 *  与 LocomotionBS 同型通吃；AimOffset 筛选器节点按资产类型化不可接引脚，故换 Player 方案）。
	 *  空=ABP 直连接线兜底 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AnimSet")
	TObjectPtr<UBlendSpace> IdleAO = nullptr;

	/** 移动混合空间（ABP Locomotion 消费；可空=回退基姿势——§7.3 明文） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AnimSet")
	TObjectPtr<UBlendSpace> LocomotionBS = nullptr;

	/** 开火蒙太奇（本地/远端播放点查表——FireMontage 成员的表驱动替身） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AnimSet")
	TObjectPtr<UAnimMontage> Fire = nullptr;

	/** 换弹蒙太奇（打空上弦/按 R 播；换弹钟是权威，蒙太奇只是表现层随行） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AnimSet")
	TObjectPtr<UAnimMontage> Reload = nullptr;

	/** 拔枪蒙太奇（对局出生播一次） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "AnimSet")
	TObjectPtr<UAnimMontage> Equip = nullptr;
};

/** 武器数据资产：一把枪 = 一个 DA 实例（DA_Weapon_Rifle / DA_Weapon_Pistol） */
UCLASS(BlueprintType)
class SIGHTLINE_API USightlineWeaponDefinition : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:
	/** 武器类型（30 号 R.Type 同域：0=Rifle / 1=Pistol——与 MatchSession::RuntimeParams 下标同键） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef")
	int32 WeaponType = 0;

	/** 角色侧动画集（ABP 三处改绑 + 播放点查表的数据源） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef")
	FSightlineWeaponAnimSet AnimSet;

	/** 枪声（FireSound 成员的表驱动替身；命中/受击音与枪无关，留在 Character） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef|Audio")
	TObjectPtr<USoundBase> FireSound = nullptr;

	/** 换弹音 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef|Audio")
	TObjectPtr<USoundBase> ReloadSound = nullptr;

	// ---- 枪模型（ep08 补口：换枪=换 Character 上枪网格组件的 SkeletalMesh；空=不换=兜底）----
	/** 枪骨骼网格（Rifle=SK_Rifle / Pistol=SK_Pistol；Character 的 WeaponMeshComp 运行时切） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef|Visual")
	TObjectPtr<USkeletalMesh> WeaponMesh = nullptr;

	/** 左手 IK 插槽名（DA 可覆盖——手枪无护木→填 None 让 IK 自动让位） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef|Visual")
	FName LeftHandSocketName = TEXT("HandGuard_L");

	/** 网格相对变换校正（换 mesh 后应用到 WeaponMeshComp——每枪原点约定不同）。
	 *  ⚠ 缩放 0（默认）=未配置→**不覆盖** BP 组件现值（防抹掉 BP 里校好的 -90° Yaw——2026-11-07
	 *  实锤：恒等变换曾无条件重置组件变换→所有枪枪口回到素材原生 +Y）。启用=缩放非 0 按值覆盖；
	 *  配置纪律：**换枪链上每把 DA 都要配**（Rifle=照抄 BP 组件现值，Pistol=校正值）——
	 *  漏配的那把会在往返切枪时保留上一把的校正值=又歪 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "WeaponDef|Visual")
	FTransform MeshRelativeTransform = FTransform(FQuat::Identity, FVector::ZeroVector, FVector::ZeroVector);
};
