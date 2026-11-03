// SightlineCombatWidgets.cpp —— ep07 战斗 UI 三件实现（数据全走 MatchSession 委托）
#include "UI/SightlineCombatWidgets.h"
#include "Match/SightlineMatchSession.h"
#include "GameInstance/SightlineGameInstance.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"

// ───────── 血条 ─────────
void UCombatHudWidgetBase::BindSession(USightlineMatchSession* InSession)
{
    Session = InSession;
    if (Session.IsValid())
    {
        SelfPid = Session->GetSelfPlayerId();
        Session->OnFireResult.AddDynamic(this, &UCombatHudWidgetBase::HandleFireResult);
        Session->OnRespawn.AddDynamic(this, &UCombatHudWidgetBase::HandleRespawn);
    }
}

void UCombatHudWidgetBase::NativeConstruct()
{
    Super::NativeConstruct();
    SetHp(100, 100);                                   // 初始满血
}

void UCombatHudWidgetBase::NativeDestruct()
{
    if (Session.IsValid())
    {
        Session->OnFireResult.RemoveDynamic(this, &UCombatHudWidgetBase::HandleFireResult);
        Session->OnRespawn.RemoveDynamic(this, &UCombatHudWidgetBase::HandleRespawn);
    }
    Super::NativeDestruct();
}

void UCombatHudWidgetBase::HandleFireResult(const FSightlineFireResult& Result)
{
    if (Session.IsValid() && !Session->IsSelfPlayer(Result.VictimPid)) return;   // 只显示自己血量
    SetHp(Result.VictimHp, 100);
}

void UCombatHudWidgetBase::HandleRespawn(int32 Pid, int32 /*SpawnIndex*/, int32 /*InvulnMs*/, FVector /*SpawnPos*/)
{
    if (Session.IsValid() && Session->IsSelfPlayer(Pid)) SetHp(100, 100);
}

void UCombatHudWidgetBase::SetHp(int32 Hp, int32 MaxHp)
{
    const float Ratio = MaxHp > 0 ? static_cast<float>(Hp) / MaxHp : 0.f;
    if (HealthBar) HealthBar->SetPercent(Ratio);
    if (HealthText) HealthText->SetText(FText::FromString(FString::Printf(TEXT("%d / %d"), Hp, MaxHp)));
}

// ───────── 击杀播报 ─────────
void UKillFeedWidgetBase::BindSession(USightlineMatchSession* InSession)
{
    Session = InSession;
    if (Session.IsValid())
    {
        SelfPid = Session->GetSelfPlayerId();
        Session->OnKill.AddDynamic(this, &UKillFeedWidgetBase::HandleKill);
    }
}

void UKillFeedWidgetBase::NativeConstruct()
{
    Super::NativeConstruct();
    // 名字不在这里建表：AddToViewport 同步触发本函数，早于 BindSession——此刻 Session 必为空
}

void UKillFeedWidgetBase::NativeDestruct()
{
    if (Session.IsValid()) Session->OnKill.RemoveDynamic(this, &UKillFeedWidgetBase::HandleKill);
    Super::NativeDestruct();
}

void UKillFeedWidgetBase::HandleKill(const FSightlineKill& Kill)
{
    PushLine(FString::Printf(TEXT("%s 击杀了 %s"), *NameOf(Kill.KillerPid), *NameOf(Kill.VictimPid)));
}

FString UKillFeedWidgetBase::NameOf(int32 Pid) const
{
    // 击杀时刻现查 Registry（人机/真人统一——服务器名册即真相；结算面板同款）
    if (Session.IsValid())
        if (const auto* Reg = Session->GetRegistry())
            for (const FSightlineEntityState& E : Reg->All())
                if (E.PlayerId == Pid) return E.Name;
    return FString::FromInt(Pid);
}

void UKillFeedWidgetBase::PushLine(const FString& Text)
{
    // 三行循环：0=最新（上），旧内容下移
    if (KillLine2) KillLine2->SetText(KillLine1 ? KillLine1->GetText() : FText::GetEmpty());
    if (KillLine1) KillLine1->SetText(KillLine0 ? KillLine0->GetText() : FText::GetEmpty());
    if (KillLine0) KillLine0->SetText(FText::FromString(Text));
}

// ───────── 结算面板 ─────────
void UResultWidgetBase::BindSession(USightlineMatchSession* InSession, int32 InSelfPid)
{
    Session = InSession;
    SelfPid = InSelfPid;
    if (Session.IsValid()) Session->OnMatchEnd.AddDynamic(this, &UResultWidgetBase::HandleMatchEnd);
}

void UResultWidgetBase::NativeConstruct()
{
    Super::NativeConstruct();
    SetVisibility(ESlateVisibility::Collapsed);        // 平时隐藏（结算事件到达才显示）
    if (RematchButton) RematchButton->OnClicked.AddDynamic(this, &UResultWidgetBase::OnRematchClicked);
    if (LeaveButton) LeaveButton->OnClicked.AddDynamic(this, &UResultWidgetBase::OnLeaveClicked);
}

void UResultWidgetBase::NativeDestruct()
{
    if (Session.IsValid()) Session->OnMatchEnd.RemoveDynamic(this, &UResultWidgetBase::HandleMatchEnd);
    Super::NativeDestruct();
}

void UResultWidgetBase::HandleMatchEnd(const TArray<FSightlineScoreRow>& Rows)
{
    // 比分行：名次. 名字 击杀/死亡（名字抓 Registry；结算时刻名册还在）
    FString List;
    int32 SelfRank = 1;
    for (const auto& r : Rows)
    {
        FString Name = FString::FromInt(r.pid);
        if (Session.IsValid())
            for (const FSightlineEntityState& E : Session->GetRegistry()->All())
                if (E.PlayerId == r.pid) { Name = E.Name; break; }
        List += FString::Printf(TEXT("%d. %s   %d 杀 / %d 死\n"), SelfRank++, *Name, r.kills, r.deaths);
    }
    if (ScoreList) ScoreList->SetText(FText::FromString(List));
    if (ResultTitle) ResultTitle->SetText(FText::FromString(TEXT("对局结束")));
    SetVisibility(ESlateVisibility::Visible);          // 显示结算
}

void UResultWidgetBase::OnRematchClicked()
{
    if (Session.IsValid()) Session->StartMatch();      // 复走 14 号（房主校验在服务器）
    SetVisibility(ESlateVisibility::Collapsed);
}

void UResultWidgetBase::OnLeaveClicked()
{
    if (Session.IsValid()) Session->NotifyLinkLost();  // 简化：离场走链路清场（正式离房=ep08）
    SetVisibility(ESlateVisibility::Collapsed);
}
