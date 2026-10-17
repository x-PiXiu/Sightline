// SightlineEntityRegistry 实现 —— 纯态演进 + 委托广播（零 UWorld，单测友好）
#include "Match/SightlineEntityRegistry.h"

void USightlineEntityRegistry::ApplyMatchStart(int32 SelfPlayerId, const TArray<FSightlineEntityState>& Roster) {
    Clear();
    for (const FSightlineEntityState& E : Roster) {
        FSightlineEntityState S = E;
        S.bSelf = (S.PlayerId == SelfPlayerId);            // 自己在名册中——本地 Pawn 由 GameMode 生成
        Entities.Add(S.PlayerId, S);
        OnEntitySpawned.Broadcast(S.PlayerId);
    }
}

void USightlineEntityRegistry::ApplyState(int32 PlayerId, const FVector& Pos, float Yaw, float Pitch, bool bFalling) {
    FSightlineEntityState* E = Entities.Find(PlayerId);
    if (!E) return;                                        // 未知实体（未进对局）静默忽略
    E->Pos = Pos;
    E->Yaw = Yaw;
    E->Pitch = Pitch;
    E->bFalling = bFalling;
    OnEntityUpdated.Broadcast(PlayerId);
}

void USightlineEntityRegistry::ApplyLeave(int32 PlayerId) {
    if (Entities.Remove(PlayerId) == 0) return;
    OnEntityLeft.Broadcast(PlayerId);
}

bool USightlineEntityRegistry::GetEntity(int32 PlayerId, FSightlineEntityState& Out) const {
    if (const FSightlineEntityState* E = Entities.Find(PlayerId)) { Out = *E; return true; }
    return false;
}

TArray<FSightlineEntityState> USightlineEntityRegistry::All() const {
    TArray<FSightlineEntityState> Out;
    Entities.GenerateValueArray(Out);
    return Out;
}

void USightlineEntityRegistry::Clear() {
    for (const auto& It : Entities) OnEntityLeft.Broadcast(It.Key);
    Entities.Empty();
}
