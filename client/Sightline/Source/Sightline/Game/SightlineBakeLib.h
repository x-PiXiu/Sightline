// SightlineBakeLib —— 第 5 期几何合同烘焙（P6 · D5-08v2：摆放即生效的"自动"就在这一步）
// 职责：扫当前关卡全部 SightlineOccluder 标记件 → 枚举几何（自定义覆盖盒/Owner 世界包围盒）
//      → 写 server/assets/maps/<地图名>.json（几何合同 v1）+ 读角色胶囊真身进 entityHitbox。
// 整洁架构位置：适配器（UE 编辑器侧生产端）——产出物是双端合同文件，服务器 MapGeometryLoader 消费。
// 调用入口：①Editor Utility Widget 按钮（蓝图调 BlueprintCallable）②PIE 控制台 SrvBakeOccluders（Character Exec）
// 边界：只支持竖直盒（yaw 旋转原语，pitch/roll 不支持——D5-08v2 合同边界）；复杂 mesh 取包围盒近似。
#pragma once
#include "Kismet/BlueprintFunctionLibrary.h"
#include "SightlineBakeLib.generated.h"

UCLASS()
class SIGHTLINE_API USightlineBakeLib : public UBlueprintFunctionLibrary {
    GENERATED_BODY()

public:
    /** 烘焙当前关卡（World 传编辑器世界或 PIE 世界）→ 几何合同 JSON。
     *  OutPath 留空 = 默认 ../../server/assets/maps/<LevelName>.json（工程目录相对——两仓同盘直写）。
     *  返回 false 时 OutSummary 带原因（无标记件/写盘失败）。 */
    UFUNCTION(BlueprintCallable, Category = "Sightline|Bake")
    static bool BakeServerOccluders(UWorld* World, const FString& OutPath, FString& OutSummary);
};
