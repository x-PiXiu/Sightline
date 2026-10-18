// SightlinePawnSpawner 实现 —— 订阅 Registry 委托，把状态变成 Pawn
#include "Match/SightlinePawnSpawner.h"
#include "Character/SightlineCharacter.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerStart.h"

void USightlinePawnSpawner::Init(USightlineEntityRegistry* InRegistry, TSubclassOf<APawn> InRemoteClass) {
    Registry = InRegistry;
    RemoteClass = InRemoteClass;
}

void USightlinePawnSpawner::BindDelegates() {
    if (Registry) {
        Registry->OnEntityUpdated.AddDynamic(this, &USightlinePawnSpawner::HandleEntityUpdated);
        Registry->OnEntityLeft.AddDynamic(this, &USightlinePawnSpawner::HandleEntityLeft);
    }
}

void USightlinePawnSpawner::HandleEntityUpdated(int32 PlayerId) {
    if (!Registry) return;
    FSightlineEntityState E;
    if (!Registry->GetEntity(PlayerId, E)) return;
    if (E.bSelf) return;                                    // 本地 Pawn 不吃网络位姿
    if (TWeakObjectPtr<APawn>* P = Live.Find(PlayerId)) {
        if (ASightlineCharacter* C = Cast<ASightlineCharacter>(P->Get()))
            C->ApplyState(E.Pos, E.Yaw, E.Pitch, E.bFalling);   // 直设位置 + 喂 FAnimInputs（M4）
    }
    // 未生成的快照（SpawnAll 之前到达）——状态已在 Registry，生成时自然带最新
}

void USightlinePawnSpawner::HandleEntityLeft(int32 PlayerId) {
    if (TWeakObjectPtr<APawn>* P = Live.Find(PlayerId)) {
        if (P->IsValid()) (*P)->Destroy();
        Live.Remove(PlayerId);
    }
}

void USightlinePawnSpawner::SpawnAllRemote(UWorld* World, TSubclassOf<APawn> InRemoteClass) {
    if (!World || !Registry) return;
    if (InRemoteClass) RemoteClass = InRemoteClass;         // 关卡切换后 GameMode 类槽可能重新可用
    if (!RemoteClass) {
        UE_LOG(LogTemp, Warning, TEXT("[Spawner] RemoteClass 未配置——远端角色不生成（检查 BP_GameMode_Sightline 的 QuinnClass）"));
        return;
    }
    for (const FSightlineEntityState& E : Registry->All()) {
        if (E.bSelf) continue;                              // 本地 Pawn 由 GameMode 生成
        if (Live.Contains(E.PlayerId) && Live[E.PlayerId].IsValid()) continue;

        // 出生点 = Spawn_{序号+1}（v3 契约规则 7；查找唯一实现=FindSpawnStart，本地/远端共用）
        const FString StartName = FString::Printf(TEXT("Spawn_%d"), E.SpawnIndex + 1);
        APlayerStart* Start = FindSpawnStart(World, E.SpawnIndex);
        const FTransform Xf = Start ? Start->GetActorTransform()
                                    : FTransform(FRotator::ZeroRotator, FVector(0.f, 0.f, 150.f));
        if (!Start) UE_LOG(LogTemp, Warning, TEXT("[Spawner] 未找到出生点 %s——回退场心"), *StartName);

        if (APawn* P = World->SpawnActor<APawn>(RemoteClass, Xf)) {
            Live.Add(E.PlayerId, P);
            UE_LOG(LogTemp, Log, TEXT("[Spawner] 生成远端角色 pid=%d name=%s @%s"),
                   E.PlayerId, *E.Name, *StartName);
        }
    }
}

APlayerStart* USightlinePawnSpawner::FindSpawnStart(UWorld* World, int32 SpawnIndex) {
    if (!World) return nullptr;
    // 三重匹配：内部名 / PlayerStart Tag / 大纲显示名（Label）——大纲里 F2 改的是 Label
    const FString StartName = FString::Printf(TEXT("Spawn_%d"), SpawnIndex + 1);
    for (TActorIterator<APlayerStart> It(World); It; ++It) {
        if (It->GetName() == StartName
            || It->PlayerStartTag == FName(*StartName)
            || It->GetActorLabel() == StartName) {
            return *It;
        }
    }
    return nullptr;
}

void USightlinePawnSpawner::DespawnAll() {
    for (auto& It : Live) {
        if (It.Value.IsValid()) It.Value->Destroy();
    }
    Live.Empty();
}

int32 USightlinePawnSpawner::SpawnedCount() const {
    int32 Count = 0;
    for (const auto& It : Live) if (It.Value.IsValid()) ++Count;
    return Count;
}
