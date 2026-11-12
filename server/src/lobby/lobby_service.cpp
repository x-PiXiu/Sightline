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
        for (auto it2 = bots_.begin(); it2 != bots_.end();) {   // ep07：席位随房清（❗12）
            if (it2->roomId == roomId) it2 = bots_.erase(it2);
            else ++it2;
        }
        roomInMatch_.erase(roomId);
    } else {
        // 房内推送：房主 = memberFds 首位（房主转移由"按加入顺序"自动成立）
        Event st{EvType::RoomStatePush};
        st.roomId = roomId;
        st.hostName = playersByFd_.at(room.memberFds.front()).name;
        for (int m : room.memberFds) st.members.push_back(playersByFd_.at(m).name);
    for (const auto& b : bots_) if (b.roomId == roomId) st.members.push_back(b.name);   // ep07：人机上榜
        out.push_back(std::move(st));
    }
    // 人数变化/房间解散 → 大厅列表刷新（推给所有大厅订阅者）
    Event list{EvType::RoomListPush};
    list.rooms = Snapshot();
    list.count = list.rooms.size();
    out.push_back(std::move(list));
    RebuildSnapshot_();                               // ep09 G3：出房/解散→快照重建
    return out;
}

std::vector<RoomBrief> LobbyService::Snapshot() const {
    std::vector<RoomBrief> out;
    out.reserve(rooms_.size());
    for (const auto& [id, room] : rooms_) {
        size_t cnt = room.memberFds.size();
        for (const auto& b : bots_) if (b.roomId == room.roomId) ++cnt;   // ep07：人机占席计入
        out.push_back({room.roomId, room.name, cnt,
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
    for (const auto& b : bots_) if (b.roomId == roomId) st.members.push_back(b.name);   // ep07：人机上榜
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
    list.fd = fd;                                       // 单发给新订阅者（fd≠0=unicast；登录不改房间列表，
    list.rooms = Snapshot();                            //  广播是对端无关的冗余——5k 并发实测 O(N²) 拖垮登录，2026-10-23 修）
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
    ++pit->second.roomEpoch;                            // ep09 G1：进房自增
    RebuildSnapshot_();                                 // ep09 G3

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
    // ep07 ❗11 契约兑现（ep09 e2e 抓出的实现缺口——lobby.h:125 注释承诺但本函数无此分支）：
    // 对局中的房不收人（码 5）。位置=已满(3)之前——对局中房即使未满同样拒。
    if (RoomInMatch(roomId))       { out.push_back({EvType::JoinFail, fd, 0, roomId, 5}); return out; }
    Room& room = rit->second;
    if (room.full())               { out.push_back({EvType::JoinFail, fd, 0, roomId, 3}); return out; }

    room.memberFds.push_back(fd);
    roomByFd_.emplace(fd, roomId);
    ++pit->second.roomEpoch;                            // ep09 G1：进房自增（旧房在途输入判旧）
    RebuildSnapshot_();                                 // ep09 G3：成员表变更→快照重建
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

std::vector<Event> LobbyService::ShutdownBroadcast() {
    // 优雅关闭（D3-19）：在线玩家 + 宽限期连接各得一条 Kicked(码3)——语义在用例，搬运归 FrameWriter
    std::vector<Event> out;
    for (const auto& [fd, p] : playersByFd_) {
        (void)p;
        out.push_back({EvType::Kicked, fd, 0, 0, 3});
    }
    for (const auto& [fd, grace] : graceByFd_) {
        (void)grace;
        out.push_back({EvType::Kicked, fd, 0, 0, 3});
    }
    return out;
}

// —— 对局编排查询（D3-15：纯查询零副作用）——

bool LobbyService::IsHost(int fd) const {
    const auto rit = roomByFd_.find(fd);
    if (rit == roomByFd_.end()) return false;
    const auto roomIt = rooms_.find(rit->second);
    return roomIt != rooms_.end() && !roomIt->second.memberFds.empty()
        && roomIt->second.memberFds.front() == fd;
}

bool LobbyService::RoomIdByFd(int fd, uint32_t& outRoomId) const {
    const auto rit = roomByFd_.find(fd);
    if (rit == roomByFd_.end()) return false;
    outRoomId = rit->second;
    return true;
}

int LobbyService::PlayerIdByFd(int fd) const {
    const auto pit = playersByFd_.find(fd);
    return pit == playersByFd_.end() ? -1 : static_cast<int>(pit->second.playerId);
}

bool LobbyService::RoomIdByPid(uint32_t pid, uint32_t& outRoomId) const {
    uint32_t epoch = 0;
    return LocateFromSnapshot(pid, outRoomId, epoch);
}

bool LobbyService::LocateFromSnapshot(uint32_t pid, uint32_t& outRoomId, uint32_t& outEpoch) const {
    const auto snap = snap_.load(std::memory_order_acquire);   // 原子取当前快照（零锁读）
    const auto it = snap->pidRoomEpoch.find(pid);
    if (it == snap->pidRoomEpoch.end()) return false;          // 不在房/人机外未知 pid——在途判旧即丢
    outRoomId = it->second.first;
    outEpoch = it->second.second;
    return true;
}

void LobbyService::RebuildSnapshot_() {
    auto snap = std::make_shared<LobbySnapshot>();   // 构建期非 const，store 时隐式转 const
    for (const auto& [rid, room] : rooms_) {
        for (const int fd : room.memberFds) {
            const auto pit = playersByFd_.find(fd);
            if (pit != playersByFd_.end())
                snap->pidRoomEpoch.emplace(pit->second.playerId,
                                           std::make_pair(rid, pit->second.roomEpoch));
        }
        for (const auto& b : bots_)
            if (b.roomId == rid)
                snap->pidRoomEpoch.emplace(b.playerId, std::make_pair(rid, 1u));   // 人机恒 epoch=1
    }
    snap_.store(std::move(snap), std::memory_order_release);
}

// —— ep07：人机席位 ——
int LobbyService::AddBot(uint32_t roomId) {
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) return -1;
    auto& room = it->second;
    size_t botCount = 0;
    for (const auto& b : bots_) if (b.roomId == roomId) ++botCount;
    if (room.memberFds.size() + botCount >= kMaxRoomMembers) return -1;   // 4 人上限（人机占席）
    if (RoomInMatch(roomId)) return -1;                                // 对局中不加人
    const uint32_t pid = nextPlayerId_++;                              // fd=-1：无连接（天然收不到 S2C）
    // 命名带房间内序号（2026-11-03 用户令：三人机都叫"Bot"→ 播报"Bot 击杀了 Bot"分不清谁是谁）
    const std::string name = "Bot" + std::to_string(botCount + 1);
    bots_.push_back({pid, name, roomId});
    RebuildSnapshot_();                                 // ep09 G3：人机席位进快照（恒 epoch=1）
    return static_cast<int>(pid);
}

std::vector<Event> LobbyService::RemoveBot(uint32_t botPid) {
    std::vector<Event> out;
    for (auto it = bots_.begin(); it != bots_.end(); ++it)
        if (it->playerId == botPid) {
            const uint32_t roomId = it->roomId;
            bots_.erase(it);
            if (rooms_.count(roomId)) out = RoomStateAndList(roomId);   // 成员变化 → 标准推送
            return out;
        }
    RebuildSnapshot_();                               // ep09 G3
    return out;
}

bool LobbyService::IsBot(uint32_t pid) const {
    for (const auto& b : bots_) if (b.playerId == pid) return true;
    return false;
}

bool LobbyService::HasRoom(uint32_t roomId) const { return rooms_.count(roomId) != 0; }

void LobbyService::SetRoomInMatch(uint32_t roomId, bool inMatch) {
    if (rooms_.count(roomId)) roomInMatch_[roomId] = inMatch;
}

bool LobbyService::RoomInMatch(uint32_t roomId) const {
    auto it = roomInMatch_.find(roomId);
    return it != roomInMatch_.end() && it->second;
}

std::vector<RoomMemberInfo> LobbyService::RoomRoster(uint32_t roomId) const {
    std::vector<RoomMemberInfo> out;
    const auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) return out;
    out.reserve(roomIt->second.memberFds.size());
    for (int fd : roomIt->second.memberFds) {
        const auto pit = playersByFd_.find(fd);
        if (pit != playersByFd_.end()) out.push_back({pit->second.playerId, pit->second.name});
    }
    for (const auto& b : bots_)                          // ep07：人机排尾部（名册序=出生点序的延续）
        if (b.roomId == roomId) out.push_back({b.playerId, b.name});
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
