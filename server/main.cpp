// Sightline 服务器 —— 第 3 期：登录 + 房间 + 朴素同步对局（装配根）
// 设计依据：docs/episodes/ep03/设计草案.md + docs/protocol/MSG_IDS.md v3 + 双端架构图（图 = 代码一致）
// 装配清单：日志 → SIGPIPE → EventLoop → Acceptor → 帧队列 → 33ms 帧驱动
//           → MessageDispatcher（注册表）→ LobbyService + MatchService（用例）+ FrameWriter（事件出站）
// 第 2 期 → 第 3 期的变化：业务层长出 MatchService（开始对局/名册/离场，D3-15 编排归适配层）；
//           帧驱动职责 +1：实体快照广播（M4 到位）；关服广播收进用例（D3-19）
#include <csignal>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <memory>

#include "logger/logger.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/acceptor.h"
#include "net/client_connection.h"
#include "adapters/frame_codec.h"
#include "adapters/lobby_wiring.h"
#include "adapters/match_wiring.h"
#include "lobby/lobby.h"
#include "match/match_service.h"
#include "protocol/msg_ids.h"

namespace {
    constexpr uint16_t kListenPort = 18888;   // 8888 被系统 iphlpsvc 占用，2026-10-15 换 18888（实现细节，不入契约）

    std::atomic<bool> gShutdownRequested{false};      // 信号只置位；广播在帧驱动里做（信号处理器内不做复杂动作）

    struct QueuedFrame {                              // 帧队列元素
        net::ClientConnection::Ptr conn;              // 回包通道
        uint16_t            msgid;
        std::string         payload;
    };

    uint64_t nowMs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    void appendU64(std::string& s, uint64_t v) {           // 小端
        for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
} // namespace

int main() {
    // ===== main 只干装配（最脏的一层，整洁架构最外圈）=====
    // 1. 日志装配
    common::logger::Logger::getInstance().addSink(
        std::make_unique<common::logger::ConsoleSink>());
    common::logger::Logger::getInstance().setLogLevel(common::logger::LogLevel::DEBUG);   // 业务 debug 可见；心跳已在 TRACE 不刷屏
    LOG_DEBUG("[自检] 日志级别=DEBUG，本行可见即 debug 通路正常");

    // 2. SIGPIPE：写已断开的 socket 默认杀进程——先显式忽略（为什么：第 13 期事故复盘）
    std::signal(SIGPIPE, SIG_IGN);

    // 2.5 优雅关闭（D2-14）：SIGINT/SIGTERM 只置位，帧驱动里广播 KICKED(3) 后 quit
    std::signal(SIGINT, [](int) { gShutdownRequested = true; });
    std::signal(SIGTERM, [](int) { gShutdownRequested = true; });

    // 3. 事件循环：单线程——IO 与逻辑同线程（世界状态一致，房间逻辑零锁）
    common::network::EventLoop loop;

    // 4. 业务用例 + 出站适配器 + 连接表（先于帧驱动构造，供回调捕获）
    sightline::lobby::LobbyService lobby;
    sightline::match::MatchService match;
    adapters::ConnTable connections;
    adapters::FrameWriter writer{connections, lobby};

    // 5. 帧队列：切好的帧入队，帧驱动消费
    std::deque<QueuedFrame> frame_queue;

    // 6. MessageDispatcher：msgid → 处理器注册表（新消息只注册，不改旧代码）
    //    注册顺序契约：wireMatch 先注册（msgid 10 的对局离场钩子须先于大厅离开做 fd→PlayerId 翻译）
    adapters::MessageDispatcher dispatcher;
    adapters::MatchWiring matchWiring{dispatcher, match, lobby, writer};
    matchWiring.Register();
    adapters::wireLobby(dispatcher, lobby, writer, connections);

    // 7. 帧驱动：33ms 一帧——消费帧队列（分发）+ 登录宽限清理
    loop.runEvery(33, [&] {
        while (!frame_queue.empty()) {
            QueuedFrame f = std::move(frame_queue.front());
            frame_queue.pop_front();
            dispatcher.dispatch(f.conn->fd(), f.msgid, f.payload);
        }
        writer.deliver(lobby.SweepLoginTimeouts(nowMs()));   // 宽限超时 → KICKED(2)+断开
        writer.deliverMatch(match.TickSnapshot());           // M4：33ms 实体快照广播（17，不含自机）

        // 优雅关闭（D2-14/D3-19）：关服语义在用例，main 只搬运——广播 KICKED(3) 后退出事件循环
        if (gShutdownRequested) {
            LOG_INFO("[关服] 广播 KICKED(3) → " + std::to_string(connections.size()) + " 个在线连接后退出");
            writer.deliver(lobby.ShutdownBroadcast());
            loop.quit();
        }
    });

    // 8. Acceptor：接客 → 新建连接 → WELCOME（第 1 期契约不变）→ 登记宽限期
    net::Acceptor acceptor(&loop, common::network::InetAddress(kListenPort));
    acceptor.setNewConnectionCallback([&](int connfd, const common::network::InetAddress& peer) {
        auto conn = std::make_shared<net::ClientConnection>(&loop, connfd, peer);
        conn->setMessageCallback([&](const net::ClientConnection::Ptr& c,
                                     uint16_t msgid, const std::string& payload) {
            frame_queue.push_back({c, msgid, payload});       // 收帧入队（帧驱动消费）
        });
        conn->setCloseCallback([&](const net::ClientConnection::Ptr& c) {
            connections.erase(c->fd());
            matchWiring.HandleDisconnect(c->fd());            // 对局离场（D3-15：须先于 lobby 移除做 fd→pid 翻译）
            writer.deliver(lobby.Disconnect(c->fd()));        // 在房中：同 Leave 路径（D2-16）
            LOG_INFO("连接断开: " + c->peerAddr().toIpPort() + " 在线=" +
                     std::to_string(connections.size()));
        });
        connections[connfd] = conn;

        std::string payload;                                  // 8B serverTs
        appendU64(payload, nowMs());
        conn->send(protocol::MSG_S2C_WELCOME, payload);
        lobby.OnConnected(connfd, nowMs());                   // 登录宽限期起点（10s）

        LOG_INFO("新连接: " + peer.toIpPort() + " fd=" + std::to_string(connfd) +
                 " 在线=" + std::to_string(connections.size()));
    });

    LOG_INFO("Sightline 服务器启动，监听 :" + std::to_string(kListenPort) +
             "（单线程 · 帧队列 · 心跳 · 登录+房间）");
    loop.loop();                                              // 主线程进入事件循环
    LOG_INFO("Sightline 服务器退出");
    return 0;
}
