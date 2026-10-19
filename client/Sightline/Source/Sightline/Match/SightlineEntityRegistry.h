// SightlineEntityRegistry —— 对局实体注册表（客户端用例层·纯态 + 委托）
// D3-16 谦卑对象：决策（名册/快照/离场的状态演进）在这里，效果（Spawn/Destroy/摆姿势）
//               在适配器 PawnSpawner——本类零 UWorld 依赖，可脱离引擎纯单测
// 数据源：UMatchSession 收帧解析后喂入（15 名册 / 17 快照 / 18 离场）
#pragma once
#include "CoreMinimal.h"
#include "SightlineEntityRegistry.generated.h"

/** 对局实体状态（本地与远端同构——bSelf 区分数据源，动画同构见 FAnimInputs 设计） */
USTRUCT(BlueprintType)
struct FSightlineEntityState {
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) int32 PlayerId = 0;
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) int32 SpawnIndex = 0;      // 出生点序号（v3 契约：名册序）
    UPROPERTY(BlueprintReadOnly) FVector Pos = FVector::ZeroVector;
    UPROPERTY(BlueprintReadOnly) float Yaw = 0.f;
    UPROPERTY(BlueprintReadOnly) float Pitch = 0.f;
    UPROPERTY(BlueprintReadOnly) bool bFalling = false;
    UPROPERTY(BlueprintReadOnly) int32 AckedSeq = 0;      // v4·R11：服务器已确认输入序号（M3 和解对账基准；UHT 不支持 uint32 属性，序号窗口内 int32 等价）
    UPROPERTY(BlueprintReadOnly) bool bSelf = false;        // 自己（本地 Pawn 由 GameMode 生成，不重复生成）
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEntitySpawned, int32, PlayerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEntityUpdated, int32, PlayerId);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEntityLeft, int32, PlayerId);

UCLASS()
class SIGHTLINE_API USightlineEntityRegistry : public UObject {
    GENERATED_BODY()

public:
    UPROPERTY(BlueprintAssignable) FOnEntitySpawned OnEntitySpawned;
    UPROPERTY(BlueprintAssignable) FOnEntityUpdated OnEntityUpdated;
    UPROPERTY(BlueprintAssignable) FOnEntityLeft OnEntityLeft;

    /** 15 名册广播：清场重建（对局重新开始 = 名册重置） */
    void ApplyMatchStart(int32 SelfPlayerId, const TArray<FSightlineEntityState>& Roster);

    /** 17 快照：更新位置/朝向/腾空/ackedSeq → OnEntityUpdated（M4 直发同步的入口；v4 起带对账基准） */
    void ApplyState(int32 PlayerId, const FVector& Pos, float Yaw, float Pitch, bool bFalling, int32 AckedSeq = 0);

    /** 18 离场：删除 → OnEntityLeft（未知 PlayerId 静默忽略） */
    void ApplyLeave(int32 PlayerId);

    bool GetEntity(int32 PlayerId, FSightlineEntityState& Out) const;
    TArray<FSightlineEntityState> All() const;              // 快照遍历（SpawnAll 批量生成用）
    void Clear();                                           // 断线/退房清场

private:
    UPROPERTY()
    TMap<int32, FSightlineEntityState> Entities;
};
