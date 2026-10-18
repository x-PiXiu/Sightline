// SightlineGameMode —— 第 3 期对局 GameMode（M2 段：壳 + 皮肤类槽）
// 设计依据：docs/episodes/ep03/设计草案.md §5 + 蓝图操作方案步骤 15
// 分工：皮肤引用留蓝图层（BP_GameMode_Sightline 子类填 MannyClass/QuinnClass），C++ 只认类不认皮肤；
//       对局生成/销毁逻辑在 M3 由 PawnSpawner 适配器承担，本类只当"对局舞台的规则位"
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SightlineGameMode.generated.h"

class APlayerStart;

UCLASS()
class SIGHTLINE_API ASightlineGameMode : public AGameModeBase {
    GENERATED_BODY()

public:
    ASightlineGameMode();

    /** P1 出生点=名册序：本地角色与远端同规——对局名册已到则按自机名册序拿 Spawn_{self+1}，
     *  大厅阶段（名册为空/点位缺失）回落引擎默认挑选。
     *  （根治：只对远端按名册序、本地靠引擎随便挑 → 引擎挑中的点与远端名册点撞车=人与远端重叠，2026-10-18 实测） */
    virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;   // UE5.7 签名= AActor*

    /** 自己的皮肤（Blueprint 子类填 BP_Character_Manny；M2 未填时回落 C++ 默认角色） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Sightline|Skins")
    TSubclassOf<APawn> MannyClass;

    /** 远端玩家皮肤（Blueprint 子类填 BP_Character_Quinn；PawnSpawner 生成远端用） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Sightline|Skins")
    TSubclassOf<APawn> QuinnClass;
};
