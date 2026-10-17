// SightlineGameMode —— 第 3 期对局 GameMode（M2 段：壳 + 皮肤类槽）
// 设计依据：docs/episodes/ep03/设计草案.md §5 + 蓝图操作方案步骤 15
// 分工：皮肤引用留蓝图层（BP_GameMode_Sightline 子类填 MannyClass/QuinnClass），C++ 只认类不认皮肤；
//       对局生成/销毁逻辑在 M3 由 PawnSpawner 适配器承担，本类只当"对局舞台的规则位"
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SightlineGameMode.generated.h"

UCLASS()
class SIGHTLINE_API ASightlineGameMode : public AGameModeBase {
    GENERATED_BODY()

public:
    ASightlineGameMode();

    /** 自己的皮肤（Blueprint 子类填 BP_Character_Manny；M2 未填时回落 C++ 默认角色） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Sightline|Skins")
    TSubclassOf<APawn> MannyClass;

    /** 远端玩家皮肤（Blueprint 子类填 BP_Character_Quinn；PawnSpawner 生成远端用） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Sightline|Skins")
    TSubclassOf<APawn> QuinnClass;
};
