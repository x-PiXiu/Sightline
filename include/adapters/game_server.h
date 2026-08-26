// adapters/game_server.h —— 组装胶水：连接管理 + 消息分发 + IGameChannel 实现
// 依赖方向合法性的体现：本类（外层）include application 的服务并调用之，
// 而 application 对本类一无所知（只认 IGameChannel / ITimerScheduler 接口）。

#pragma once
#include <memory>
#include <unordered_map>
#include <vector>
#include "adapters/acceptor.h"
#include "adapters/tcp_connection.h"
#include "adapters/protocol_codec.h"
#include "application/dto.h"
#include "application/ports/i_game_channel.h"
#include "application/session_service.h"
#include "application/room_service.h"
#include "net/event_loop.h"
#include "logger/logger.h"

namespace sightline::adapters {

class GameServer : public app::IGameChannel {
public:
    GameServer(common::network::EventLoop& loop,
               app::SessionService& sessions,
               app::RoomService& rooms,
               uint16_t listen_port)
        : loop_(loop), sessions_(sessions), rooms_(rooms),
          acceptor_(&loop, common::network::InetAddress(listen_port)) {
        acceptor_.setNewConnectionCallback(
            [this](int fd, const common::network::InetAddress& peer) {
                onNewConnection(fd, peer);
            });
    }

    void start() { acceptor_.listen(); }

    // ---- IGameChannel 实现（application 经此把事件送回网络世界）----

    void sendTo(app::PlayerId pid, const app::GameEvent& ev) override {
        auto cit = connections_.find(sessions_.connIdOf(pid));
        if (cit == connections_.end()) return;
        cit->second->send(ev_wire(ev));
    }

    void sendToAll(const std::vector<app::PlayerId>& pids, const app::GameEvent& ev) override {
        for (auto pid : pids) sendTo(pid, ev);
    }

    void close(app::PlayerId pid) override {
        auto cit = connections_.find(sessions_.connIdOf(pid));
        if (cit != connections_.end()) cit->second->shutdown();
    }

private:
    void onNewConnection(int fd, const common::network::InetAddress& peer) {
        uint64_t id = next_conn_id_++;
        auto conn = std::make_shared<TcpConnection>(&loop_, id, fd, peer);
        conn->setMessageCallback(
            [this](const TcpConnectionPtr& c, Buffer& buf) { onMessage(c, buf); });
        conn->setCloseCallback(
            [this](const TcpConnectionPtr& c) { onConnectionClosed(c); });
        conn->establish();
        connections_[id] = conn;
        sessions_.onConnected(id);
        LOG_INFO("conn#" + std::to_string(id) + " in from " + peer.toIpPort() +
                 ", total=" + std::to_string(connections_.size()));
    }

    void onConnectionClosed(const TcpConnectionPtr& conn) {
        sessions_.onDisconnected(conn->id());
        connections_.erase(conn->id());
        LOG_INFO("conn#" + std::to_string(conn->id()) + " closed, total=" +
                 std::to_string(connections_.size()));
    }

    void onMessage(const TcpConnectionPtr& conn, Buffer& buf) {
        while (auto pkt = ProtocolCodec::tryExtract(buf)) {
            dispatch(conn, pkt->msg_id, pkt->payload.data(), pkt->payload.size());
        }
    }

    void dispatch(const TcpConnectionPtr& conn, uint16_t msg_id,
                  const uint8_t* p, size_t n) {
        auto cmd = ProtocolCodec::decode(msg_id, p, n);
        if (!cmd) return;

        auto pid = sessions_.playerIdOf(conn->id());
        if (pid != 0) sessions_.onActivity(pid);   // 任意消息喂心跳狗

        std::visit([this, conn, pid](const auto& c) { handle(conn, pid, c); }, *cmd);
    }

    // 各命令的分发（Login 之外都以已登录为前提）
    void handle(const TcpConnectionPtr& conn, app::PlayerId, const app::LoginCommand& c) {
        // pid 分配与连接映射的真源都在 SessionService（connIdOf 反查），
        // LoginAck 在 handleLogin 内部发出时映射已就绪
        sessions_.handleLogin(conn->id(), c);
    }
    void handle(const TcpConnectionPtr&, app::PlayerId pid, const app::JoinRoomCommand&) {
        if (pid) rooms_.handleJoin(pid, sessions_.name(pid));
    }
    void handle(const TcpConnectionPtr&, app::PlayerId pid, const app::MoveCommand& c) {
        if (pid) rooms_.handleMove(pid, c);
    }
    void handle(const TcpConnectionPtr&, app::PlayerId pid, const app::FireCommand& c) {
        if (pid) rooms_.handleFire(pid, c);
    }
    void handle(const TcpConnectionPtr&, app::PlayerId pid, const app::PingCommand& c) {
        if (pid) sessions_.handlePing(pid, c);
    }

    static const std::string& ev_wire(const app::GameEvent& ev) {
        static thread_local std::string wire;
        wire = ProtocolCodec::encode(ev);
        return wire;
    }

    common::network::EventLoop& loop_;
    app::SessionService& sessions_;
    app::RoomService& rooms_;
    Acceptor acceptor_;

    std::unordered_map<uint64_t, TcpConnectionPtr> connections_;
    uint64_t next_conn_id_ = 1;
};

} // namespace sightline::adapters
