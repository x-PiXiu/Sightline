// main.cpp —— 组装工（整洁架构最外层）：创建一切、接好依赖、上电、退场
// 业务代码对本文件一无所知。

#include <csignal>
#include <memory>
#include "sightline_config.h"
#include "adapters/game_server.h"
#include "adapters/timer_wheel_adapter.h"
#include "application/session_service.h"
#include "application/room_service.h"
#include "logger/logger.h"

namespace {

common::network::EventLoop* g_loop = nullptr;   // 信号处理用

void onSignal(int) {
    if (g_loop) g_loop->quit();   // quit 内部只写原子标志 + eventfd，信号上下文安全
}

// 服务(GameServer)与服务(Session/Room)互相需要的解法：
// 组装期先用代理占位，GameServer 构造完成后绑定真身。代理属于 main，不污染分层。
class ChannelProxy final : public sightline::app::IGameChannel {
public:
    void bind(sightline::app::IGameChannel& real) { real_ = &real; }
    void sendTo(sightline::app::PlayerId pid, const sightline::app::GameEvent& ev) override {
        if (real_) real_->sendTo(pid, ev);
    }
    void sendToAll(const std::vector<sightline::app::PlayerId>& pids,
                   const sightline::app::GameEvent& ev) override {
        if (real_) real_->sendToAll(pids, ev);
    }
    void close(sightline::app::PlayerId pid) override {
        if (real_) real_->close(pid);
    }

private:
    sightline::app::IGameChannel* real_ = nullptr;
};

} // namespace

int main(int argc, char* argv[]) {
    using namespace sightline;

    // 日志：显式挂控制台 sink + 启用异步（热路径不再承担控制台写出的阻塞）
    auto& logger = common::logger::Logger::getInstance();
    logger.addSink(std::make_unique<common::logger::ConsoleSink>());
    logger.setAsyncLogging(true);
    logger.setLogLevel(common::logger::LogLevel::INFO);

    const auto cfg = SightlineConfig::fromArgs(argc, argv);

    // ---- 装配线 ----
    common::network::EventLoop loop(cfg.loop);          // 主 loop：acceptor + 定时器 + 游戏逻辑
    adapters::TimerWheelAdapter timers(loop);           // Port#2 实现

    ChannelProxy proxy;                                 // Port#1 占位
    app::RoomService rooms(proxy, timers, cfg.room);    // 用例层
    app::SessionService sessions(
        proxy, timers,
        [&rooms](app::PlayerId pid) { rooms.handlePlayerGone(pid); },
        cfg.session);

    // 适配层 + Port#1 实现：主从 Reactor（IO 多线程，逻辑单线程跳回主 loop）
    adapters::GameServer::Options net_opts;
    net_opts.io_threads = cfg.io_threads;
    net_opts.conn.high_water_mark = cfg.high_water_mark;
    net_opts.conn.rate_bytes_per_sec = cfg.send_rate_bytes_per_sec;
    adapters::GameServer server(loop, sessions, rooms, cfg.port, net_opts);
    proxy.bind(server);
    server.start();

    // 可观测性：周期上报（走时间轮的周期定时器）
    loop.runEvery(cfg.stats_interval_s * 1000, [&server] { server.logStats(); });

    // ---- 上电 ----
    g_loop = &loop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // 压测第一课：不忽略 SIGPIPE，向已 RST 的连接 write 会直接杀死进程（无日志暴毙）
    std::signal(SIGPIPE, SIG_IGN);

    logger.info("Sightline FPS server listening on port " + std::to_string(cfg.port),
                __FILE__, __LINE__);
    loop.loop();
    logger.info("Sightline stopped", __FILE__, __LINE__);
    return 0;
}
