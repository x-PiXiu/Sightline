// SightlineDecalManagerSubsystem 实现 —— 策略层：预算/复用/剔除/可观测（设计依据 docs/episodes/ep05/贴花管理器设计.md）
#include "Game/SightlineDecalManagerSubsystem.h"
#include "Components/DecalComponent.h"
#include "Engine/DecalActor.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Sightline.h"

ADecalActor* USightlineDecalManagerSubsystem::SpawnSlot(UWorld* World) {
    FActorSpawnParameters Params;
    Params.ObjectFlags |= RF_Transient;              // 运行时表现物：不入存档
    ADecalActor* D = World->SpawnActor<ADecalActor>(ADecalActor::StaticClass(), FTransform::Identity, Params);
    if (D) {
        D->SetActorHiddenInGame(true);               // 待命形态（显形由 SpawnImpactDecal 驱动）
        // ⚠ ADecalActor 出厂自带 bDestroyOwnerAfterFade=true（DecalActor.cpp 构造显式置位）——
        // 淡出完成即销毁宿主 Actor=槽位 6s 后逐个自灭、环池退化成动态建删。槽位寿命归管理器：必须关掉。
        if (UDecalComponent* Comp = D->GetDecal())
            Comp->bDestroyOwnerAfterFade = false;
    }
    return D;
}

ADecalActor* USightlineDecalManagerSubsystem::AcquireSlot(UWorld* World, bool& bOutReused) {
    bOutReused = false;
    if (Slots.Num() < kCapacity) {                   // 懒增长期：每发新 Spawn，无开局建件卡顿
        ADecalActor* D = SpawnSlot(World);
        if (D) {
            Slots.Add(D);
            NextSlot = (NextSlot + 1) % kCapacity;   // 指针先行：填满瞬间指向 0=最老槽
        }
        return D;
    }
    // 稳态：环指针即最老槽；槽被销毁/待销毁/切图 GC 置空 → 就地重 Spawn=跨局自愈
    // （IsValid 必查：pending-kill 指针非空但不可用—— Destroy 与 GC 之间的窗口期）
    ADecalActor* D = Slots[NextSlot].Get();
    if (D && IsValid(D)) {
        bOutReused = true;
    } else {
        D = SpawnSlot(World);
        if (D) Slots[NextSlot] = D;
    }
    NextSlot = (NextSlot + 1) % kCapacity;
    return D;
}

void USightlineDecalManagerSubsystem::Reset() {
    if (UWorld* World = GetWorld())
        for (const TObjectPtr<ADecalActor>& D : Slots)
            if (D) D->Destroy();                     // 旧世界槽位清场（防跨对局残影）
    Slots.Empty();
    NextSlot = 0;
    SpawnedCount = 0;
    ReusedCount = 0;
    bLoggedBudgetFull = false;
}

bool USightlineDecalManagerSubsystem::SpawnImpactDecal(const FSightlineDecalRequest& Request) {
    UWorld* World = GetWorld();
    if (!World) return false;
    if (!Request.Material || Request.Location.IsZero()) return false;   // 防御校验：管理器不信任调用方

    // 距离剔除：落点距所有本地玩家都超阈 → 无人可见，豁免生成（生成端节流，企业惯例）
    bool bRelevant = false;
    for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It) {
        const APlayerController* PC = It->Get();
        if (const APawn* P = PC ? PC->GetPawn() : nullptr) {
            if (FVector::DistSquared(P->GetActorLocation(), Request.Location) <= FMath::Square(kMaxRelevantDistance)) {
                bRelevant = true;
                break;
            }
        }
    }
    if (!bRelevant) return false;

    bool bReused = false;
    ADecalActor* D = AcquireSlot(World, bReused);
    if (!D) return false;

    D->SetActorLocationAndRotation(Request.Location, Request.Rotation);
    D->SetActorHiddenInGame(false);
    if (UDecalComponent* Comp = D->GetDecal()) {
        Comp->SetDecalMaterial(Request.Material);
        Comp->DecalSize = Request.Size;
        Comp->FadeStartDelay = kFadeStartDelay;      // 复用槽自带旧淡出态——取用即重置（池纪律）
        Comp->FadeDuration = kFadeDuration;
        Comp->FadeScreenSize = kFadeScreenSize;
    }

    if (bReused) {
        ++ReusedCount;
    } else {
        ++SpawnedCount;
        if (!bLoggedBudgetFull && Slots.Num() >= kCapacity) {
            bLoggedBudgetFull = true;
            UE_LOG(LogSightline, Log, TEXT("[贴花] 预算满（%d）→ LRU 覆盖开始（后续零分配零销毁）"), kCapacity);
        }
    }
    return true;
}
