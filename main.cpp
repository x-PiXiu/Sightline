// main.cpp —— 组装工（整洁架构最外层）：创建一切、接好依赖、上电、退场
// 业务代码对本文件一无所知。

#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>
#include "sightline_config.h"
#include "adapters/game_server.h"
#include "adapters/timer_wheel_adapter.h"
#include "adapters/mysql_account_repository.h"
#include "application/session_service.h"
#include "application/room_service.h"
#include "application/ports/i_account_repository.h"
#include "storage/storage_io.h"
#include "storage/mysql_pool.h"
#include "lua/lua_vm.h"
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

    // ---- 配置装配（优先级：命令行 > config.lua > 内置默认；15 号 01 文档 D1）----
    LuaVM vm;
    const bool luaOk = vm.loadFile("config.lua");
    SightlineConfig cfg;
    if (luaOk) cfg.applyLua(vm);                                                // Lua 覆盖内置默认
    if (argc > 1) cfg.port = static_cast<uint16_t>(std::stoi(argv[1]));         // 命令行最高优先
    if (argc > 2 && std::string(argv[2]) == "debug") cfg.debug_log = true;

    // 日志：异步 + 限频（热路径零同步 IO；风暴期按调用点限流防日志风暴）
    auto& logger = common::logger::Logger::getInstance();
    logger.addSink(std::make_unique<common::logger::ConsoleSink>());
    logger.setRateLimitPerSecond(100);
    logger.setAsyncLogging(true);
    logger.setLogLevel(cfg.debug_log ? common::logger::LogLevel::DEBUG
                                     : common::logger::LogLevel::INFO);
    if (cfg.file_log) {   // 生产：追加滚动文件（8MB×5 个），控制台留给开发期
        std::filesystem::create_directories(cfg.log_dir);
        logger.addSink(std::make_unique<common::logger::FileSink>(
            cfg.log_dir + "/sightline.log", 8 * 1024 * 1024, 5));
    }
    if (!luaOk)
        logger.warn("config.lua 未加载成功——全部使用内置默认配置");

    // ---- 装配线 ----
    common::network::EventLoop loop(cfg.loop);          // 主 loop：acceptor + 定时器 + 游戏逻辑
    adapters::TimerWheelAdapter timers(loop);           // Port#2 实现

    // ---- 存储线程池 + 账号管线（15 号 01 文档 D1 / D2；须在 server.start 前装配）----
    sightline::StorageIO storageIO;
    storageIO.start(&loop);
    std::shared_ptr<sightline::storage::MysqlPool> mysqlPool;
    std::shared_ptr<sightline::app::IAccountRepository> accountRepo;
    if (luaOk)
    {
        sightline::storage::MysqlConfig mc;
        mc.host     = vm.getString("database", "mysql_host", mc.host);
        mc.port     = static_cast<int>(vm.getNumber("database", "mysql_port", mc.port));
        mc.user     = vm.getString("database", "mysql_user", mc.user);
        mc.password = vm.getString("database", "mysql_password", mc.password);
        mc.database = vm.getString("database", "mysql_database", mc.database);
        mysqlPool   = std::make_shared<sightline::storage::MysqlPool>(mc, 4);
        accountRepo = std::make_shared<sightline::app::MySqlAccountRepository>(mysqlPool);
        logger.info("[StorageIO] MySQL " + mc.host + ":" + std::to_string(mc.port) +
                    " db=" + mc.database + " user=" + mc.user, __FILE__, __LINE__);
    }

    ChannelProxy proxy;                                 // Port#1 占位
    app::RoomService rooms(proxy, timers, cfg.room);    // 用例层
    app::SessionService sessions(
        proxy, timers,
        [&rooms](app::PlayerId pid) { rooms.handlePlayerGone(pid); },
        cfg.session);
    sessions.attachAccountPipeline(                     // 账号管线：登录/注册走存储线程
        [&storageIO](std::function<void()> job) { storageIO.post(std::move(job)); },
        [&loop](std::function<void()> cb) { loop.queueInLoop(std::move(cb)); },
        accountRepo);

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

    storageIO.post([&logger] {
        // 此段运行在存储线程——D2 起这里将执行 SQL/Redis/Mongo 任务
        logger.info("[StorageIO] 存储线程就绪（骨架）", __FILE__, __LINE__);
    });
    storageIO.postBackToMain([&logger] {
        logger.info("[StorageIO] 回投主 EventLoop 正常", __FILE__, __LINE__);
    });

    // ---- 控制台管理线程（reload = 热载 config.lua；quit = 退出）----
    std::thread consoleThread([&] {
        std::string line;
        while (std::getline(std::cin, line))
        {
            if (line == "reload")
            {
                g_loop->queueInLoop([&] {
                    if (vm.hotReload("config.lua"))
                    {
                        cfg.applyLua(vm);
                        rooms.updateConfig(cfg.room);     // 热应用：新开局按新规则（进行中对局不变）
                        sessions.updateConfig(cfg.session);
                        logger.info("config reloaded: win_kills=" +
                                    std::to_string(cfg.room.room_rules.kills_to_win) +
                                    " (新开局生效)", __FILE__, __LINE__);
                    }
                });
            }
            else if (line == "quit")
            {
                if (g_loop) g_loop->quit();
                break;
            }
        }
    });

    // ---- 上电 ----
    g_loop = &loop;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGHUP, onSignal);   // CLion Stop/终端关闭会发 SIGHUP——同走 quit 避免非零退出码
    // 压测第一课：不忽略 SIGPIPE，向已 RST 的连接 write 会直接杀死进程（无日志暴毙）
    std::signal(SIGPIPE, SIG_IGN);

    logger.info("Sightline FPS server listening on port " + std::to_string(cfg.port),
                __FILE__, __LINE__);
    loop.loop();

    // ---- 退场 ----
    // 控制台线程阻塞在 getline(stdin)——join 会永远等待。
    // detach 让它随进程退出自然消亡（daemon 线程不需要 join）。
    if (consoleThread.joinable()) consoleThread.detach();
    storageIO.stop();
    logger.info("Sightline stopped", __FILE__, __LINE__);
    return 0;
}
