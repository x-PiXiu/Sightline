// SightlineUIWidgets —— 第 2 期三界面的 C++ 基类（混合模式）
// 分工：WBP 蓝图资产只放"皮"（布局/样式，根类设为这里的基类）；逻辑全在 C++（蓝图=0 逻辑铁律）
//      BindWidget 按名字自动抓蓝图控件——WBP 里控件名必须与这里一致，缺了编译期/运行期即报
// 依赖方向：UI（最外层）→ ULobbySession → FSightlineConnection；本文件不认识服务器细节
#pragma once
#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Lobby/SightlineLobby.h"
#include "SightlineUIWidgets.generated.h"

class UButton;
class UEditableTextBox;
class UTextBlock;

/**
 * 登录界面基类（WBP_Login 的根类）
 * 控件：NameInput（名字输入框）、LoginButton（登录按钮）、MessageText（可选：错误提示行）
 */
UCLASS(Abstract)
class SIGHTLINE_API ULoginWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void Init(ULobbySession* InSession);

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION()
    void OnLoginClicked();

    UFUNCTION()
    void HandleLoginResult(bool bSuccess, int32 PlayerId, const FString& Message);

    UFUNCTION()
    void HandleKicked(int32 ReasonCode);

    UFUNCTION()
    void HandleLinkLost();

    UPROPERTY(meta = (BindWidget))
    UEditableTextBox* NameInput = nullptr;

    UPROPERTY(meta = (BindWidget))
    UButton* LoginButton = nullptr;

    UPROPERTY(meta = (BindWidgetOptional))
    UTextBlock* MessageText = nullptr;

private:
    void SetMessage(const FString& Text);

    TWeakObjectPtr<ULobbySession> Session;
};

/**
 * 大厅界面基类（WBP_Lobby 的根类）
 * 控件：PlayerNameText（我的名字）、RoomListText（房间列表多行文本）、
 *       RoomNameInput（建房名）、RoomIdInput（要进的房号）、CreateButton、JoinButton
 * 刷新：OnRoomListChanged → 重绘列表（GI 负责界面切换，本类负责列表内容）
 */
UCLASS(Abstract)
class SIGHTLINE_API ULobbyWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void Init(ULobbySession* InSession);
    void RefreshFromModel();                        // 已在本界面时 GI 幂等刷新用

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION()
    void OnCreateClicked();

    UFUNCTION()
    void OnJoinClicked();

    UFUNCTION()
    void HandleRoomListChanged(const TArray<FRoomInfo>& Rooms);

    UFUNCTION()
    void HandleActionFailed(const FString& Reason);

    /** 可选提示行（WBP_Lobby 摆了才显示，没摆不崩） */
    UPROPERTY(meta = (BindWidgetOptional))
    UTextBlock* MessageText = nullptr;

    UPROPERTY(meta = (BindWidget))
    UTextBlock* PlayerNameText = nullptr;

    UPROPERTY(meta = (BindWidget))
    UTextBlock* RoomListText = nullptr;

    UPROPERTY(meta = (BindWidget))
    UEditableTextBox* RoomNameInput = nullptr;

    UPROPERTY(meta = (BindWidget))
    UEditableTextBox* RoomIdInput = nullptr;

    UPROPERTY(meta = (BindWidget))
    UButton* CreateButton = nullptr;

    UPROPERTY(meta = (BindWidget))
    UButton* JoinButton = nullptr;

private:
    TWeakObjectPtr<ULobbySession> Session;
};

/**
 * 房间界面基类（WBP_Room 的根类）
 * 控件：RoomTitleText（房号）、MemberListText（成员名单，首位★房主）、LeaveButton
 */
UCLASS(Abstract)
class SIGHTLINE_API URoomWidgetBase : public UUserWidget {
    GENERATED_BODY()

public:
    void Init(ULobbySession* InSession);
    void RefreshFromModel();                        // 幂等进入：先显示当前房间状态

protected:
    virtual void NativeConstruct() override;
    virtual void NativeDestruct() override;

    UFUNCTION()
    void OnLeaveClicked();

    UFUNCTION()
    void HandleRoomStateChanged(int32 RoomId, const FString& HostName, const TArray<FString>& Members);

    UPROPERTY(meta = (BindWidget))
    UTextBlock* RoomTitleText = nullptr;

    UPROPERTY(meta = (BindWidget))
    UTextBlock* MemberListText = nullptr;

    UPROPERTY(meta = (BindWidget))
    UButton* LeaveButton = nullptr;

private:
    TWeakObjectPtr<ULobbySession> Session;
};
