// Sightline 服务器 —— 第 4 期 M2：主从 Reactor（装配根）
// 设计依据：docs/episodes/ep04/设计草案.md（§2.2）+ net_reactor 定稿图（图 = 代码一致）
// 拓扑：main = accept + 定时器 + 33ms 帧驱动 + 业务（单线程串行铁律，零锁）；
//       IO Worker ×N = 独立 EventLoop（收字节/切帧/EPOLLOUT 排空，连接归属线程固定）；
//       frame_queue（MPSC 控制帧族 Frame/Closed）= 唯一跨线程门——与客户端 AsyncTask 对称；
//       kIoThreads=0（环境变量 SIGHTLINE_IO_THREADS）→ 回落单线程拓扑（同一套代码两种形态，smoke 双过）
// 业务层零改动抵押：lobby/match 一行不碰（git diff 为证——验收四条之二）
#include <csignal>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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

    // ===== frame_queue：唯一跨线程门（worker → main 的 MPSC 控制帧族）=====
    enum class QType { Frame, Closed };               // Connected 走 main 本地日志，不入队（最小集）
    struct QueuedEvent {
        QType                   type;
        net::ClientConnection::Ptr conn;              // shared_ptr 跨线程安全（原子引用计数）
        uint16_t                msgid = 0;
        std::string             payload;
    };
    std::mutex          gqMx;                         // MPSC 保护（N worker 生产，main tick 消费）
    std::deque<QueuedEvent> gq;

    // ===== IO Worker：独立 EventLoop + 线程（主从 Reactor 的"从"）=====
    struct Worker {
        common::network::EventLoop loop;
        std::thread               th;
        std::atomic<int>          connCount{0};       // 连接计数（选择用；main 唯一读写者=零竞态）

        void start() { th = std::thread([this] { loop.loop(); }); }   // loop() 内绑定线程亲和
        void stop()  { loop.quit(); if (th.joinable()) th.join(); }
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
    common::logger::Logger::getInstance().setAsyncLogging(true);   // 异步日志：终端冻结（选中文本/拖窗口）不再卡事件循环——双端 LAG 级联根因（M6 实锤）
    LOG_DEBUG("[自检] 日志级别=DEBUG，本行可见即 debug 通路正常");

    // 2. SIGPIPE：写已断开的 socket 默认杀进程——先显式忽略（为什么：第 13 期事故复盘）
    std::signal(SIGPIPE, SIG_IGN);

    // 2.5 优雅关闭（D2-14）：SIGINT/SIGTERM 只置位，帧驱动里广播 KICKED(3) 后 quit
    std::signal(SIGINT, [](int) { gShutdownRequested = true; });
    std::signal(SIGTERM, [](int) { gShutdownRequested = true; });

    // 3. IO Worker 拓扑（kIoThreads：环境变量，默认 2；0 = 回落单线程——同一套代码两种形态）
    //    双拓扑 smoke 双过 = 验收四条之一；性能定档留给第 12 期压测（验证叙事不施工）
    int kIoThreads = 2;
    if (const char* env = std::getenv("SIGHTLINE_IO_THREADS")) {
        kIoThreads = std::atoi(env);
        if (kIoThreads < 0) kIoThreads = 0;
        if (kIoThreads > 8) kIoThreads = 8;
    }
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < kIoThreads; ++i) {
        workers.push_back(std::make_unique<Worker>());
        workers.back()->start();
    }

    // 4. main 事件循环（accept/定时器/业务永远在这条线程——单线程串行铁律）
    common::network::EventLoop loop;

    // 5. 业务用例 + 出站适配器 + 连接表（先于帧驱动构造，供回调捕获）
    sightline::lobby::LobbyService lobby;
    sightline::match::MatchService match;
    adapters::ConnTable connections;
    adapters::FrameWriter writer{connections, lobby};
    std::map<int, int> fdWorker;                         // fd → worker 下标（选择计数的 main 侧账本）

    // 6. MessageDispatcher：msgid → 处理器注册表（新消息只注册，不改旧代码）
    //    注册顺序契约：wireMatch 先注册（msgid 10 的对局离场钩子须先于大厅离开做 fd→PlayerId 翻译）
    adapters::MessageDispatcher dispatcher;
    adapters::MatchWiring matchWiring{dispatcher, match, lobby, writer};
    matchWiring.Register();
    adapters::wireLobby(dispatcher, lobby, writer, connections);

    // 7. 帧驱动：33ms 一帧——消费唯一跨线程门（Frame/Closed 控制帧族）+ 登录宽限清理 + 快照广播
    loop.runEvery(33, [&] {
        std::deque<QueuedEvent> local;                   // swap 出来再处理（锁内零业务）
        { std::lock_guard<std::mutex> lk(gqMx); local.swap(gq); }
        for (QueuedEvent& ev : local) {
            // 陈旧事件防护（档二 UAF 根因）：fd 号会被内核复用——事件在队列里排队期间，同号新连接
            // 可能已接管（业务账在 accept 回调里结清）。指针对不上=属于已死的旧连接：帧丢弃、
            // 报丧跳过——现主人的连接表/计数/业务状态一根手指都不许碰。
            const int fd = ev.conn->fd();
            auto itc = connections.find(fd);
            if (itc == connections.end() || itc->second != ev.conn) {
                if (ev.type == QType::Closed)
                    LOG_INFO("陈旧报丧已丢弃: " + ev.conn->peerAddr().toIpPort());
                continue;
            }
            switch (ev.type) {
            case QType::Frame:
                dispatcher.dispatch(fd, ev.msgid, ev.payload);
                break;
            case QType::Closed: {                        // 业务清理只在 main（worker 只报丧，不动业务）
                connections.erase(itc);
                if (auto it = fdWorker.find(fd); it != fdWorker.end()) {
                    workers[it->second]->connCount--;
                    fdWorker.erase(it);
                }
                matchWiring.HandleDisconnect(fd);            // 对局离场（D3-15：先于 lobby 移除做 fd→pid 翻译）
                writer.deliver(lobby.Disconnect(fd));        // 在房中：同 Leave 路径（D2-16）
                LOG_INFO("连接断开: " + ev.conn->peerAddr().toIpPort() + " 在线=" +
                         std::to_string(connections.size()));
                break;
            }
            }
        }
        writer.deliver(lobby.SweepLoginTimeouts(nowMs()));   // 宽限超时 → KICKED(2)+断开
        writer.deliverMatch(match.TickSnapshot());           // 33ms 实体快照广播（17v2，不含自机）

        // 优雅关闭（D2-14/D3-19）：关服语义在用例，main 只搬运——广播 KICKED(3) 后退出事件循环
        if (gShutdownRequested) {
            LOG_INFO("[关服] 广播 KICKED(3) → " + std::to_string(connections.size()) + " 个在线连接后退出");
            writer.deliver(lobby.ShutdownBroadcast());
            loop.quit();
        }
    });

    // 8. Acceptor：接客 → 选 worker（最少连接计数 argmin——main 单线程所有权，零锁零原子）
    //    对象 main 构造（连接表/发送视角），Channel 注册投归属线程 Start()（epoll 操作永不跨线程）
    net::Acceptor acceptor(&loop, common::network::InetAddress(kListenPort));
    acceptor.setNewConnectionCallback([&](int connfd, const common::network::InetAddress& peer) {
        // fd 复用旧账结清（档二 UAF 根因的前半）：同号旧连接的收尾已在归属线程完成（fd 已关才会
        // 被内核复用），但它的 Closed 报丧可能还在队列里——先在此结清它的业务账，防止残留会话
        // 被宽限超时误踢新主人；队列里那条报丧随后会被身份校验判为陈旧丢弃。
        if (connections.count(connfd)) {
            matchWiring.HandleDisconnect(connfd);
            writer.deliver(lobby.Disconnect(connfd));
            if (auto it = fdWorker.find(connfd); it != fdWorker.end()) {
                workers[it->second]->connCount--;
                fdWorker.erase(it);
            }
            connections.erase(connfd);
            LOG_INFO("fd=" + std::to_string(connfd) + " 被新连接复用，旧连接业务账已结清（报丧在途）");
        }
        int wIdx = -1;
        if (!workers.empty()) {                           // 主从拓扑：argmin 选归属
            int best = 0;
            for (int i = 1; i < static_cast<int>(workers.size()); ++i)
                if (workers[i]->connCount.load() < workers[best]->connCount.load()) best = i;
            wIdx = best;
            workers[best]->connCount++;                    // main 唯一计数者（Closed 在 tick 里减）
        }
        common::network::EventLoop* owner = (wIdx < 0) ? &loop : &workers[wIdx]->loop;
        auto conn = std::make_shared<net::ClientConnection>(owner, connfd, peer);

        conn->setMessageCallback([&](const net::ClientConnection::Ptr& c,
                                     uint16_t msgid, const std::string& payload) {
            std::lock_guard<std::mutex> lk(gqMx);           // worker 线程投递（唯一跨线程门）
            gq.push_back({QType::Frame, c, msgid, payload});
        });
        conn->setCloseCallback([&](const net::ClientConnection::Ptr& c) {
            std::lock_guard<std::mutex> lk(gqMx);           // 报丧也走门——业务清理统一回 main tick
            gq.push_back({QType::Closed, c, 0, {}});
        });
        connections[connfd] = conn;
        if (wIdx >= 0) fdWorker[connfd] = wIdx;

        owner->queueInLoop([conn] { conn->Start(); });      // Channel 注册在归属线程（线程断言纪律）

        std::string payload;                                  // 8B serverTs（WELCOME 任意线程可发）
        appendU64(payload, nowMs());
        conn->send(protocol::MSG_S2C_WELCOME, payload);
        lobby.OnConnected(connfd, nowMs());                   // 登录宽限期起点（10s）

        LOG_INFO("新连接: " + peer.toIpPort() + " fd=" + std::to_string(connfd) +
                 " 归属=" + (wIdx < 0 ? "main(单线程)" : ("worker-" + std::to_string(wIdx))) +
                 " 在线=" + std::to_string(connections.size()));
    });

    LOG_INFO("Sightline 服务器启动，监听 :" + std::to_string(kListenPort) + "（" +
             (workers.empty() ? "单线程拓扑"
                              : ("主从 Reactor · IO workers=" + std::to_string(workers.size()))) +
             " · 帧队列 · 心跳 · 登录+房间+对局）");
    loop.loop();                                              // 主线程进入事件循环

    // 9. 收尾顺序（Channel 摘除必须归属线程）：先请 worker 关完连接，再停 worker，最后对象析构（无 Channel 裸对象任意线程安全）
    for (auto& kv : connections) kv.second->ownerLoop()->queueInLoop([c = kv.second] { c->handleClose(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 等 worker 跑完 close（粗但够用；精确化留给压测期）
    for (auto& w : workers) w->stop();

    LOG_INFO("Sightline 服务器退出");
    return 0;
}
