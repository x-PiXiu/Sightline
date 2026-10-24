#include "Game/SightlineOccluderComponent.h"

USightlineOccluderComponent::USightlineOccluderComponent() {
    // 运行时零逻辑（D5-08v2）：不碰撞、不渲染、不 tick——纯标记。PrimaryComponentTick 默认已关。
    SetAutoActivate(false);
}
