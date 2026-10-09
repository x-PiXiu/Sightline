// lobby_service.cpp —— 用例实现：纯逻辑，零 net 依赖（依赖规则：只 include 自己的实体）
#include "lobby/lobby.h"

namespace sightline::lobby {

bool ValidName(const std::string& name, size_t maxBytes) {
    return !name.empty() && name.size() <= maxBytes;
}

namespace {

bool ValidLoginName(const std::string& n) { return ValidName(n, kMaxNameBytes); }
bool ValidRoomName(const std::string& n)  { return ValidName(n, kMaxRoomNameBytes); }

} // namespace

// —— 私有助手 ——

void LobbyService::RemovePlayer(int fd) {
    if (auto it = playersByFd_.find(fd); it != playersByFd_.end()) {
        fdByName_.erase(it->second.name);
        playersByFd_.erase(it);
    }
    graceByFd_.erase(fd);
}

std::vector<Event> LobbyService::LeaveInternal(int fd) {
    std::vector<Event> out;
    auto rit = roomByFd_.find(fd);
    if (rit == roomByFd_.end()) return out;             // 不在房中：无事发生

    const uint32_t roomId = rit->second;
    auto& room = rooms_.at(roomId);
    for (size_t i = 0; i < room.memberFds.size(); ++i)
        if (room.memberFds[i] == fd) { room.memberFds.erase(room.memberFds.begin() + i); break; }
    roomByFd_.erase(rit);

    if (room.empty()) {
        rooms_.erase(roomId);                           // 人数归 0 → 解散
    } else {
        // 房内推送：房主 = memberFds 首位（房主转移由"按加入顺序"自动成立）
        Event st{EvType::RoomStatePush};
        st.roomId = roomId;
        st.hostName = playersByFd_.at(room.memberFds.front()).name;
        for (int m : room.memberFds) st.members.push_back(playersByFd_.at(m).name);
        out.push_back(std::move(st));
    }
    // 人数变化/房间解散 → 大厅列表刷新（推给所有大厅订阅者）
    Event list{EvType::RoomListPush};
    list.rooms = Snapshot();
    list.count = list.rooms.size();
    out.push_back(std::move(list));
    return out;
}

std::vector<RoomBrief> LobbyService::Snapshot() const {
    std::vector<RoomBrief> out;
    out.reserve(rooms_.size());
    for (const auto& [id, room] : rooms_) {
        out.push_back({room.roomId, room.name, room.memberFds.size(),
                       playersByFd_.at(room.memberFds.front()).name});
    }
    return out;
}

std::vector<Event> LobbyService::RoomStateAndList(uint32_t roomId) {
    std::vector<Event> out;
    auto& room = rooms_.at(roomId);
    Event st{EvType::RoomStatePush};
    st.roomId = roomId;
    st.hostName = playersByFd_.at(room.memberFds.front()).name;
    for (int m : room.memberFds) st.members.push_back(playersByFd_.at(m).name);
    out.push_back(std::move(st));
    Event list{EvType::RoomListPush};
    list.rooms = Snapshot();
    list.count = list.rooms.size();
    out.push_back(std::move(list));
    return out;
}

// —— 意图接口 ——

void LobbyService::OnConnected(int fd, int64_t nowMs) {
    graceByFd_[fd] = nowMs;                         // 宽限期起点（SweepLoginTimeouts 清理）
}

std::vector<Event> LobbyService::Login(int fd, const std::string& name) {
    std::vector<Event> out;
    if (!ValidLoginName(name)) {
        out.push_back({EvType::LoginFail, fd, 0, 0, 2});
        return out;                                     // 名字非法：宽限期照旧（可重试）
    }

    // ① 同名顶号：旧连接 KICKED(1) → 走离房/注销（D2 顶号；同名者只可能是别的 fd）
    if (auto it = fdByName_.find(name); it != fdByName_.end() && it->second != fd) {
        const int oldFd = it->second;
        out.push_back({EvType::Kicked, oldFd, 0, 0, 1});
        auto leaveEvents = LeaveInternal(oldFd);
        out.insert(out.end(), leaveEvents.begin(), leaveEvents.end());
        RemovePlayer(oldFd);
    }
    // ② 同 fd 重登录：先按断线自清（离房 + 注销旧名）
    if (playersByFd_.count(fd)) {
        auto leaveEvents = LeaveInternal(fd);
        out.insert(out.end(), leaveEvents.begin(), leaveEvents.end());
        RemovePlayer(fd);
    }

    Player p{nextPlayerId_++, name, fd};
    playersByFd_.emplace(fd, p);
    fdByName_.emplace(name, fd);
    graceByFd_.erase(fd);                               // 登录成功 → 出宽限期

    Event ok{EvType::LoginOk, fd, p.playerId};
    out.push_back(ok);
    Event list{EvType::RoomListPush};                   // 新大厅订阅者的首次全量快照
    list.rooms = Snapshot();
    list.count = list.rooms.size();
    out.push_back(std::move(list));
    return out;
}

std::vector<Event> LobbyService::CreateRoom(int fd, const std::string& roomName) {
    std::vector<Event> out;
    auto pit = playersByFd_.find(fd);
    if (pit == playersByFd_.end()) { out.push_back({EvType::CreateFail, fd, 0, 0, 1}); return out; }
    if (roomByFd_.count(fd))        { out.push_back({EvType::CreateFail, fd, 0, 0, 2}); return out; }
    if (!ValidRoomName(roomName))   { out.push_back({EvType::CreateFail, fd, 0, 0, 3}); return out; }  // 房间名非法

    Room room{nextRoomId_++, roomName};
    room.memberFds.push_back(fd);                       // 建房者首任房主（memberFds 首位）
    const uint32_t roomId = room.roomId;
    rooms_.emplace(roomId, std::move(room));
    roomByFd_.emplace(fd, roomId);

    out.push_back({EvType::CreateOk, fd, 0, roomId});
    auto pushes = RoomStateAndList(roomId);
    out.insert(out.end(), pushes.begin(), pushes.end());
    return out;
}

std::vector<Event> LobbyService::Join(int fd, uint32_t roomId) {
    std::vector<Event> out;
    auto pit = playersByFd_.find(fd);
    if (pit == playersByFd_.end()) { out.push_back({EvType::JoinFail, fd, 0, 0, 1}); return out; }
    auto rit = rooms_.find(roomId);
    if (rit == rooms_.end())       { out.push_back({EvType::JoinFail, fd, 0, roomId, 2}); return out; }
    if (roomByFd_.count(fd))       { out.push_back({EvType::JoinFail, fd, 0, roomId, 4}); return out; }
    Room& room = rit->second;
    if (room.full())               { out.push_back({EvType::JoinFail, fd, 0, roomId, 3}); return out; }

    room.memberFds.push_back(fd);
    roomByFd_.emplace(fd, roomId);
    out.push_back({EvType::JoinOk, fd, 0, roomId, 0, room.memberFds.size()});
    auto pushes = RoomStateAndList(roomId);
    out.insert(out.end(), pushes.begin(), pushes.end());
    return out;
}

std::vector<Event> LobbyService::Leave(int fd) {
    std::vector<Event> out;
    auto rit = roomByFd_.find(fd);
    if (rit == roomByFd_.end() || playersByFd_.count(fd) == 0) return out;  // 不在房中：无事发生
    out = LeaveInternal(fd);
    return out;
}

std::vector<Event> LobbyService::Disconnect(int fd) {
    std::vector<Event> out;
    auto leaveEvents = LeaveInternal(fd);               // 在房中：同 Leave 路径（D2-16）
    out.insert(out.end(), leaveEvents.begin(), leaveEvents.end());
    RemovePlayer(fd);                                   // 注销（不广播 Kicked——连接已死，无事可发）
    return out;
}

std::vector<Event> LobbyService::SweepLoginTimeouts(int64_t nowMs) {
    std::vector<Event> out;
    for (auto it = graceByFd_.begin(); it != graceByFd_.end();) {
        if (nowMs - it->second > kLoginGraceMs) {
            out.push_back({EvType::Kicked, it->first, 0, 0, 2});   // 适配层负责断开
            it = graceByFd_.erase(it);
        } else {
            ++it;
        }
    }
    return out;
}

// —— 适配器查询 ——

bool LobbyService::IsLoggedIn(int fd) const { return playersByFd_.count(fd) != 0; }

std::vector<int> LobbyService::LobbySubscribers() const {
    std::vector<int> fds;
    for (const auto& [fd, p] : playersByFd_)
        if (roomByFd_.count(fd) == 0) fds.push_back(fd);
    return fds;
}

std::vector<int> LobbyService::RoomMemberFds(uint32_t roomId) const {
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) return {};
    return it->second.memberFds;                    // 按加入顺序；首位 = 房主
}

} // namespace sightline::lobby
