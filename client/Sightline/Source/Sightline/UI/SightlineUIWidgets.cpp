// SightlineUIWidgets 实现 —— 逻辑全 C++：按钮 → 意图；委托 → 刷 UI
#include "UI/SightlineUIWidgets.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/TextBlock.h"

namespace {
    // 房间容量上限 = 服务器契约满员码的判据（MSG_IDS 消息 9：3 = 已满 4 人）
    constexpr int32 kRoomCapacity = 4;
}

// ============ 登录界面 ============

void ULoginWidgetBase::Init(ULobbySession* InSession) {
    Session = InSession;
}

void ULoginWidgetBase::NativeConstruct() {
    Super::NativeConstruct();
    if (LoginButton) LoginButton->OnClicked.AddDynamic(this, &ULoginWidgetBase::OnLoginClicked);
    if (Session.IsValid()) {                                                       // Init 之后被构造（GI 流程保证）
        Session->OnLoginResult.AddDynamic(this, &ULoginWidgetBase::HandleLoginResult);
        Session->OnKicked.AddDynamic(this, &ULoginWidgetBase::HandleKicked);
        Session->OnLobbyLinkLost.AddDynamic(this, &ULoginWidgetBase::HandleLinkLost);
    }
    SetMessage(TEXT("连接服务器成功，请输入名字登录"));
}

void ULoginWidgetBase::NativeDestruct() {
    if (Session.IsValid()) {
        Session->OnLoginResult.RemoveDynamic(this, &ULoginWidgetBase::HandleLoginResult);
        Session->OnKicked.RemoveDynamic(this, &ULoginWidgetBase::HandleKicked);
        Session->OnLobbyLinkLost.RemoveDynamic(this, &ULoginWidgetBase::HandleLinkLost);
    }
    Super::NativeDestruct();
}

void ULoginWidgetBase::HandleKicked(int32 ReasonCode) {
    SetMessage(FString::Printf(TEXT("已被服务器断开（码 %d）——请重新登录"), ReasonCode));
}

void ULoginWidgetBase::HandleLinkLost() {
    SetMessage(TEXT("连接已断开，自动重连中……"));
}

void ULoginWidgetBase::OnLoginClicked() {
    if (!Session.IsValid()) return;
    Session->Login(NameInput ? NameInput->GetText().ToString() : FString());
    SetMessage(TEXT("登录中……"));
}

void ULoginWidgetBase::HandleLoginResult(bool bSuccess, int32 PlayerId, const FString& Message) {
    if (bSuccess) {
        SetMessage(FString::Printf(TEXT("登录成功（编号 %d），正在进入大厅"), PlayerId));
        // 界面切换由 GameInstance 的委托处理（切大厅）
    } else {
        SetMessage(Message.IsEmpty() ? TEXT("登录失败") : Message);
    }
}

void ULoginWidgetBase::SetMessage(const FString& Text) {
    if (MessageText) MessageText->SetText(FText::FromString(Text));
}

// ============ 大厅界面 ============

void ULobbyWidgetBase::Init(ULobbySession* InSession) {
    Session = InSession;
}

void ULobbyWidgetBase::NativeConstruct() {
    Super::NativeConstruct();
    if (RoomListText) RoomListText->SetAutoWrapText(true);   // 多行列表自动换行（C++ 兜底，不依赖面板设置）
    if (CreateButton) CreateButton->OnClicked.AddDynamic(this, &ULobbyWidgetBase::OnCreateClicked);
    if (JoinButton) JoinButton->OnClicked.AddDynamic(this, &ULobbyWidgetBase::OnJoinClicked);
    if (Session.IsValid()) {
        Session->OnRoomListChanged.AddDynamic(this, &ULobbyWidgetBase::HandleRoomListChanged);
        Session->OnActionFailed.AddDynamic(this, &ULobbyWidgetBase::HandleActionFailed);
        RefreshFromModel();                                                       // 幂等进入：先刷一次已有数据
    }
}

void ULobbyWidgetBase::NativeDestruct() {
    if (Session.IsValid()) {
        Session->OnRoomListChanged.RemoveDynamic(this, &ULobbyWidgetBase::HandleRoomListChanged);
        Session->OnActionFailed.RemoveDynamic(this, &ULobbyWidgetBase::HandleActionFailed);
    }
    Super::NativeDestruct();
}

void ULobbyWidgetBase::HandleActionFailed(const FString& Reason) {
    if (MessageText) MessageText->SetText(FText::FromString(FString::Printf(TEXT("⚠ %s"), *Reason)));
}

void ULobbyWidgetBase::RefreshFromModel() {
    if (!Session.IsValid()) return;
    const FLobbyModel& M = Session->Model;
    if (PlayerNameText) PlayerNameText->SetText(FText::FromString(M.PlayerName));
    HandleRoomListChanged(M.Rooms);
}

void ULobbyWidgetBase::HandleRoomListChanged(const TArray<FRoomInfo>& Rooms) {
    if (!RoomListText) return;
    if (Rooms.Num() == 0) {
        RoomListText->SetText(FText::FromString(TEXT("暂无房间——输入房间名点【建房】")));
        return;
    }
    FString Out;
    for (const FRoomInfo& R : Rooms) {
        const bool bFull = R.MemberCount >= kRoomCapacity;
        Out += FString::Printf(TEXT("【%d】%s　%d/%d 人　房主：%s%s\n"),
                               R.RoomId, *R.RoomName, R.MemberCount, kRoomCapacity,
                               *R.HostName, bFull ? TEXT("　【满】") : TEXT(""));
    }
    Out += TEXT("\n要加入哪个房间，就把它的【】里的数字填到房号框里，点【进房】");
    RoomListText->SetText(FText::FromString(Out));
}

void ULobbyWidgetBase::OnCreateClicked() {
    if (!Session.IsValid()) return;
    const FString Name = RoomNameInput ? RoomNameInput->GetText().ToString() : FString();
    if (Name.IsEmpty()) return;                                   // 空名在客户端就拦住（服务器也会拒）
    Session->CreateRoom(Name);
}

void ULobbyWidgetBase::OnJoinClicked() {
    if (!Session.IsValid() || !RoomIdInput) return;
    const FString Text = RoomIdInput->GetText().ToString();
    if (Text.IsEmpty()) return;
    Session->JoinRoom(FCString::Atoi(*Text));
}

// ============ 房间界面 ============

void URoomWidgetBase::Init(ULobbySession* InSession) {
    Session = InSession;
}

void URoomWidgetBase::NativeConstruct() {
    Super::NativeConstruct();
    UE_LOG(LogTemp, Warning,
           TEXT("[诊断] Room 构造：根类=%s | MemberListText=%s | RoomTitleText=%s | LeaveButton=%s | 槽位模式=%s"),
           *GetClass()->GetName(),
           MemberListText ? TEXT("已绑定") : TEXT("未绑定!"),
           RoomTitleText ? TEXT("已绑定") : TEXT("未绑定!"),
           LeaveButton ? TEXT("已绑定") : TEXT("未绑定!"),
           SlotText_1 ? TEXT("ON") : TEXT("OFF（文本名单兜底）"));
    if (MemberListText) MemberListText->SetAutoWrapText(true);   // 成员名单自动换行
    if (LeaveButton) LeaveButton->OnClicked.AddDynamic(this, &URoomWidgetBase::OnLeaveClicked);
    if (Session.IsValid()) {
        Session->OnRoomStateChanged.AddDynamic(this, &URoomWidgetBase::HandleRoomStateChanged);
        RefreshFromModel();                                       // 幂等进入：先显示当前房间状态
    } else {
        UE_LOG(LogTemp, Warning, TEXT("[诊断] Room 构造时 Session 无效！"));
    }
}

void URoomWidgetBase::NativeDestruct() {
    if (Session.IsValid()) Session->OnRoomStateChanged.RemoveDynamic(this, &URoomWidgetBase::HandleRoomStateChanged);
    Super::NativeDestruct();
}

void URoomWidgetBase::RefreshFromModel() {
    if (!Session.IsValid()) return;
    const FLobbyModel& M = Session->Model;
    // 房主 = 成员列表首位（服务器契约：按加入顺序）；成员名逐行显示
    HandleRoomStateChanged(M.CurrentRoomId, FString(), M.CurrentMembers);
}

void URoomWidgetBase::HandleRoomStateChanged(int32 RoomId, const FString& HostName, const TArray<FString>& Members) {
    const FString RoomName = Session.IsValid() ? Session->Model.CurrentRoomName : FString();
    const FString MyName = Session.IsValid() ? Session->Model.PlayerName : FString();
    const bool bSlotMode = (SlotText_1 != nullptr);           // WBP 摆了槽位卡 → 槽位模式

    // 房号大字徽章（最高频操作 = 报号加入）
    if (RoomIdBadgeText) RoomIdBadgeText->SetText(FText::AsNumber(RoomId));

    // 标题：槽位模式 = 房名 + 人数（房号已由徽章承担）；文本模式 = 全量
    if (RoomTitleText) {
        FString Title;
        if (!bSlotMode) Title += FString::Printf(TEXT("房间 %d"), RoomId);
        if (!RoomName.IsEmpty()) Title += (Title.IsEmpty() ? TEXT("") : TEXT(" · ")) + RoomName;
        Title += FString::Printf(TEXT("　%d/%d 人"), Members.Num(), kRoomCapacity);
        RoomTitleText->SetText(FText::FromString(Title));
    }

    if (bSlotMode) {
        // 槽位模式：4 格（有人=绿底白字/房主★/你标记；空=暗底灰字）
        for (int32 i = 0; i < kRoomCapacity; ++i) {
            UTextBlock* SlotText = (i == 0) ? SlotText_1 : (i == 1) ? SlotText_2
                                 : (i == 2) ? SlotText_3 : SlotText_4;
            UBorder* SlotBox = (i == 0) ? SlotBox_1 : (i == 1) ? SlotBox_2
                             : (i == 2) ? SlotBox_3 : SlotBox_4;
            if (!SlotText) continue;
            if (i < Members.Num()) {
                const bool bHost = (i == 0);                  // 服务器契约：房主 = 首位
                const bool bMe = (Members[i] == MyName);
                SlotText->SetText(FText::FromString(FString::Printf(
                    TEXT("%s%s%s"), bHost ? TEXT("★ ") : TEXT(""),
                    *Members[i], bMe ? TEXT("（你）") : TEXT(""))));
                SlotText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
                if (SlotBox) SlotBox->SetBrushColor(FLinearColor(0.13f, 0.42f, 0.24f, 0.9f));
            } else {
                SlotText->SetText(FText::FromString(TEXT("（空位）")));
                SlotText->SetColorAndOpacity(FSlateColor(FLinearColor(0.5f, 0.5f, 0.53f)));
                if (SlotBox) SlotBox->SetBrushColor(FLinearColor(0.11f, 0.11f, 0.13f, 0.8f));
            }
        }
        if (MemberListText) MemberListText->SetVisibility(ESlateVisibility::Collapsed);
        if (StatusText) StatusText->SetText(FText::FromString(
            FString::Printf(TEXT("等待中…  把房间号 %d 告诉朋友，对方输入房号即可加入"), RoomId)));
    } else {
        // 文本名单模式（第 1 档，WBP 未摆槽位卡时兜底）
        if (MemberListText) {
            FString Out;
            for (int32 i = 0; i < kRoomCapacity; ++i) {
                if (i < Members.Num()) {
                    const bool bHost = (i == 0);
                    const bool bMe = (Members[i] == MyName);
                    Out += FString::Printf(TEXT("%d. %s %s%s%s\n"), i + 1,
                                           bHost ? TEXT("★") : TEXT("　"),
                                           *Members[i],
                                           bHost ? TEXT("（房主）") : TEXT(""),
                                           bMe ? TEXT("　← 你") : TEXT(""));
                } else {
                    Out += FString::Printf(TEXT("%d. （空位）\n"), i + 1);
                }
            }
            Out += TEXT("──────────\n等待中… 把房间号告诉朋友，对方输入房号即可加入");
            MemberListText->SetText(FText::FromString(Out));
            MemberListText->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        }
        if (StatusText) StatusText->SetText(FText::FromString(
            FString::Printf(TEXT("等待中…  把房间号 %d 告诉朋友，对方输入房号即可加入"), RoomId)));
    }
}

void URoomWidgetBase::OnLeaveClicked() {
    if (Session.IsValid()) Session->LeaveRoom();                  // 回大厅由 GI 的列表委托驱动
}
