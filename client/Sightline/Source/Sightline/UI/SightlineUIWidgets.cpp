// SightlineUIWidgets 实现 —— 逻辑全 C++：按钮 → 意图；委托 → 刷 UI
#include "UI/SightlineUIWidgets.h"

#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/RichTextBlock.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "GameInstance/SightlineGameInstance.h"
#include "Match/SightlineMatchSession.h"

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

namespace {
    // —— 名册排版（docs/episodes/ep03/ui_roomlist_spec）：Font_Mono 等宽 + 全角补位列锚 + RichText 颜色标签 ——
    // 半角槽：ASCII=1、CJK/全角=2；全角空格(U+3000)=2 槽。列宽（半角槽）：房号 4 · 房名 24 · 人数 4
    int32 ListDisplaySlots(const FString& In) {
        int32 Slots = 0;
        for (const TCHAR C : In) Slots += (C >= 0x2E80) ? 2 : 1;
        return Slots;
    }
    FString ListPad(const FString& In, const int32 TargetSlots) {
        FString Out = In;
        int32 Gap;
        while ((Gap = TargetSlots - ListDisplaySlots(Out)) >= 2) Out += TEXT("　");
        if (Gap == 1) Out += TEXT(" ");
        return Out;
    }
    FString ListRow(const int32 RoomId, const FString& RoomName,
                    const int32 Count, const int32 Capacity, const FString& Host,
                    const bool bFullStyled) {
        const FString Row = FString::Printf(TEXT("%s　%s　%s　%s"),
            *ListPad(FString::FromInt(RoomId), 4),
            *ListPad(RoomName, 24),
            *ListPad(FString::Printf(TEXT("%d/%d"), Count, Capacity), 4),
            *Host);
        return bFullStyled ? FString::Printf(TEXT("<full>%s</>"), *Row) : Row;
    }
}

void ULobbyWidgetBase::Init(ULobbySession* InSession) {
    Session = InSession;
}

void ULobbyWidgetBase::NativeConstruct() {
    Super::NativeConstruct();
    if (RoomListText) RoomListText->SetAutoWrapText(false);  // 列锚名册禁自动换行（列宽有界见 ui_roomlist_spec，折行即破对齐）
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
    FString Out = FString::Printf(TEXT("<head>%s　%s　%s　%s</>\n"),
        *ListPad(TEXT("房号"), 4), *ListPad(TEXT("房名"), 24),
        *ListPad(TEXT("人数"), 4), TEXT("房主"));
    for (const FRoomInfo& R : Rooms) {
        Out += ListRow(R.RoomId, R.RoomName, R.MemberCount, kRoomCapacity, R.HostName,
                       R.MemberCount >= kRoomCapacity) + TEXT("\n");
    }
    Out += TEXT("\n要加入哪个房间，就把它的房号填到右侧房号框里，点【申请进房】");
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

void URoomWidgetBase::InitMatch(USightlineMatchSession* InMatch) {
    MatchSession = InMatch;                                // 开始对局意图的出口（D3-15 客户端镜像）
}

void URoomWidgetBase::NativeConstruct() {
    Super::NativeConstruct();
    UE_LOG(LogTemp, Warning,
           TEXT("[诊断] Room 构造：根类=%s | MemberListText=%s | RoomTitleText=%s | LeaveButton=%s | StartMatchButton=%s | 槽位模式=%s"),
           *GetClass()->GetName(),
           MemberListText ? TEXT("已绑定") : TEXT("未绑定!"),
           RoomTitleText ? TEXT("已绑定") : TEXT("未绑定!"),
           LeaveButton ? TEXT("已绑定") : TEXT("未绑定!"),
           StartMatchButton ? TEXT("已绑定") : TEXT("未绑定!"),
           SlotText_1 ? TEXT("ON") : TEXT("OFF（文本名单兜底）"));
    if (MemberListText) MemberListText->SetAutoWrapText(true);   // 成员名单自动换行
    if (LeaveButton) LeaveButton->OnClicked.AddDynamic(this, &URoomWidgetBase::OnLeaveClicked);
    if (StartMatchButton) {
        StartMatchButton->OnClicked.AddDynamic(this, &URoomWidgetBase::OnStartMatchClicked);
        StartMatchButton->SetVisibility(ESlateVisibility::Collapsed);   // 默认折叠，房主身份到了再显
    }
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
    // HostName 实参修正：原传空串——P5 的开始对局按钮可见性依赖它，进房首次绘制即须正确显隐
    const FString HostName = M.CurrentMembers.Num() > 0 ? M.CurrentMembers[0] : FString();
    HandleRoomStateChanged(M.CurrentRoomId, HostName, M.CurrentMembers);
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

    // 开始对局按钮可见性（P5）：房主可见，非房主折叠（点击只发意图，服务器二次校验）
    if (StartMatchButton)
        StartMatchButton->SetVisibility(HostName == MyName
            ? ESlateVisibility::Visible
            : ESlateVisibility::Collapsed);
}

void URoomWidgetBase::OnLeaveClicked() {
    if (Session.IsValid()) Session->LeaveRoom();                  // 回大厅由 GI 的列表委托驱动
}

void URoomWidgetBase::OnStartMatchClicked() {
    if (MatchSession.IsValid()) MatchSession->StartMatch();       // 意图只发包（v3 契约 14，空 payload）
}

// ============ 调试面板（M5 损伤滑块）============

void UDebugPanelBase::Init(USightlineGameInstance* InGI) {
    GI = InGI;
}

void UDebugPanelBase::NativeConstruct() {
    Super::NativeConstruct();
    // 绑定诊断：Optional 控件名对不上时静默失败（不摆/错名都不报错）——一行日志见分晓
    UE_LOG(LogSightline, Log, TEXT("[UI] DebugPanel 绑定：Slider=%s Text=%s（WBP 控件名须为 DelaySlider / DelayText）"),
           DelaySlider ? TEXT("有") : TEXT("无"),
           DelayText ? TEXT("有") : TEXT("无"));
    if (DelaySlider) {
        DelaySlider->SetValue(0.f);
        DelaySlider->OnValueChanged.AddDynamic(this, &UDebugPanelBase::OnDelayChanged);
    }
    if (DelayText) DelayText->SetText(FText::FromString(TEXT("入站延迟 0 ms")));
}

void UDebugPanelBase::OnDelayChanged(float Value) {
    const int32 Ms = FMath::RoundToInt(Value);
    if (DelayText) DelayText->SetText(FText::FromString(
        FString::Printf(TEXT("入站延迟 %d ms"), Ms)));
    if (GI.IsValid()) GI->SetInboundDelayMs(Ms);              // 队列本体在连接适配器（D3-11）
}
