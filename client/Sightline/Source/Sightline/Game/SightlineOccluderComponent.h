// SightlineOccluderComponent —— 第 5 期遮挡标记件（P6 · D5-08v2 编辑器烘焙管道的"生产端标记"）
// 整洁架构位置：适配层标记件——运行时零逻辑（游戏循环一根手指都不碰它），只活在编辑器期
// 被烘焙工具（SightlineBakeLib）读取。组合优于继承：任何 Actor（下载地图的 StaticMesh/BP）挂上
// 本组件 = 参与服务器碰撞；摘掉 = 装饰物。Lyra 素材不入库红线不受影响（组件自身入库，资产不入）。
// 服务器消费形态：烘焙 JSON（server/assets/maps/*.json）→ MapGeometryLoader → 域层 OccluderTable
#pragma once
#include "Components/SceneComponent.h"
#include "SightlineOccluderComponent.generated.h"

UCLASS(ClassGroup = (Sightline), meta = (BlueprintSpawnableComponent),
       DisplayName = "Sightline 遮挡标记（服务器碰撞）")
class SIGHTLINE_API USightlineOccluderComponent : public USceneComponent {
    GENERATED_BODY()

public:
    USightlineOccluderComponent();

    /** 覆盖包围盒：默认关闭——烘焙取 Owner 的组件包围盒（世界 AABB）；
     *  开启后用本地系自定义盒（Center+HalfExtents 随组件变换进合同，支持 yaw 旋转原语） */
    UPROPERTY(EditAnywhere, Category = "Sightline|Occluder")
    bool bOverrideBounds = false;

    /** 自定义盒中心（组件本地系；仅 bOverrideBounds=true 时生效） */
    UPROPERTY(EditAnywhere, Category = "Sightline|Occluder", meta = (EditCondition = "bOverrideBounds"))
    FVector CustomCenter = FVector::ZeroVector;

    /** 自定义盒半尺寸（uu；仅 bOverrideBounds=true 时生效——服务器只收半尺寸正数） */
    UPROPERTY(EditAnywhere, Category = "Sightline|Occluder", meta = (EditCondition = "bOverrideBounds"))
    FVector CustomHalfExtents = FVector(50.f, 50.f, 50.f);
};
