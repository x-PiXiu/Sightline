#include "Game/SightlineObjectPoolSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Engine/World.h"

UObject* USightlineObjectPoolSubsystem::Acquire(TSubclassOf<UObject> Class) {
    if (!Class) return nullptr;
    UWorld* CurWorld = GetWorld();
    if (FPoolItemList* List = Idle.Find(Class)) {
        while (List->Items.Num() > 0) {
            UObject* Obj = List->Items.Pop();
            if (!IsValid(Obj)) continue;                     // GC 已收（Idle 挂 UPROPERTY 后=安全置空，非悬垂）
            if (CurWorld && Obj->GetWorld() != CurWorld) continue;   // 上局世界产物：弃用（防跨图拖旧世界引用）
            Active.Add(Obj);                             // 借出登记（双还守卫的入账侧）
            return Obj;
        }
    }
    // widget 必须走 CreateWidget（装配 WorldContext/PlayerContext/WidgetTree 后才可安全
    // AddToViewport/SetVisibility）——raw NewObject 的 widget 首次显示侥幸可过，归池复用
    // 即在 UMG 显示路径读空崩溃（PIE 命中链首验 2026-10-28 实锤 0x228 AV）
    if (Class->IsChildOf<UUserWidget>()) {
        if (!CurWorld) return nullptr;
        UObject* Fresh = CreateWidget<UUserWidget>(CurWorld, TSubclassOf<UUserWidget>(Class));
        if (Fresh) Active.Add(Fresh);                    // 借出登记（双还守卫的入账侧；建失败不记账）
        return Fresh;
    }
    UObject* Plain = NewObject<UObject>(this, Class);    // 非 widget 外层=this：池生命周期托管
    Active.Add(Plain);
    return Plain;
}

void USightlineObjectPoolSubsystem::Release(UObject* Obj) {
    if (!Obj) return;
    if (!Active.Remove(Obj)) return;                     // 未借出/重复归还：直接忽略（防同对象入池两份）
    UWorld* CurWorld = GetWorld();
    if (CurWorld && Obj->GetWorld() != CurWorld) return;   // 跨图归还：不回池（随旧世界自然回收，防拖世界）
    if (UUserWidget* Widget = Cast<UUserWidget>(Obj)) {
        Widget->RemoveFromParent();                      // 归还即重置：隐藏（内容状态由取用时重置）
        Widget->SetVisibility(ESlateVisibility::Collapsed);
    } else if (AActor* Actor = Cast<AActor>(Obj)) {
        Actor->SetActorHiddenInGame(true);
        Actor->SetActorEnableCollision(false);
        Actor->SetActorTickEnabled(false);
    }
    FPoolItemList* List = Idle.Find(Obj->GetClass());
    if (!List) List = &Idle.Add(Obj->GetClass());
    if (List->Items.Num() >= kMaxIdlePerClass) return;   // 上限：丢弃引用交 GC（强回收最老=同效，省一次换位）
    List->Items.Push(Obj);
}

void USightlineObjectPoolSubsystem::Prewarm(TSubclassOf<UObject> Class, int32 Count) {
    for (int32 i = 0; i < Count; ++i) {
        if (UObject* Obj = Acquire(Class)) Release(Obj);
    }
}
