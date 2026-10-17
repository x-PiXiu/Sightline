// SightlineGameMode 实现 —— M2 段壳：默认角色指向 SightlineCharacter，皮肤槽留蓝图子类填
#include "Game/SightlineGameMode.h"
#include "Character/SightlineCharacter.h"

ASightlineGameMode::ASightlineGameMode() {
    // M2 未建 BP 皮肤时也能 PIE（回落 C++ 默认角色）；M3 起蓝图子类覆盖 DefaultPawnClass = BP_Character_Manny
    DefaultPawnClass = ASightlineCharacter::StaticClass();
}
