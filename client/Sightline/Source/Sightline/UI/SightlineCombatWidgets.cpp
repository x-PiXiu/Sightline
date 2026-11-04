// SightlineCombatWidgets.cpp —— ep07 战斗 UI 三件实现（数据全走 MatchSession 委托）
#include "UI/SightlineCombatWidgets.h"
#include "Match/SightlineMatchSession.h"
#include "GameInstance/SightlineGameInstance.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"

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
    if (InvulnText) InvulnText->SetVisibility(ESlateVisibility::Collapsed);
}

void UCombatHudWidgetBase::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
    Super::NativeTick(MyGeometry, InDeltaTime);
    if (!InvulnText) return;
    if (InvulnRemainSec > 0.0)
    {
        InvulnRemainSec = FMath::Max(0.0, InvulnRemainSec - InDeltaTime);
        if (InvulnRemainSec > 0.0)
        {
            InvulnText->SetText(FText::FromString(FString::Printf(TEXT("无敌 %.1f 秒"), InvulnRemainSec)));
            InvulnText->SetVisibility(ESlateVisibility::HitTestInvisible);   // 可见不挡输入
            return;
        }
    }
    InvulnText->SetVisibility(ESlateVisibility::Collapsed);
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

void UCombatHudWidgetBase::HandleRespawn(int32 Pid, int32 /*SpawnIndex*/, int32 InvulnMs, FVector /*SpawnPos*/)
{
    if (Session.IsValid() && Session->IsSelfPlayer(Pid))
    {
        SetHp(100, 100);
        InvulnRemainSec = InvulnMs / 1000.0;           // 无敌倒计时入账（NativeTick 递减；不摆 InvulnText=静默）
    }
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
    // 名字查询上收 MatchSession::NameOf（2026-11-05 比分板共用——消重）
    return Session.IsValid() ? Session->NameOf(Pid) : FString::FromInt(Pid);
}

void UKillFeedWidgetBase::PushLine(const FString& Text)
{
    // 三行循环：0=最新（上），旧内容下移
    if (KillLine2) KillLine2->SetText(KillLine1 ? KillLine1->GetText() : FText::GetEmpty());
    if (KillLine1) KillLine1->SetText(KillLine0 ? KillLine0->GetText() : FText::GetEmpty());
    if (KillLine0) KillLine0->SetText(FText::FromString(Text));
}

// ───────── 比分板（Tab 按住显示·谦卑对象）─────────
void UScoreboardWidgetBase::BindSession(USightlineMatchSession* InSession)
{
    Session = InSession;
    if (Session.IsValid())
    {
        Session->OnKill.AddDynamic(this, &UScoreboardWidgetBase::HandleKill);
        Session->OnRespawn.AddDynamic(this, &UScoreboardWidgetBase::HandleRespawn);
    }
}

void UScoreboardWidgetBase::NativeConstruct()
{
    Super::NativeConstruct();
    SetVisibility(ESlateVisibility::Collapsed);        // Tab 按住才显示（结算面板同款自管理）
}

void UScoreboardWidgetBase::NativeDestruct()
{
    if (Session.IsValid())
    {
        Session->OnKill.RemoveDynamic(this, &UScoreboardWidgetBase::HandleKill);
        Session->OnRespawn.RemoveDynamic(this, &UScoreboardWidgetBase::HandleRespawn);
    }
    Super::NativeDestruct();
}

void UScoreboardWidgetBase::HandleKill(const FSightlineKill& /*Kill*/)
{
    if (IsVisible()) Refresh();                        // 按住期间实时刷新（含人机互杀=全房间榜）
}

void UScoreboardWidgetBase::HandleRespawn(int32 /*Pid*/, int32, int32, FVector)
{
    if (IsVisible()) Refresh();
}

UTextBlock* UScoreboardWidgetBase::Cell(int32 Row, int32 Col)
{
    switch (Row)
    {
    case 0: return Col == 0 ? SRowName0 : Col == 1 ? SRowKill0 : SRowDeath0;
    case 1: return Col == 1 ? SRowKill1 : Col == 2 ? SRowDeath1 : SRowName1;
    case 2: return Col == 0 ? SRowName2 : Col == 1 ? SRowKill2 : SRowDeath2;
    default: return Col == 0 ? SRowName3 : Col == 1 ? SRowKill3 : SRowDeath3;
    }
}

void UScoreboardWidgetBase::Refresh()
{
    if (!Session.IsValid()) return;
    const TArray<FSightlineScoreRow> Rows = Session->GetScoreboard();   // 排序快照（kills 降序）
    for (int32 Row = 0; Row < 4; ++Row)
    {
        UTextBlock* NameT = Cell(Row, 0);
        UTextBlock* KillT = Cell(Row, 1);
        UTextBlock* DeathT = Cell(Row, 2);
        if (!NameT || !KillT || !DeathT) continue;
        if (Row < Rows.Num())
        {
            const FSightlineScoreRow& r = Rows[Row];
            NameT->SetText(FText::FromString(Session->NameOf(r.pid)));
            KillT->SetText(FText::AsNumber(r.kills));
            DeathT->SetText(FText::AsNumber(r.deaths));
            // 自机行高亮（青色——与 InvulnText 同色系；他人白色）
            const bool bSelf = Session->IsSelfPlayer(r.pid);
            NameT->SetColorAndOpacity(FSlateColor(bSelf ? FLinearColor(0.3f, 0.9f, 1.0f) : FLinearColor::White));
        }
        else
        {
            NameT->SetText(FText::GetEmpty());
            KillT->SetText(FText::GetEmpty());
            DeathT->SetText(FText::GetEmpty());
        }
    }
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
    // ep07 收官：对局结束弹鼠标（此前大厅 EnsureUIMouseCursor 显示过、切图后新 PC 默认隐藏——
    // 结算按钮一直点不了）。离开回登录保持可见（登录要打字）；再来一局在 OnRematchClicked 收
    if (APlayerController* PC = GetOwningPlayer())
        PC->bShowMouseCursor = true;
}

void UResultWidgetBase::OnRematchClicked()
{
    if (APlayerController* PC = GetOwningPlayer())
        PC->bShowMouseCursor = false;                  // 收鼠标（15 号切图新 PC 本会重置——防御双保险）
    if (Session.IsValid()) Session->StartMatch();      // 复走 14 号（房主校验在服务器）
    SetVisibility(ESlateVisibility::Collapsed);
}

void UResultWidgetBase::OnLeaveClicked()
{
    // ep07 收官：走契约 10 号离房（连接保持）——服务器移出房间广播 ROOM_LIST → GI 既有流程
    // "切回大厅世界+显示大厅"（2026-11-05 用户实锤"点返回大厅后大厅页面没有显示"：
    //  原接 NotifyLinkLost=断链回登录，大厅自然不显示）
    if (Session.IsValid()) Session->SendLeaveRoom();
    SetVisibility(ESlateVisibility::Collapsed);
}
