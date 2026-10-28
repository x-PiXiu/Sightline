// SightlineObjectPoolSubsystem —— 第 5 期对象池基建（P7 · D5-13：建成即用、不作展示重点）
// 整洁架构位置：GameInstanceSubsystem（客户端表现层基建）——域层（MatchSession/Registry）对它一无所知，
// 消费方是表现层（UMG 反馈件/后续 Actor 类）。热区拆账（D5-13）：ep05 首批用户=Hitmarker widget 池
// （600 RPM 并发峰值 2~3）；tracer 走零分配 DrawLine；ep15 手雷投射物=第二用户（规格按它设计）。
// 对照取舍：引擎有 CommonUI 的 UCommonWidgetPool——不引插件自研百行的取舍=避免 CommonUI 全家依赖。
// ⚠ widget 池化两炸存档（2026-10-28）：widget 生命周期（CreateWidget 初始化/归池复用/跨切图弱化）
//   手搓易碎，Hitmarker 已改常驻单实例；本池 ep15 起供 Actor 形态（手雷投射物）使用。
#pragma once
#include "Subsystems/GameInstanceSubsystem.h"
#include "SightlineObjectPoolSubsystem.generated.h"

/** 池条目列表（UHT 不支持容器嵌套容器——TMap 值挂 TArray 须经 USTRUCT 转接） */
USTRUCT()
struct FPoolItemList {
    GENERATED_BODY()

    UPROPERTY()
    TArray<UObject*> Items;
};

UCLASS(DisplayName = "Sightline 对象池")
class SIGHTLINE_API USightlineObjectPoolSubsystem : public UGameInstanceSubsystem {
    GENERATED_BODY()

public:
    /** 取一个闲置实例（空池则新建：widget 走 CreateWidget 带世界上下文，其余 NewObject 外层=池；
     *  池对象"出生即隐藏、归还即重置"）
     *  widget：调用方自行 AddToViewport；actor：调用方自行放置/显示 */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Pool")
    UObject* Acquire(TSubclassOf<UObject> Class);

    /** 归还（隐藏回池，不销毁）：widget → RemoveFromParent；actor → 隐藏+关碰撞+关 tick。
     *  超过上限强回收最老（丢弃引用交给 GC）——Acquire 必配 Release（RAII guard 由调用方保证） */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Pool")
    void Release(UObject* Obj);

    /** 预热（防首帧 NewObject 卡顿）：Hitmarker 池预热 4 / 上限 8（蓝图方案 4.3） */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Pool")
    void Prewarm(TSubclassOf<UObject> Class, int32 Count);

private:
    /** GC 可见性红线：Idle 必须挂 UPROPERTY——池内引用不进反射，归池对象（已出视口/已隐藏）
     *  在全工程无可达引用 → GC 直接回收 → 池发悬垂指针。池化两炸（AV 0x228 / Index>=0 断言）
     *  的共同根因即此缺失 */
    UPROPERTY()
    TMap<TSubclassOf<UObject>, FPoolItemList> Idle;
    UPROPERTY()
    TSet<UObject*> Active;                               // 已借出未归还：双还守卫（二次 Release 直接忽略——
                                                         // 否则同对象入池两份，两个使用者互踢显隐）
    static constexpr int32 kMaxIdlePerClass = 16;        // 上限：超还即弃（防泄漏上限，非硬并发顶）
};
