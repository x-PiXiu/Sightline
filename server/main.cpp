// Sightline 服务器 —— 第 1 期：UE 连接 + 心跳 echo（装配根）
// 设计依据：docs/episodes/ep01/ 四张定稿图（图 = 代码一致）
// 装配清单：日志 addSink → SIGPIPE → EventLoop（单线程）→ Acceptor → 帧队列 → 33ms 帧驱动
#include <csignal>
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
#include "protocol/msg_ids.h"

namespace {
    constexpr uint16_t kListenPort = 8888;

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
    // ===== main 只干装配（最脏的一层）=====
    // 1. 日志装配：不 addSink 则全部静默（交接批注第 7 条，冒烟实测）
    common::logger::Logger::getInstance().addSink(
        std::make_unique<common::logger::ConsoleSink>());

    // 2. SIGPIPE：写已断开的 socket 默认杀进程——先显式忽略（为什么：第 13 期事故复盘）
    std::signal(SIGPIPE, SIG_IGN);

    // 3. 事件循环：第 1 期单线程——IO 与逻辑同线程（架构图 v2.0）
    common::network::EventLoop loop;

    // 4. 帧队列：切好的帧入队，帧驱动消费（第 3 期同步复用）
    std::deque<QueuedFrame> frame_queue;              // main 不返回（loop 死循环），栈生命周期安全
    std::map<int, net::ClientConnection::Ptr> connections;

    // 5. 帧驱动：33ms 一帧，消费帧队列（单线程串行——世界状态一致）
    loop.runEvery(33, [&] {
        while (!frame_queue.empty()) {
            QueuedFrame f = std::move(frame_queue.front());
            frame_queue.pop_front();
            if (f.msgid == protocol::MSG_C2S_HEARTBEAT && f.payload.size() == 12) {
                const uint32_t seq =
                    static_cast<uint32_t>(static_cast<unsigned char>(f.payload[0]))
                  | (static_cast<uint32_t>(static_cast<unsigned char>(f.payload[1])) << 8)
                  | (static_cast<uint32_t>(static_cast<unsigned char>(f.payload[2])) << 16)
                  | (static_cast<uint32_t>(static_cast<unsigned char>(f.payload[3])) << 24);
                LOG_INFO("[HEARTBEAT] seq=" + std::to_string(seq) +
                         " from=" + f.conn->peerAddr().toIpPort());
                std::string ack = f.payload;                  // 原样回显 seq + clientTs
                appendU64(ack, nowMs());                      // 附加 serverTs
                f.conn->send(protocol::MSG_S2C_HEARTBEAT_ACK, ack);
            } else {
                LOG_WARNING("[未知消息] msgid=" + std::to_string(f.msgid) +
                            " 长度=" + std::to_string(f.payload.size()) +
                            " 来自=" + f.conn->peerAddr().toIpPort() + " —— 已忽略");
            }
        }
    });

    // 6. Acceptor：接客 → 新建连接 → WELCOME
    net::Acceptor acceptor(&loop, common::network::InetAddress(kListenPort));
    acceptor.setNewConnectionCallback([&](int connfd, const common::network::InetAddress& peer) {
        auto conn = std::make_shared<net::ClientConnection>(&loop, connfd, peer);
        conn->setMessageCallback([&](const net::ClientConnection::Ptr& c,
                                     uint16_t msgid, const std::string& payload) {
            frame_queue.push_back({c, msgid, payload});       // 收帧入队（帧驱动消费）
        });
        conn->setCloseCallback([&](const net::ClientConnection::Ptr& c) {
            connections.erase(c->fd());
            LOG_INFO("连接断开: " + c->peerAddr().toIpPort() + " 在线=" +
                     std::to_string(connections.size()));
        });
        connections[connfd] = conn;

        // WELCOME：连接建立即发（时序图 ②b）
        std::string payload;                            // 8B serverTs（恰好，不再多初始化）
        appendU64(payload, nowMs());
        conn->send(protocol::MSG_S2C_WELCOME, payload);

        LOG_INFO("新连接: " + peer.toIpPort() + " fd=" + std::to_string(connfd) +
                 " 在线=" + std::to_string(connections.size()));
    });

    LOG_INFO("Sightline 服务器启动，监听 :" + std::to_string(kListenPort) +
             "（单线程 · 帧队列 · 心跳 echo）");
    loop.loop();                                              // 主线程进入事件循环
    LOG_INFO("Sightline 服务器退出");
    return 0;
}
