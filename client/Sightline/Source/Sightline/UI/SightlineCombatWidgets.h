// SightlineCombatWidgets.h —— ep07 对局战斗 UI 基类（BindWidget 按名抓取——WBP 放皮，逻辑在 C++）
// 三块：血条（扩展现有对局 HUD 行）/ 击杀播报（3 行循环）/ 结算面板（胜负+比分+再来一局）
// 数据源全走 MatchSession 委托：OnFireResult.VictimHp（血条）/ OnKill（播报）/ OnMatchEnd（结算）
// 工艺=UI/SightlineUIWidgets.h 同款（BindWidget 名字合同；Optional 缺了不崩）
#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Match/SightlineMatchSession.h"   // FSightlineFireResult/Kill/ScoreRow：UFUNCTION 形参要完整类型（头文件自足，不赖 unity 拼接顺序）
#include "SightlineCombatWidgets.generated.h"

/** 血条（对局 HUD 扩展——自己血量；数据=OnFireResult.VictimHp + OnRespawn 满血） */
UCLASS(Abstract)
class SIGHTLINE_API UCombatHudWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void BindSession(USightlineMatchSession* InSession);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION() void HandleFireResult(const FSightlineFireResult& Result);
    UFUNCTION() void HandleRespawn(int32 Pid, int32 SpawnIndex, int32 InvulnMs);

    UPROPERTY(meta = (BindWidgetOptional))
    UProgressBar* HealthBar = nullptr;
    UPROPERTY(meta = (BindWidgetOptional))
    UTextBlock* HealthText = nullptr;

private:
    void SetHp(int32 Hp, int32 MaxHp);
    TWeakObjectPtr<USightlineMatchSession> Session;
    int32 SelfPid = 0;
};

/** 击杀播报（3 行循环——最新在上；数据=OnKill 23 号） */
UCLASS(Abstract)
class SIGHTLINE_API UKillFeedWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void BindSession(USightlineMatchSession* InSession);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION() void HandleKill(const FSightlineKill& Kill);

    UPROPERTY(meta = (BindWidget)) UTextBlock* KillLine0 = nullptr;   // 最新
    UPROPERTY(meta = (BindWidget)) UTextBlock* KillLine1 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* KillLine2 = nullptr;   // 最旧

private:
    void PushLine(const FString& Text);
    FString NameOf(int32 Pid) const;              // 击杀时刻现查 Registry（构造期建表=空表，见 .cpp）
    TWeakObjectPtr<USightlineMatchSession> Session;
    int32 SelfPid = 0;
};

/** 结算面板（OnMatchEnd 25 号触发显示；房主可再来一局） */
UCLASS(Abstract)
class SIGHTLINE_API UResultWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void BindSession(USightlineMatchSession* InSession, int32 InSelfPid);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION() void HandleMatchEnd(const TArray<FSightlineScoreRow>& Rows);
    UFUNCTION() void OnRematchClicked();
    UFUNCTION() void OnLeaveClicked();

    UPROPERTY(meta = (BindWidget)) UTextBlock* ResultTitle = nullptr;   // 胜利/败北
    UPROPERTY(meta = (BindWidget)) UTextBlock* ScoreList = nullptr;     // 逐行比分
    UPROPERTY(meta = (BindWidget)) UButton* RematchButton = nullptr;    // 房主可用
    UPROPERTY(meta = (BindWidget)) UButton* LeaveButton = nullptr;

private:
    TWeakObjectPtr<USightlineMatchSession> Session;
    int32 SelfPid = 0;
};
