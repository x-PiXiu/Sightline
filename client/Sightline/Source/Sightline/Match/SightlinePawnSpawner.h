// SightlinePawnSpawner —— 对局实体生成/销毁适配器（谦卑对象对面的"效果层"，D3-16）
// 整洁架构位置：订阅 Registry（用例侧纯态）的委托 → 执行 UWorld 效果（Spawn/Destroy）
// 效果与决策分离：Registry 管状态演进（可纯单测），本类只把状态变成世界里的 Pawn
// 生成规则：bSelf 跳过（本地 Pawn 由 GameMode 生成）· 出生点 = Spawn_{序号+1}（v3 契约规则 7）
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Match/SightlineEntityRegistry.h"
#include "SightlinePawnSpawner.generated.h"

class USightlineEntityRegistry;

UCLASS()
class SIGHTLINE_API USightlinePawnSpawner : public UObject {
    GENERATED_BODY()

public:
    void Init(USightlineEntityRegistry* InRegistry, TSubclassOf<APawn> InRemoteClass);
    void BindDelegates();                                   // 订阅 Registry 委托（GI 在 Init 后调用）

    /** 对局关卡就绪后由 GI 调用：批量生成在局远端角色（远端类槽为空则静默跳过） */
    void SpawnAllRemote(UWorld* World, TSubclassOf<APawn> InRemoteClass);

    /** 退房/回大厅世界：全部销毁并清账 */
    void DespawnAll();

    int32 SpawnedCount() const;

    UFUNCTION()
    void HandleEntityUpdated(int32 PlayerId);               // 17：直设位置 + 喂 FAnimInputs（M4）

    UFUNCTION()
    void HandleEntityLeft(int32 PlayerId);                  // 18：销毁对端角色

private:
    UPROPERTY()
    TObjectPtr<USightlineEntityRegistry> Registry;

    UPROPERTY()
    TSubclassOf<APawn> RemoteClass;

    TMap<int32, TWeakObjectPtr<APawn>> Live;                // PlayerId → 已生成 Pawn（弱引用）
};
