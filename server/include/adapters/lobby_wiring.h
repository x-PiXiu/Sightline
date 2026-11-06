// lobby_wiring.h —— 第 2 期接口适配器：MessageDispatcher（请求分发）+ FrameWriter（事件出站）
// 依赖方向：本文件（适配器）→ 用例（LobbyService）+ 框架（net）——内层 lobby/ 对这里一无所知
// 职责边界：
//   MessageDispatcher = 注册表 msgid → Handler（开闭：新消息只注册不改旧代码）
//   FrameWriter       = 领域事件 → S2C 帧（MSG_IDS v2 编码唯一落点；KICKED 事件附带断开）
#pragma once
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "application/config_views.h"   // ep08：30 号只读视图端口（适配器→用例，方向向内）
#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/match_service.h"   // M3：FrameWriter 翻译对局事件（适配器→用例，方向向内）
#include "net/client_connection.h"
#include "protocol/messages.h"   // v4 wire 结构体（Encode 唯一落点）
#include "protocol/msg_ids.h"

namespace adapters {

using net::ClientConnection;
using ConnTable = std::map<int, ClientConnection::Ptr>;

// ===== 出站：领域事件 → S2C 帧 =====
class FrameWriter {
public:
    FrameWriter(ConnTable& conns, const sightline::lobby::LobbyService& lobby)
        : conns_(conns), lobby_(lobby) {}

    // ep08 G5：30 号载荷组装 = 数据格式翻译 = 适配器职责；只读视图由组合根注入。
    void SetConfigVersion(const sightline::application::IConfigVersion* v) { configVersion_ = v; }

    void BroadcastConfigVersion() {
        if (!configVersion_) return;
        const auto snap = configVersion_->Snapshot();
        sightline::wire::ConfigVersionMsg msg;
        msg.version = snap.version;
        for (const auto& [type, w] : snap.weapons)
            msg.weapons.push_back({static_cast<std::uint8_t>(type), w.damage, w.mag,
                                   w.reloadMs, w.fireMs});
        const std::string p = msg.Encode();
        for (const auto& [fd, conn] : conns_) {
            (void)conn;
            send(fd, protocol::MSG_S2C_CONFIG_VERSION, p);   // 开局 / reload / patch 后全员同步
        }
    }

    void deliver(const std::vector<sightline::lobby::Event>& events) {
        using ET = sightline::lobby::EvType;
        for (const auto& e : events) {
            switch (e.type) {
            case ET::LoginOk:
                send(e.fd, protocol::MSG_S2C_LOGIN_RESULT,
                     std::string{char(0)} + u32(e.playerId));
                LOG_DEBUG("[出站] LOGIN_RESULT 成功 -> fd=" + std::to_string(e.fd) +
                          " pid=" + std::to_string(e.playerId));
                break;
            case ET::LoginFail:
                send(e.fd, protocol::MSG_S2C_LOGIN_RESULT, std::string{char(e.code)} + u32(0));
                LOG_INFO("[出站] LOGIN_RESULT 失败 码=" + std::to_string(e.code) + " -> fd=" +
                         std::to_string(e.fd));
                break;
            case ET::CreateOk:
                send(e.fd, protocol::MSG_S2C_CREATE_RESULT,
                     std::string{char(0)} + u32(e.roomId));
                LOG_DEBUG("[出站] CREATE_RESULT 成功 -> fd=" + std::to_string(e.fd) +
                          " 房间=" + std::to_string(e.roomId));
                break;
            case ET::CreateFail:
                send(e.fd, protocol::MSG_S2C_CREATE_RESULT, std::string{char(e.code)} + u32(0));
                LOG_INFO("[出站] CREATE_RESULT 失败 码=" + std::to_string(e.code) + " -> fd=" +
                         std::to_string(e.fd));
                break;
            case ET::JoinOk:
                send(e.fd, protocol::MSG_S2C_JOIN_RESULT,
                     std::string{char(0)} + u32(e.roomId) + char(e.count));
                LOG_DEBUG("[出站] JOIN_RESULT 成功 -> fd=" + std::to_string(e.fd) +
                          " 房间=" + std::to_string(e.roomId) + " 人数=" + std::to_string(e.count));
                break;
            case ET::JoinFail:
                send(e.fd, protocol::MSG_S2C_JOIN_RESULT,
                     std::string{char(e.code)} + u32(e.roomId));
                LOG_INFO("[出站] JOIN_RESULT 失败 码=" + std::to_string(e.code) + " -> fd=" +
                         std::to_string(e.fd));
                break;
            case ET::Kicked:
                send(e.fd, protocol::MSG_S2C_KICKED, std::string{char(e.code)});
                LOG_DEBUG("[出站] KICKED 码=" + std::to_string(e.code) + " -> fd=" +
                          std::to_string(e.fd) + " 并断开");
                forceClose(e.fd);                       // KICKED 之后服务器主动断开（契约）
                break;
            case ET::RoomListPush: {
                LOG_DEBUG("[广播] ROOM_LIST 房间数=" + std::to_string(e.count) +
                          " -> 大厅订阅者 " + std::to_string(lobby_.LobbySubscribers().size()) + " 人");
                std::string payload = u16(e.count);
                for (const auto& r : e.rooms) {
                    payload += u32(r.roomId) + char(r.memberCount)
                             + lenPrefixed(r.roomName) + lenPrefixed(r.hostName);
                }
                if (e.fd > 0) {
                    send(e.fd, protocol::MSG_S2C_ROOM_LIST, payload);  // fd>0：登录首照单发（非广播）
                } else {
                    broadcastLobby(payload);            // fd=0：房间变化 → 所有"已登录未进房"连接
                }
                break;
            }
            case ET::RoomStatePush: {
                LOG_DEBUG("[广播] ROOM_STATE 房间=" + std::to_string(e.roomId) +
                          " 成员数=" + std::to_string(e.members.size()) +
                          " -> 房内 " + std::to_string(lobby_.RoomMemberFds(e.roomId).size()) + " 人");
                std::string payload = u32(e.roomId) + lenPrefixed(e.hostName) + char(e.members.size());
                for (const auto& m : e.members) payload += lenPrefixed(m);
                multicastRoom(e.roomId, payload);       // 发给房内所有成员
                break;
            }
            }
        }
    }

    // 对局事件 → 15/17/18 帧（v3 契约；翻译唯一落点，与 deliver 同一职责线）
    void deliverMatch(const std::vector<sightline::match::MatchEvent>& events) {
        using MT = sightline::match::MatchEvent::Type;
        for (const auto& e : events) {
            switch (e.type) {
            case MT::MatchStart: {
                sightline::wire::MatchStartMsg msg;               // v4：Encode 同居 wire 头
                for (const auto& r : e.roster)
                    msg.roster.push_back({r.playerId, r.name, r.spawnIndex});
                const std::string p = msg.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    send(fd, protocol::MSG_S2C_MATCH_START, p);
                LOG_INFO("[出站] MATCH_START 房=" + std::to_string(e.roomId) +
                         " 名册=" + std::to_string(e.roster.size()));
                BroadcastConfigVersion();                   // ep08：15 号后广播 30 号（开局参数同步）
                break;
            }
            case MT::EntityLeave: {
                const std::string p = sightline::wire::EntityLeaveMsg{e.leavePlayerId}.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    if (lobby_.PlayerIdByFd(fd) != static_cast<int>(e.leavePlayerId))  // 不含当事人
                        send(fd, protocol::MSG_S2C_ENTITY_LEAVE, p);
                LOG_INFO("[出站] ENTITY_LEAVE pid=" + std::to_string(e.leavePlayerId));
                break;
            }
            case MT::Kill: {                       // ep07：23 号击杀事件（房内全员·播报+比分源）
                const std::string p = sightline::wire::KillMsg{
                    e.killVictim, e.killKiller, e.respawnDelayMs}.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    send(fd, protocol::MSG_S2C_KILL, p);
                LOG_INFO("[出站] KILL victim=" + std::to_string(e.killVictim) +
                         " killer=" + std::to_string(e.killKiller));
                break;
            }
            case MT::Respawn: {                    // v4.5：24 号（权威落位坐标下发——客户端不再自查表传送）
                const std::string p = sightline::wire::RespawnMsg{
                    e.respawnPid, e.spawnIndex, e.invulnMs,
                    e.respawnX, e.respawnY, e.respawnZ, e.respawnYawDeg}.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    send(fd, protocol::MSG_S2C_RESPAWN, p);
                LOG_INFO("[出站] RESPAWN pid=" + std::to_string(e.respawnPid) +
                         " spawnIx=" + std::to_string(e.spawnIndex) +
                         " @(" + std::to_string(e.respawnX) + "," + std::to_string(e.respawnY) + ")");
                break;
            }
            case MT::Ended: {                      // ep07：25 号结算（比分行）
                sightline::wire::MatchEndMsg msg;
                msg.reason = e.endReason;
                for (const auto& r : e.scores)
                    msg.rows.push_back({r.playerId, r.kills, r.deaths});
                const std::string p = msg.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    send(fd, protocol::MSG_S2C_MATCH_END, p);
                LOG_INFO("[出站] MATCH_END 房=" + std::to_string(e.roomId) +
                         " 原因=" + std::to_string(e.endReason) + " 行数=" +
                         std::to_string(msg.rows.size()));
                break;
            }
            case MT::Snapshot: {
                // 按接收者编码。v4.1（M3b 和解对账）：**含接收者自己**——自实体的权威位+ackedSeq
                // 即客户端回滚/重放的对账基准（R12 权威收回后，自实体=老师批改后的标准答案）；
                // v3 直发转发版的滤自己废止（契约 v4.1 登记；设计草案和解状态机的必要输入）
                // ⚠ 计数必须 = 实际写入条数（数清再写头）——声明的 n 与实体不一致会导致客户端幻影解析
                for (int fd : lobby_.RoomMemberFds(e.roomId)) {
                    std::string body;
                    int n = 0;
                    for (const auto& S : e.entities) {
                        ++n;
                        // v4：29B/实体（+4B ackedSeq）——Encode 同居 wire 头（契约破例 D4-01）
                        sightline::wire::EntityStateV2 ent{S.playerId, S.x, S.y, S.z,
                                                           S.yaw, S.pitch, S.flags, S.ackedSeq};
                        body += ent.Encode();
                    }
                    std::string p = u16(static_cast<uint16_t>(n));  // 头仍 2B 计数，实体 25→29B
                    p += body;
                    send(fd, protocol::MSG_S2C_ENTITY_STATE, p);
                }
                break;
            }
            case MT::FireResult: {
                // 诊断行（每次开火一条全参数判定链：门/眼位/方向/命中距离/余血）
                static const char* GateName[] = {"Ok", "换弹中", "弹匣空", "射速限制", "死亡/终局"};
                const auto& F = e.fire;
                const auto& D = e.fireDiag;   // 判定链诊断快照（2026-11-04 拆分——判定行专用）
                if (!F.bAccepted) {
                    LOG_INFO("[射击拒绝] shooter=" + std::to_string(F.shooterPid) +
                              " 原因=" + GateName[F.gate % 5]);
                    // 2026-11-05 弹匣漂移根治（D5-03 修订）：被拒射击回传射手本人（21 号+gate）——
                    // 客户端撤销本地预测扣弹=双端弹匣/换弹钟锁相（"死窗"族灭绝）。广播全员无害：
                    // 他人客户端按"非自机+gate 过滤"；人机的拒绝在驱动器已就地丢弃不进此路径。
                    const sightline::wire::FireResultMsg m{
                        F.shooterPid, 0, 0, 0.f, 0.f, 0.f, 0, F.gate};
                    const std::string p = m.Encode();
                    for (int fd : lobby_.RoomMemberFds(e.roomId))
                        send(fd, protocol::MSG_S2C_FIRE_RESULT, p);
                    break;
                }
                std::string judgeLine = "[射击判定] shooter=" + std::to_string(F.shooterPid) +
                          " 眼位=(" + std::to_string(D.oX) + "," + std::to_string(D.oY) + "," + std::to_string(D.oZ) + ")" +
                          " 方向=(" + std::to_string(D.dX) + "," + std::to_string(D.dY) + "," + std::to_string(D.dZ) + ")" +
                          " hit=" + std::to_string(F.bHit) +
                          " victim=" + std::to_string(F.victimPid) +
                          " dist=" + std::to_string(D.dist) +
                          " hp=" + std::to_string(F.victimHp) +
                          " 射手位=(" + std::to_string(D.shooterX) + "," + std::to_string(D.shooterY) + "," + std::to_string(D.shooterZ) + ")" +
                          " 受害位=(" + std::to_string(D.victimX) + "," + std::to_string(D.victimY) + "," + std::to_string(D.victimZ) + ")" +
                          " 命中体=(r=" + std::to_string(D.hbRadius) + " 半高=" + std::to_string(D.hbHalfH) + ")" +
                          " 回溯=" + std::to_string(D.rewindMs) + "ms";
                // miss 诊断：最近候选（回溯采样位+垂距）——"打不中"复盘直接可读目标方位与偏差量；
                // [无敌] 旗=该目标仅因重生 2s 保护未被判定（2026-11-04 实锤：完美瞄准 0.2° 打无敌=零反馈）
                if (!F.bHit && D.nearPid != 0)
                    judgeLine += " 最近候选=pid" + std::to_string(D.nearPid) +
                                 " 垂距=" + std::to_string(D.nearPerp) + "cm 位=(" +
                                 std::to_string(D.nearX) + "," + std::to_string(D.nearY) + "," +
                                 std::to_string(D.nearZ) + ")" +
                                 (D.nearInvuln ? " [无敌——重生保护内·命中无效]" : "");
                // 人机每个约 10 发/秒：判定行降 TRACE（真人仍 INFO 整链可见；击杀 23 号恒 INFO）
                if (lobby_.IsBot(F.shooterPid)) LOG_TRACE(judgeLine);
                else LOG_INFO(judgeLine);
                // v4.2：21 号房内全员广播（**含射手自己**——射手要出 Hitmarker；其余画 tracer；
                // 受击者红闪）——一条消息三方共用（D5-02v2；翻译唯一落点纪律同 15/17/18）
                const sightline::wire::FireResultMsg m{
                    F.shooterPid, F.bHit ? std::uint8_t{1} : std::uint8_t{0},
                    F.victimPid, F.hitX, F.hitY, F.hitZ, F.victimHp};
                const std::string p = m.Encode();
                for (int fd : lobby_.RoomMemberFds(e.roomId))
                    send(fd, protocol::MSG_S2C_FIRE_RESULT, p);
                break;
            }
            }
        }
    }

private:
    static std::string u32(uint32_t v) {
        std::string s;
        for (int i = 0; i < 4; ++i) s.push_back(char((v >> (8 * i)) & 0xFF));
        return s;
    }
    static std::string u16(uint16_t v) {
        return std::string{char(v & 0xFF), char((v >> 8) & 0xFF)};
    }
    static std::string lenPrefixed(const std::string& s) {
        return std::string{char(s.size())} + s;
    }

    ClientConnection::Ptr find(int fd) {
        auto it = conns_.find(fd);
        return it == conns_.end() ? nullptr : it->second;
    }
    void send(int fd, uint16_t msgid, const std::string& payload) {
        if (auto c = find(fd)) c->send(msgid, payload);  // 连接已死则静默跳过
    }
    void forceClose(int fd) {
        // M2：礼貌关闭——投归属线程执行收尾（epoll 永不跨线程；收尾仍经 closeCallback → Closed 入队 → main tick 清 map）
        if (auto c = find(fd)) c->closeAsync();
    }
    void broadcastLobby(const std::string& payload) {
        for (int fd : lobby_.LobbySubscribers()) send(fd, protocol::MSG_S2C_ROOM_LIST, payload);
    }
    void multicastRoom(uint32_t roomId, const std::string& payload) {
        for (int fd : lobby_.RoomMemberFds(roomId)) send(fd, protocol::MSG_S2C_ROOM_STATE, payload);
    }

    ConnTable& conns_;
    const sightline::lobby::LobbyService& lobby_;
    const sightline::application::IConfigVersion* configVersion_ = nullptr;   // ep08：可空（未注入时不发 30 号）
};

// ===== 入站：msgid → Handler 注册表（开闭原则：新消息只注册）=====
// 同一 msgid 可挂多个 Handler（M3：10 号的"对局离场钩子"与大厅离开共存），
// 执行顺序 = 注册顺序——顺序敏感的钩子（如 fd→PlayerId 翻译须先于 lobby 移除）应先注册
class MessageDispatcher {
public:
    using Handler = std::function<void(int fd, const std::string& payload)>;
    struct Entry {                       // 注册表条目：功能名 + 高频帧降级标记 + 处理器
        std::string name;
        bool trace = false;              // 心跳这类高频帧降 TRACE，不刷屏
        Handler handler;
    };

    void on(uint16_t msgid, std::string name, Handler h, bool trace = false) {
        handlers_[msgid].push_back(Entry{std::move(name), trace, std::move(h)});
    }

    void dispatch(int fd, uint16_t msgid, const std::string& payload) {
        auto it = handlers_.find(msgid);
        if (it == handlers_.end()) {
            LOG_WARNING("[未知消息] msgid=" + std::to_string(msgid) +
                        " 长度=" + std::to_string(payload.size()) +
                        " 来自 fd=" + std::to_string(fd) + " —— 已忽略");
            return;
        }
        for (const auto& entry : it->second) {
            const std::string line = "[收帧·" + entry.name + "] fd=" +
                                     std::to_string(fd) + " msgid=" + std::to_string(msgid) +
                                     " 字节=" + std::to_string(payload.size());
            if (entry.trace) LOG_TRACE(line); else LOG_DEBUG(line);
            entry.handler(fd, payload);
        }
    }

private:
    std::map<uint16_t, std::vector<Entry>> handlers_;
};

// ===== 请求侧注册（适配器职责：payload 解析 + 调用例）=====
// main 只调用本函数完成装配，自己不再携带任何 handler 逻辑
inline void wireLobby(MessageDispatcher& disp, sightline::lobby::LobbyService& lobby,
                      FrameWriter& writer, ConnTable& conns) {
    using namespace sightline::lobby;

    // 心跳 echo（第 1 期契约 2/3，一个字节不改）
    disp.on(protocol::MSG_C2S_HEARTBEAT, "心跳", [&conns](int fd, const std::string& payload) {
        if (payload.size() != 12) {
            LOG_WARNING("[HEARTBEAT] 长度异常 " + std::to_string(payload.size()) + " fd=" +
                        std::to_string(fd));
            return;
        }
        auto it = conns.find(fd);
        if (it == conns.end()) return;
        std::string ack = payload;                        // 原样回显 seq + clientTs
        const auto now = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        for (int i = 0; i < 8; ++i) ack.push_back(char((now >> (8 * i)) & 0xFF));  // 附加 serverTs
        it->second->send(protocol::MSG_S2C_HEARTBEAT_ACK, ack);
        const uint32_t seq = static_cast<uint32_t>(static_cast<unsigned char>(payload[0])) |
                             (static_cast<uint32_t>(static_cast<unsigned char>(payload[1])) << 8) |
                             (static_cast<uint32_t>(static_cast<unsigned char>(payload[2])) << 16) |
                             (static_cast<uint32_t>(static_cast<unsigned char>(payload[3])) << 24);
        LOG_TRACE("[HEARTBEAT] seq=" + std::to_string(seq) +
                  " from=" + it->second->peerAddr().toIpPort());   // TRACE：不刷屏；第 1 期验收镜头录制时临时调回 INFO
    });
    // 登录
    disp.on(protocol::MSG_C2S_LOGIN, "登录", [&lobby, &writer](int fd, const std::string& payload) {
        if (payload.empty()) return;
        const auto n = static_cast<unsigned char>(payload[0]);
        if (payload.size() < 1u + n) return;
        const std::string name = payload.substr(1, n);
        LOG_DEBUG("[LOGIN] fd=" + std::to_string(fd) + " 名字=" + name);
        writer.deliver(lobby.Login(fd, name));
    });
    // 建房
    disp.on(protocol::MSG_C2S_CREATE_ROOM, "建房", [&lobby, &writer](int fd, const std::string& payload) {
        if (payload.empty()) return;
        const auto m = static_cast<unsigned char>(payload[0]);
        if (payload.size() < 1u + m) return;
        LOG_DEBUG("[CREATE_ROOM] fd=" + std::to_string(fd) + " 房名字节=" + std::to_string(m));
        writer.deliver(lobby.CreateRoom(fd, payload.substr(1, m)));
    });
    // 进房
    disp.on(protocol::MSG_C2S_JOIN_ROOM, "进房", [&lobby, &writer](int fd, const std::string& payload) {
        if (payload.size() < 4) return;
        uint32_t roomId = 0;
        for (int i = 0; i < 4; ++i)
            roomId |= static_cast<uint32_t>(static_cast<unsigned char>(payload[i])) << (8 * i);
        LOG_DEBUG("[JOIN_ROOM] fd=" + std::to_string(fd) + " 房间=" + std::to_string(roomId));
        writer.deliver(lobby.Join(fd, roomId));
    });
    // 离开房间
    disp.on(protocol::MSG_C2S_LEAVE_ROOM, "离开房间", [&lobby, &writer](int fd, const std::string&) {
        LOG_DEBUG("[LEAVE_ROOM] fd=" + std::to_string(fd));
        writer.deliver(lobby.Leave(fd));
    });
}

} // namespace adapters
