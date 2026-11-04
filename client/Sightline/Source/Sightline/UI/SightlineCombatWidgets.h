// SightlineCombatWidgets.h —— ep07 对局战斗 UI 基类（BindWidget 按名抓取——WBP 放皮，逻辑在 C++）
// 三块：对局 HUD（WBP_CombatHud·血条是首个控件，弹药/枪械名后续扩展同一面板）/ 击杀播报（3 行循环）/ 结算面板（胜负+比分+再来一局）
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

/** 对局 HUD 基类（血条=首个控件；弹药/枪械名/技能等后续控件同面板扩展——新控件随意命名
 *  + 在此加 BindWidgetOptional 字段即可，不与已有合同名冲突）。
 *  血条数据=OnFireResult.VictimHp（受击掉血）+ OnRespawn（满血）；文本格式「75 / 100」单控件 */
UCLASS(Abstract)
class SIGHTLINE_API UCombatHudWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void BindSession(USightlineMatchSession* InSession);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

    UFUNCTION() void HandleFireResult(const FSightlineFireResult& Result);
    UFUNCTION() void HandleRespawn(int32 Pid, int32 SpawnIndex, int32 InvulnMs, FVector SpawnPos);

    UPROPERTY(meta = (BindWidgetOptional))
    UProgressBar* HealthBar = nullptr;
    UPROPERTY(meta = (BindWidgetOptional))
    UTextBlock* HealthText = nullptr;
    /** 自机无敌倒计时（2026-11-04 收官：24 号 InvulnMs 终于被消费）——可摆可不摆，不摆=静默降级 */
    UPROPERTY(meta = (BindWidgetOptional))
    UTextBlock* InvulnText = nullptr;

private:
    void SetHp(int32 Hp, int32 MaxHp);
    TWeakObjectPtr<USightlineMatchSession> Session;
    int32 SelfPid = 0;
    double InvulnRemainSec = 0.0;                  // >0=自机无敌剩余（NativeTick 递减驱动）
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

/** 比分板（2026-11-05 收官：Tab 按住显示——23 号事件流累加比分·终局 25 号兜底对账日志）。
 *  谦卑对象：只渲染 MatchSession 的排序快照（GetScoreboard）+Registry 名字（NameOf），
 *  自身零状态；4 行×3 列（满房 4 人固定行，ep08 扩容再动态）；必绑命名合同见下 */
UCLASS(Abstract)
class SIGHTLINE_API UScoreboardWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void BindSession(USightlineMatchSession* InSession);
    void Refresh();                                    // 现拉现填（显示/事件三处共用）

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION() void HandleKill(const FSightlineKill& Kill);      // 可见则刷新
    UFUNCTION() void HandleRespawn(int32 Pid, int32 SpawnIndex, int32 InvulnMs, FVector SpawnPos);

    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowName0 = nullptr;   // 4 行×3 列（必绑：命名错编辑器即拦）
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowKill0 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowDeath0 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowName1 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowKill1 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowDeath1 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowName2 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowKill2 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowDeath2 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowName3 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowKill3 = nullptr;
    UPROPERTY(meta = (BindWidget)) UTextBlock* SRowDeath3 = nullptr;

private:
    UTextBlock* Cell(int32 Row, int32 Col);            // (行,列) 取格：0=名 1=杀 2=死
    TWeakObjectPtr<USightlineMatchSession> Session;
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
