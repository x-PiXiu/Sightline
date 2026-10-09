// lobby.h —— 第 2 期业务层（整洁架构内两层：实体 + 用例）
// 依赖规则：本目录零 include net/* —— LobbyService 不知道 socket 的存在，
//           一切"该通知谁"以领域事件返回，由外层 FrameWriter 翻译成帧（可脱离网络纯单测）。
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace sightline::lobby {

// ===== 实体：是什么（企业级规则，即使没有软件也存在）=====
constexpr size_t kMaxRoomMembers = 4;
constexpr int64_t kLoginGraceMs = 60000;   // 登录前宽限期（60s：够玩家从容输入名字；仍拦截死连接）
constexpr size_t kMaxNameBytes = 16;       // UTF-8 字节数
constexpr size_t kMaxRoomNameBytes = 24;

struct Player {
    uint32_t playerId = 0;
    std::string name;          // 全服唯一（服务器查重）
    int connFd = 0;            // 回包通道（适配层知道它是什么）
};

struct Room {
    uint32_t roomId = 0;
    std::string name;                   // 允许重名（身份 = RoomId）
    std::vector<int> memberFds;         // 按加入顺序 —— **首位即现任房主**（房主转移由删除自动成立）
    static constexpr size_t kMaxMembers = kMaxRoomMembers;
    bool full() const { return memberFds.size() >= kMaxMembers; }
    bool empty() const { return memberFds.empty(); }
};

// 名字合法性：非空、≤16B（两处调用：登录校验 / 房间名 ≤24B 走同函数传上限）
bool ValidName(const std::string& name, size_t maxBytes);

// ===== 领域事件：用例的输出（FrameWriter 翻译成 S2C 帧）=====
enum class EvType {
    LoginOk,          // → 5  [code0][playerId]
    LoginFail,        // → 5  code: 1 重名 / 2 名字非法
    CreateOk,         // → 7  [code0][roomId]
    CreateFail,       // → 7  code: 1 未登录 / 2 已在房间
    JoinOk,           // → 9  [code0][roomId][count]
    JoinFail,         // → 9  code: 1 未登录 / 2 不存在 / 3 已满 / 4 已在房间
    Kicked,           // → 13 code: 1 顶号 / 2 登录超时（随后适配层断开该 fd）
    RoomListPush,     // → 11 全量快照 → 发给所有"已登录未进房"连接
    RoomStatePush,    // → 12 成员快照 → 发给该房间所有成员
};

struct RoomBrief {                  // ROOM_LIST 的每一项
    uint32_t roomId = 0;
    std::string roomName;
    size_t memberCount = 0;
    std::string hostName;           // memberIds 首位玩家的名字
};

struct Event {
    EvType type;
    int fd = 0;                     // 该事件发给谁（广播类事件由适配层展开）
    uint32_t playerId = 0, roomId = 0;
    unsigned code = 0;
    size_t count = 0;               // JoinOk 在场人数 / RoomListPush 房间数
    std::vector<RoomBrief> rooms;   // RoomListPush
    std::string hostName;           // RoomStatePush
    std::vector<std::string> members;  // RoomStatePush（按加入顺序，首位=房主）
};

// ===== 用例：系统怎么被使用（应用级业务规则）=====
class LobbyService {
public:
    // —— 意图接口（一一对应 C2S 帧）——
    void OnConnected(int fd, int64_t nowMs);     // Acceptor 新连接：登记登录宽限期起点
    std::vector<Event> Login(int fd, const std::string& name);
    std::vector<Event> CreateRoom(int fd, const std::string& roomName);
    std::vector<Event> Join(int fd, uint32_t roomId);
    std::vector<Event> Leave(int fd);
    // —— 连接断开：与 Leave 同路径（D2-16），另注销玩家与宽限期登记 ——
    std::vector<Event> Disconnect(int fd);
    // —— 帧驱动每 tick 调用：清理登录宽限超时（时间由外层注入，纯函数可测）——
    std::vector<Event> SweepLoginTimeouts(int64_t nowMs);

    // —— 适配器查询（FrameWriter 编码用）——
    bool IsLoggedIn(int fd) const;
    std::vector<int> LobbySubscribers() const;   // 已登录且未进房：ROOM_LIST 广播目标
    std::vector<int> RoomMemberFds(uint32_t roomId) const;  // 房内成员 fd：ROOM_STATE 投递目标

private:
    std::vector<Event> LeaveInternal(int fd);    // 离房（Leave/Disconnect/顶号共用）
    std::vector<Event> RoomStateAndList(uint32_t roomId);  // 成员变化后的标准推送组
    std::vector<RoomBrief> Snapshot() const;     // 全量房间快照（列表推送的载荷）
    void RemovePlayer(int fd);                   // 注销玩家 + 名字索引

    std::unordered_map<int, Player> playersByFd_;
    std::unordered_map<std::string, int> fdByName_;       // 名字 → fd（查重/顶号）
    std::unordered_map<int, uint32_t> roomByFd_;          // fd → RoomId（在房中才有）
    std::unordered_map<int, int64_t> graceByFd_;          // 未登录连接 → 接入时刻
    std::unordered_map<uint32_t, Room> rooms_;
    uint32_t nextPlayerId_ = 1;
    uint32_t nextRoomId_ = 1;
};

} // namespace sightline::lobby
