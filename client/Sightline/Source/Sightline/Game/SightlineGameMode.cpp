// SightlineGameMode 实现 —— M2 段壳：默认角色指向 SightlineCharacter，皮肤槽留蓝图子类填
#include "Game/SightlineGameMode.h"
#include "Character/SightlineCharacter.h"
#include "GameInstance/SightlineGameInstance.h"
#include "Match/SightlineEntityRegistry.h"
#include "Match/SightlinePawnSpawner.h"
#include "GameFramework/PlayerStart.h"

ASightlineGameMode::ASightlineGameMode() {
    // M2 未建 BP 皮肤时也能 PIE（回落 C++ 默认角色）；M3 起蓝图子类覆盖 DefaultPawnClass = BP_Character_Manny
    DefaultPawnClass = ASightlineCharacter::StaticClass();
}

AActor* ASightlineGameMode::ChoosePlayerStart_Implementation(AController* Player) {
    // P1 出生点=名册序（本地与远端同规）：MATCH_START 解析进 Registry 在先、切图在后，
    // 本钩子跑在新世界 Pawn 生成时——自机名册序已可查
    if (const USightlineGameInstance* GI = Cast<USightlineGameInstance>(GetGameInstance())) {
        if (const USightlineEntityRegistry* Reg = GI->GetEntityRegistry()) {
            for (const FSightlineEntityState& E : Reg->All()) {
                if (!E.bSelf) continue;
                if (APlayerStart* Start = USightlinePawnSpawner::FindSpawnStart(GetWorld(), E.SpawnIndex)) {
                    UE_LOG(LogTemp, Log, TEXT("[GameMode] 本地角色出生点=名册序 %s（pid=%d）"), *Start->GetName(), E.PlayerId);
                    return Start;
                }
                UE_LOG(LogTemp, Warning, TEXT("[GameMode] 自机名册点 Spawn_%d 未找到——回落引擎默认"), E.SpawnIndex + 1);
                break;
            }
        }
    }
    return Super::ChoosePlayerStart_Implementation(Player);   // 大厅阶段/名册未到：维持引擎默认
}
