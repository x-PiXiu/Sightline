// SightlineDecalManagerSubsystem —— 第 5 期贴花管理器（企业级缩小版：固定预算环池+策略层）
// 整洁架构位置：表现层基建（GameInstanceSubsystem，与对象池同族）——域层（MatchSession）对它一无所知：
// 21 号广播的是纯数据，贴花是表现层的自主决策；调用方（Character）只组装请求，预算/复用/剔除/淡出
// 策略全部收归本类（手册 §7.6A：调用方组装数据，策略归策略持有者）。
// 设计文档：docs/episodes/ep05/贴花管理器设计.md（含"组件池为何在 UE 不成立"的引擎源码证据）
// ep15 扩展点：手雷焦痕 = 新的 FSightlineDecalRequest 参数组，本类零改动。
#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SightlineDecalManagerSubsystem.generated.h"

class ADecalActor;
class UMaterialInterface;

/** 贴花请求（纯数据过界——调用方组装，策略收归管理器） */
USTRUCT(BlueprintType)
struct FSightlineDecalRequest {
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) TObjectPtr<UMaterialInterface> Material = nullptr;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Location = FVector::ZeroVector;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FRotator Rotation = FRotator::ZeroRotator;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) FVector Size = FVector(20.f, 24.f, 24.f);   // X=投射深度 YZ=直径
};

UCLASS(DisplayName = "Sightline 贴花管理器")
class SIGHTLINE_API USightlineDecalManagerSubsystem : public UGameInstanceSubsystem {
    GENERATED_BODY()

public:
    /** 对局开始清账（GI HandleMatchStarted 调用）：销毁旧世界槽位+计数归零——新对局=新预算周期 */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Decal")
    void Reset();

    /** 提交贴花请求：预算内懒增长→稳态 LRU 复用；距离剔除/防御校验内建；返回 false=被策略拒收 */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Decal")
    bool SpawnImpactDecal(const FSightlineDecalRequest& Request);

    UFUNCTION(BlueprintPure, Category = "Sightline|Decal") int32 GetSpawnedCount() const { return SpawnedCount; }
    UFUNCTION(BlueprintPure, Category = "Sightline|Decal") int32 GetReusedCount() const { return ReusedCount; }
    UFUNCTION(BlueprintPure, Category = "Sightline|Decal") int32 GetCapacity() const { return kCapacity; }

private:
    ADecalActor* AcquireSlot(UWorld* World, bool& bOutReused);   // 懒增长/稳态复用/跨局自愈的唯一入口
    ADecalActor* SpawnSlot(UWorld* World);                       // 生成一个隐藏槽位（寿命归管理器，永不设 LifeSpan）

    /** 槽位环：UPROPERTY=GC 可见（对象池两炸的红线教训）；切图销毁的槽位指针置空 → 取用时跨局自愈重 Spawn */
    UPROPERTY()
    TArray<TObjectPtr<ADecalActor>> Slots;
    int32 NextSlot = 0;                        // 环形指针=最老槽（均匀 age 的 LRU 等价实现）
    int32 SpawnedCount = 0;                    // 本对局实际 Spawn 次数（增长期递增至 kCapacity）
    int32 ReusedCount = 0;                     // 本对局复用次数（预算满后持续增长=池在工作的直接证据）
    bool bLoggedBudgetFull = false;            // 预算满提示只打一次

    static constexpr int32 kCapacity = 64;                // 预算上界（企业 200~500，地图规模缩小版）
    static constexpr float kMaxRelevantDistance = 8000.f; // 距离剔除：距所有本地玩家超此值不生成（无人可见）
    static constexpr float kFadeStartDelay = 4.f;         // 显形 4s 后开始淡出
    static constexpr float kFadeDuration = 2.f;           // 2s 淡完（视觉寿命 6s）
    static constexpr float kFadeScreenSize = 0.02f;       // 视口剔除阈值（远处小贴花引擎自动隐去；0=永不剔除是反模式）
};
