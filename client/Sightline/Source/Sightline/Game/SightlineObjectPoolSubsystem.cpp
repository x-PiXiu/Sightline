#include "Game/SightlineObjectPoolSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Engine/World.h"

UObject* USightlineObjectPoolSubsystem::Acquire(TSubclassOf<UObject> Class) {
    if (!Class) return nullptr;
    if (TArray<TObjectPtr<UObject>>* List = Idle.Find(Class)) {
        while (List->Num() > 0) {
            UObject* Obj = List->Pop().Get();
            if (IsValid(Obj)) return Obj;                // GC 已收的池条目顺路剔除
        }
    }
    // widget 必须走 CreateWidget（装配 WorldContext/PlayerContext/WidgetTree 后才可安全
    // AddToViewport/SetVisibility）——raw NewObject 的 widget 首次显示侥幸可过，归池复用
    // 即在 UMG 显示路径读空崩溃（PIE 命中链首验 2026-10-28 实锤 0x228 AV）
    if (Class->IsChildOf<UUserWidget>()) {
        UWorld* World = GetWorld();
        return World ? CreateWidget<UUserWidget>(World, TSubclassOf<UUserWidget>(Class)) : nullptr;
    }
    return NewObject<UObject>(this, Class);              // 非 widget 外层=this：池生命周期托管
}

void USightlineObjectPoolSubsystem::Release(UObject* Obj) {
    if (!Obj) return;
    if (UUserWidget* Widget = Cast<UUserWidget>(Obj)) {
        Widget->RemoveFromParent();                      // 归还即重置：隐藏（内容状态由取用时重置）
        Widget->SetVisibility(ESlateVisibility::Collapsed);
    } else if (AActor* Actor = Cast<AActor>(Obj)) {
        Actor->SetActorHiddenInGame(true);
        Actor->SetActorEnableCollision(false);
        Actor->SetActorTickEnabled(false);
    }
    TArray<TObjectPtr<UObject>>* List = Idle.Find(Obj->GetClass());
    if (!List) List = &Idle.Add(Obj->GetClass());
    if (List->Num() >= kMaxIdlePerClass) return;         // 上限：丢弃引用交 GC（强回收最老=同效，省一次换位）
    List->Push(Obj);
}

void USightlineObjectPoolSubsystem::Prewarm(TSubclassOf<UObject> Class, int32 Count) {
    for (int32 i = 0; i < Count; ++i) {
        if (UObject* Obj = Acquire(Class)) Release(Obj);
    }
}
