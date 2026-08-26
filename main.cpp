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

    // 日志：显式挂控制台 sink（Logger 默认不添加任何输出目标）
    auto& logger = common::logger::Logger::getInstance();
    logger.addSink(std::make_unique<common::logger::ConsoleSink>());
    logger.setLogLevel(common::logger::LogLevel::INFO);

    const auto cfg = SightlineConfig::fromArgs(argc, argv);

    // ---- 装配线 ----
    common::network::EventLoop loop(cfg.loop);          // 框架层：Reactor + 时间轮
    adapters::TimerWheelAdapter timers(loop);           // Port#2 实现

    ChannelProxy proxy;                                 // Port#1 占位
    app::RoomService rooms(proxy, timers, cfg.room);    // 用例层
    app::SessionService sessions(
        proxy, timers,
        [&rooms](app::PlayerId pid) { rooms.handlePlayerGone(pid); },
        cfg.session);

    adapters::GameServer server(loop, sessions, rooms, cfg.port);   // 适配层 + Port#1 实现
    proxy.bind(server);
    server.start();

    // ---- 上电 ----
    g_loop = &loop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    logger.info("Sightline FPS server listening on port " + std::to_string(cfg.port),
                __FILE__, __LINE__);
    loop.loop();
    logger.info("Sightline stopped", __FILE__, __LINE__);
    return 0;
}
