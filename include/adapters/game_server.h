// adapters/game_server.h —— 组装胶水：连接管理 + 消息分发 + IGameChannel 实现
//
// 本轮（网络库优化）的并发决策落地：
//   主从 Reactor：acceptor 跑主 loop，连接 round-robin 分发到 N 个 IO loop；
//   **消息从 IO 线程跳回主 loop 分发（queueInLoop）——游戏逻辑保持单线程**，
//   SessionService/RoomService/domain 因此零锁、零改动（架构文档 §6.1 的兑现）。
//
// 线程契约速查：
//   onNewConnection / dispatch / onConnectionClosed(服务部分)  → 主 loop（逻辑线程）
//   TcpConnection 的读写回调                                   → 各自 IO loop
//   connections_ 表：主线程写（插入/删除），发送路径读 → mutex 保护（低频，非热路径）
//   统计计数器：atomic，IO/逻辑线程皆可增

#pragma once
#include <atomic>
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "net/acceptor.h"
#include "net/tcp_connection.h"
#include "adapters/protocol_codec.h"
#include "application/dto.h"
#include "application/ports/i_game_channel.h"
#include "application/session_service.h"
#include "application/room_service.h"
#include "net/event_loop.h"
#include "net/event_loop_thread_pool.h"
#include "logger/logger.h"

namespace sightline::adapters {

class GameServer : public app::IGameChannel {
public:
    struct Options {
        int io_threads = 2;                        // 0 = 单 Reactor（保持旧行为）
        TcpConnection::Options conn;
    };

    GameServer(common::network::EventLoop& logic_loop,
               app::SessionService& sessions,
               app::RoomService& rooms,
               uint16_t listen_port)
        : GameServer(logic_loop, sessions, rooms, listen_port, Options()) {}

    GameServer(common::network::EventLoop& logic_loop,
               app::SessionService& sessions,
               app::RoomService& rooms,
               uint16_t listen_port,
               const Options& opts)
        : logic_loop_(logic_loop), sessions_(sessions), rooms_(rooms),
          acceptor_(&logic_loop, common::network::InetAddress(listen_port)),
          io_pool_(std::make_unique<common::network::EventLoopThreadPool>(
              &logic_loop, opts.io_threads)),
          opts_(opts) {
        acceptor_.setNewConnectionCallback(
            [this](int fd, const common::network::InetAddress& peer) {
                onNewConnection(fd, peer);
            });
    }

    void start() {
        io_pool_->start();
        acceptor_.listen();
        LOG_INFO("GameServer started, io_threads=" + std::to_string(io_pool_->size()));
    }

    // ---- IGameChannel 实现（逻辑线程调用）----

    void sendTo(app::PlayerId pid, const app::GameEvent& ev) override {
        auto cit = findConn(sessions_.connIdOf(pid));
        if (cit == nullptr) return;
        // 编码一次，所有收件人共享同一份 wire（sendToAll 免重复编码）
        const std::string& wire = ev_wire(ev);
        stats_.msgs_out.fetch_add(1, std::memory_order_relaxed);
        stats_.bytes_out.fetch_add(wire.size(), std::memory_order_relaxed);
        cit->send(wire);    // 跨线程安全：内部投递到该连接的 IO loop
    }

    void sendToAll(const std::vector<app::PlayerId>& pids, const app::GameEvent& ev) override {
        if (pids.empty()) return;
        const std::string& wire = ev_wire(ev);   // 广播编码一次
        for (auto pid : pids) {
            auto cit = findConn(sessions_.connIdOf(pid));
            if (cit == nullptr) continue;
            stats_.msgs_out.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes_out.fetch_add(wire.size(), std::memory_order_relaxed);
            cit->send(wire);
        }
    }

    void close(app::PlayerId pid) override {
        auto cit = findConn(sessions_.connIdOf(pid));
        if (cit != nullptr) cit->shutdown();   // 优雅关闭（策略见 TcpConnection）
    }

    // ---- 可观测性（主 loop 周期调用）----

    struct Stats {
        std::atomic<uint64_t> msgs_in{0}, msgs_out{0};
        std::atomic<uint64_t> bytes_in{0}, bytes_out{0};
    };

    void logStats() {
        size_t conns = 0;
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            conns = connections_.size();
        }
        LOG_INFO("[stats] conns=" + std::to_string(conns) +
                 " msgs(in/out)=" + std::to_string(stats_.msgs_in.load()) + "/" +
                 std::to_string(stats_.msgs_out.load()) +
                 " bytes(in/out)=" + std::to_string(stats_.bytes_in.load()) + "/" +
                 std::to_string(stats_.bytes_out.load()) +
                 " rooms=" + std::to_string(rooms_.roomCount()) +
                 " timerfd_settime=" + std::to_string(logic_loop_.timerfdSettimeCount() - last_timerfd_count_) + "/周期");
        last_timerfd_count_ = logic_loop_.timerfdSettimeCount();
    }

private:
    // ---- 连接生命周期 ----

    void onNewConnection(int fd, const common::network::InetAddress& peer) {
        // 主 loop（acceptor 线程）：选 IO loop、建连接、跨线程 establish
        common::network::EventLoop* io_loop = io_pool_->getNextLoop();
        uint64_t id = next_conn_id_++;
        auto conn = std::make_shared<TcpConnection>(io_loop, id, fd, peer, opts_.conn);

        conn->setMessageCallback(
            [this](const TcpConnectionPtr& c, Buffer& buf) { onMessage(c, buf); });
        conn->setCloseCallback(
            [this](const TcpConnectionPtr& c) { onConnectionClosed(c); });
        // 高水位策略：慢客户端 → 记日志并立即断开（策略在装配侧，机制在连接内）
        conn->setHighWaterMarkCallback(
            [](const TcpConnectionPtr& c, size_t pending) {
                LOG_WARNING("conn#" + std::to_string(c->id()) + " slow client: output " +
                            std::to_string(pending) + " bytes > high water mark, force close");
                c->forceClose();
            });

        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            connections_[id] = conn;
        }
        io_loop->runInLoop([conn] { conn->establish(); });   // 在其 IO 线程注册读事件
        sessions_.onConnected(id);
        LOG_DEBUG("conn#" + std::to_string(id) + " in from " + peer.toIpPort() +
                 ", total=" + std::to_string(currentConnCount()));
    }

    size_t currentConnCount() {
        std::lock_guard<std::mutex> lock(conns_mutex_);
        return connections_.size();
    }

    void onConnectionClosed(const TcpConnectionPtr& conn) {
        // IO 线程上下文：表操作锁保护；服务状态清理跳回逻辑线程
        {
            std::lock_guard<std::mutex> lock(conns_mutex_);
            connections_.erase(conn->id());
        }
        logic_loop_.queueInLoop([this, conn] {
            sessions_.onDisconnected(conn->id());   // → dropPlayer → 联动房间退房
            LOG_DEBUG("conn#" + std::to_string(conn->id()) + " closed, total=" +
                     std::to_string(currentConnCount()));
        });
    }

    // ---- 消息路径：IO 线程拆帧 → 跳回逻辑线程分发 ----

    void onMessage(const TcpConnectionPtr& conn, Buffer& buf) {
        // 在 IO 线程完成拆帧（Buffer 属于 IO 线程），把完整的包跳回逻辑线程
        std::vector<ProtocolCodec::RawPacket> packets;
        while (auto pkt = ProtocolCodec::tryExtract(buf)) {
            packets.push_back(std::move(*pkt));
        }
        if (packets.empty()) return;
        uint64_t in_bytes = 0;
        for (auto& pkt : packets) in_bytes += 4 + pkt.payload.size();
        stats_.msgs_in.fetch_add(packets.size(), std::memory_order_relaxed);
        stats_.bytes_in.fetch_add(in_bytes, std::memory_order_relaxed);
        logic_loop_.queueInLoop([this, conn, packets = std::move(packets)]() mutable {
            for (auto& pkt : packets) {
                dispatch(conn, pkt.msg_id, pkt.payload.data(), pkt.payload.size());
            }
        });
    }

    void dispatch(const TcpConnectionPtr& conn, uint16_t msg_id,
                  const uint8_t* p, size_t n) {
        auto cmd = ProtocolCodec::decode(msg_id, p, n);
        if (!cmd) return;

        auto pid = sessions_.playerIdOf(conn->id());
        if (pid != 0) sessions_.onActivity(pid);   // 任意消息喂心跳狗

        LOG_DEBUG("conn#" + std::to_string(conn->id()) +
                  " msgid=" + std::to_string(msg_id) +
                  " payload=" + std::to_string(n) + "B" +
                  (pid != 0 ? " pid=" + std::to_string(pid) : ""));

        std::visit([this, conn, pid](const auto& c) { handle(conn, pid, c); }, *cmd);
    }

    // 各命令的分发（Login 之外都以已登录为前提）
    void handle(const TcpConnectionPtr& conn, app::PlayerId, const app::LoginCommand& c) {
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

    TcpConnectionPtr findConn(uint64_t conn_id) {
        if (conn_id == 0) return nullptr;
        std::lock_guard<std::mutex> lock(conns_mutex_);
        auto it = connections_.find(conn_id);
        return it == connections_.end() ? nullptr : it->second;
    }

    static const std::string& ev_wire(const app::GameEvent& ev) {
        static thread_local std::string wire;
        wire = ProtocolCodec::encode(ev);
        return wire;
    }

    common::network::EventLoop& logic_loop_;
    app::SessionService& sessions_;
    app::RoomService& rooms_;
    Acceptor acceptor_;
    std::unique_ptr<common::network::EventLoopThreadPool> io_pool_;
    Options opts_;

    std::mutex conns_mutex_;                                   // 保护 connections_
    std::unordered_map<uint64_t, TcpConnectionPtr> connections_;
    uint64_t next_conn_id_ = 1;                                // 仅主 loop 访问
    uint64_t last_timerfd_count_ = 0;                           // stats 的 timerfd 速率差分
    Stats stats_;
};

} // namespace sightline::adapters
