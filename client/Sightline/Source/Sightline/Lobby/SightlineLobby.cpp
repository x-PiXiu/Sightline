// SightlineLobby 实现 —— 帧解析按 MSG_IDS.md v2（4~13 号消息，小端）
// 名字编码：TCHAR → UTF-8（服务器契约）；解析侧防越界：任何字段先验长度再解包
#include "Lobby/SightlineLobby.h"

#include "Connection/SightlineConnection.h"
#include "Connection/SightlineProtocol.h"
#include "Logging/LogMacros.h"

namespace {
    DEFINE_LOG_CATEGORY_STATIC(LogSightlineLobby, Log, All);

    // TCHAR → UTF-8 字节（含长度）；契约里名字字段 = [1B len][utf8 bytes]
    TArray<uint8> ToUtf8Bytes(const FString& Text) {
        FTCHARToUTF8 Conv(*Text);
        TArray<uint8> Out;
        Out.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
        return Out;
    }

    // UTF-8 字节 → FString（不要求源以 0 结尾）
    FString FromUtf8(const uint8* Data, int32 Size) {
        TArray<ANSICHAR> Tmp;
        Tmp.Append(reinterpret_cast<const ANSICHAR*>(Data), Size);
        Tmp.Add(0);
        return FString(UTF8_TO_TCHAR(Tmp.GetData()));
    }

    uint32_t ReadU32LE(const TArray<uint8>& P, int32 Off) {
        return static_cast<uint32_t>(P[Off]) | (static_cast<uint32_t>(P[Off + 1]) << 8)
             | (static_cast<uint32_t>(P[Off + 2]) << 16) | (static_cast<uint32_t>(P[Off + 3]) << 24);
    }

    // 解析 [1B len][utf8]；越界返回 false
    bool ReadLenStr(const TArray<uint8>& P, int32& Off, FString& Out) {
        if (!P.IsValidIndex(Off)) return false;
        const int32 Len = P[Off];
        if (Off + 1 + Len > P.Num()) return false;
        Out = FromUtf8(P.GetData() + Off + 1, Len);
        Off += 1 + Len;
        return true;
    }

    // 解析 [4B roomId][1B hostLen][host][1B n]{[1B len][name]}（MSG 12）
    bool ParseRoomState(const TArray<uint8>& P, int32& RoomId, FString& Host, TArray<FString>& Members) {
        int32 Off = 4;                              // 跳过 [4B RoomId]——从第 4 字节起读房主名
        if (P.Num() < 5) return false;
        RoomId = static_cast<int32>(ReadU32LE(P, 0));
        if (!ReadLenStr(P, Off, Host)) return false;
        if (!P.IsValidIndex(Off)) return false;
        const int32 N = P[Off];
        ++Off;
        Members.Reset();
        for (int32 i = 0; i < N; ++i) {
            FString M;
            if (!ReadLenStr(P, Off, M)) return false;
            Members.Add(M);
        }
        return true;
    }
} // namespace

void ULobbySession::BindConnection(FSightlineConnection* InConn) {
    Conn = InConn;
}

void ULobbySession::ResetModel() {
    Model.bLoggedIn = false;
    Model.PlayerId = 0;
    Model.PlayerName.Reset();
    Model.Rooms.Reset();
    Model.CurrentRoomId = 0;
    Model.CurrentRoomName.Reset();
    Model.CurrentMembers.Reset();
}

void ULobbySession::NotifyLinkLost() {
    BindConnection(nullptr);
    const bool bWasLoggedIn = Model.bLoggedIn;
    ResetModel();
    if (bWasLoggedIn) {
        UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 链路断开 → 回登录界面"));
        OnLobbyLinkLost.Broadcast();
    }
}

bool ULobbySession::SendLenPrefixedName(uint16 MsgId, const FString& Name) {
    if (!Conn) return false;
    const TArray<uint8> Bytes = ToUtf8Bytes(Name);
    TArray<uint8> Payload;
    Payload.Add(static_cast<uint8>(Bytes.Num()));
    Payload.Append(Bytes);
    Conn->Send(MsgId, Payload);
    return true;
}

void ULobbySession::Login(const FString& PlayerName) {
    const TArray<uint8> Bytes = ToUtf8Bytes(PlayerName);
    if (Bytes.Num() == 0 || Bytes.Num() > 16) {           // 契约：非空 ≤16B（客户端预检，UX）
        OnLoginResult.Broadcast(false, 0, TEXT("名字需非空且不超过 16 字节"));
        return;
    }
    if (!Conn) {                                          // 无连接（被踢/断线未恢复）：界面可见反馈
        OnLoginResult.Broadcast(false, 0, TEXT("尚未连接服务器（自动重连中，稍候再点）"));
        return;
    }
    Model.PlayerName = PlayerName;
    SendLenPrefixedName(SightlineProtocol::MSG_C2S_LOGIN, PlayerName);
}

void ULobbySession::CreateRoom(const FString& RoomName) {
    const TArray<uint8> Bytes = ToUtf8Bytes(RoomName);
    if (!Model.bLoggedIn) { OnActionFailed.Broadcast(TEXT("尚未登录")); return; }
    if (Bytes.Num() == 0 || Bytes.Num() > 24) { OnActionFailed.Broadcast(TEXT("房间名需非空且不超过 24 字节")); return; }
    SendLenPrefixedName(SightlineProtocol::MSG_C2S_CREATE_ROOM, RoomName);
}

void ULobbySession::JoinRoom(int32 RoomId) {
    if (!Model.bLoggedIn) { OnActionFailed.Broadcast(TEXT("尚未登录")); return; }
    if (Model.CurrentRoomId != 0) { OnActionFailed.Broadcast(TEXT("你已在房间中，请先离开")); return; }
    TArray<uint8> Payload;
    for (int i = 0; i < 4; ++i) Payload.Add(static_cast<uint8>((RoomId >> (8 * i)) & 0xFF));
    SendIntent(SightlineProtocol::MSG_C2S_JOIN_ROOM, Payload);
}

void ULobbySession::LeaveRoom() {
    if (Model.CurrentRoomId == 0) { OnActionFailed.Broadcast(TEXT("当前不在房间中")); return; }
    SendIntent(SightlineProtocol::MSG_C2S_LEAVE_ROOM);
}

void ULobbySession::SendIntent(uint16 MsgId, const TArray<uint8>& Payload) {
    if (!Conn) {
        UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 无连接，意图 %d 被丢弃"), MsgId);
        return;
    }
    Conn->Send(MsgId, Payload);
}

void ULobbySession::OnFrame(uint16 MsgId, const TArray<uint8>& Payload) {
    using namespace SightlineProtocol;
    switch (MsgId) {
    case MSG_S2C_LOGIN_RESULT: {                          // [1B 码][4B PlayerId]
        if (Payload.Num() < 5) break;
        const uint8 Code = Payload[0];
        const int32 Pid = static_cast<int32>(ReadU32LE(Payload, 1));
        if (Code == 0) {
            Model.bLoggedIn = true;
            Model.PlayerId = Pid;
            UE_LOG(LogSightlineLobby, Log, TEXT("[Lobby] 登录成功 pid=%d"), Pid);
            OnLoginResult.Broadcast(true, Pid, TEXT(""));
        } else {
            const FString Msg = (Code == 1) ? TEXT("名字已被占用") : TEXT("名字非法");
            UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 登录失败 码=%d"), Code);
            OnLoginResult.Broadcast(false, 0, Msg);
        }
        break;
    }
    case MSG_S2C_CREATE_RESULT:                           // [1B 码][4B RoomId]
        if (Payload.Num() < 5) break;
        if (Payload[0] != 0) {
            static const TCHAR* kCreateFail[4] = {TEXT(""), TEXT("未登录"), TEXT("你已在房间中"), TEXT("房间名非法")};
            const uint8 Code = Payload[0];
            const FString Reason = Code <= 3 ? kCreateFail[Code] : TEXT("建房被拒绝");
            UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 建房失败 码=%u %s"), Code, *Reason);
            OnActionFailed.Broadcast(Reason);
        }
        break;                                            // 成功的确认 = 随后的 ROOM_STATE
    case MSG_S2C_JOIN_RESULT:                             // 成功 [0][4B RoomId][1B 人数]=6B；失败 [码][4B RoomId]=5B
        if (Payload.Num() < 5) break;
        if (Payload[0] == 0) {
            if (Payload.Num() >= 6)
                Model.CurrentRoomId = static_cast<int32>(ReadU32LE(Payload, 1));
            UE_LOG(LogSightlineLobby, Log, TEXT("[Lobby] 进房 %d"), Model.CurrentRoomId);
        } else {
            static const TCHAR* kJoinFail[5] = {TEXT(""), TEXT("未登录"), TEXT("房间不存在"),
                                                TEXT("房间已满(4人)"), TEXT("你已在其他房间中")};
            const uint8 Code = Payload[0];
            const FString Reason = Code <= 4 ? kJoinFail[Code] : TEXT("进房被拒绝");
            UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 进房失败 码=%u %s"), Code, *Reason);
            OnActionFailed.Broadcast(Reason);             // 界面可见反馈（错误 #14 修复）
        }
        break;                                            // 房间界面切换 = 随后的 ROOM_STATE
    case MSG_S2C_ROOM_LIST: {                             // [2B N]{...} → 全量替换
        if (Payload.Num() < 2) break;
        const int32 N = Payload[0] | (Payload[1] << 8);
        int32 Off = 2;
        TArray<FRoomInfo> Rooms;
        bool Ok = true;
        for (int32 i = 0; i < N && Ok; ++i) {
            if (Off + 5 > Payload.Num()) { Ok = false; break; }
            FRoomInfo Info;
            Info.RoomId = static_cast<int32>(ReadU32LE(Payload, Off));
            Info.MemberCount = Payload[Off + 4];
            Off += 5;
            Ok = ReadLenStr(Payload, Off, Info.RoomName) && ReadLenStr(Payload, Off, Info.HostName);
            if (Ok) Rooms.Add(Info);
        }
        if (Ok) {
            Model.Rooms = Rooms;
            Model.CurrentRoomId = 0;                      // 收到列表 = 此刻人在大厅
            Model.CurrentRoomName.Reset();
            Model.CurrentMembers.Reset();
            OnRoomListChanged.Broadcast(Model.Rooms);
        }
        break;
    }
    case MSG_S2C_ROOM_STATE: {                            // [4B RoomId][房主][成员...]
        int32 RoomId = 0;
        FString Host;
        TArray<FString> Members;
        if (!ParseRoomState(Payload, RoomId, Host, Members)) break;
        Model.CurrentRoomId = RoomId;                     // 离开者收到的也是"所在房"最终态前的快照
        if (Model.Rooms.Num() > 0)                        // 从列表里补房间名（若本端见过）
            for (const FRoomInfo& R : Model.Rooms)
                if (R.RoomId == RoomId) { Model.CurrentRoomName = R.RoomName; break; }
        Model.CurrentMembers = Members;
        OnRoomStateChanged.Broadcast(RoomId, Host, Members);
        break;
    }
    case MSG_S2C_KICKED: {                                // [1B 原因码]
        const uint8 Code = Payload.Num() > 0 ? Payload[0] : 0;
        UE_LOG(LogSightlineLobby, Warning, TEXT("[Lobby] 被 KICKED 码=%d"), Code);
        ResetModel();
        OnKicked.Broadcast(Code);
        break;                                            // 服务器随后断开 → NotifyLinkLost 兜底
    }
    default:
        break;
    }
}
