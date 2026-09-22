// main.cpp —— 组装工（整洁架构最外层）：创建一切、接好依赖、上电、退场
// 业务代码对本文件一无所知。

#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <regex>
#include <map>
#include <set>
#include <thread>
#include "sightline_config.h"
#include "adapters/game_server.h"
#include "adapters/admin_api.h"
#include "adapters/admin_http_server.h"
#include "adapters/timer_wheel_adapter.h"
#include "adapters/mysql_account_repository.h"
#include "adapters/mysql_match_repository.h"
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

// 相对路径解析：CLion 等启动器默认工作目录在构建目录，而 config.lua/www/logs
// 按仓库根组织。查找顺序：① CWD（手动从仓库根启动）→ ② 可执行文件所在目录
// → ③ 其父目录（build-wsl/ 的上级即仓库根）。都找不到则原样返回（保持旧行为）。
std::filesystem::path resolveRepoPath(const std::string& rel)
{
    namespace fs = std::filesystem;
    if (fs::exists(rel)) return rel;
    std::error_code ec;
    const auto exe = fs::read_symlink("/proc/self/exe", ec);   // Linux：exe 真实位置
    if (!ec)
    {
        const auto dir = exe.parent_path();
        for (const auto& base : {dir, dir / ".."})
        {
            std::error_code ec2;
            const auto p = base / rel;
            if (fs::exists(p, ec2)) return p;
        }
    }
    return rel;
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
    const std::string configPath = resolveRepoPath("config.lua").string();   // 热载/写回须用同一路径
    LuaVM vm;
    const bool luaOk = vm.loadFile(configPath);
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
        const std::string log_dir = resolveRepoPath(cfg.log_dir).string();
        std::filesystem::create_directories(log_dir);
        logger.addSink(std::make_unique<common::logger::FileSink>(
            log_dir + "/sightline.log", 8 * 1024 * 1024, 5));
    }
    if (!luaOk)
        logger.warn("config.lua 未找到（尝试过: " + configPath +
                    "）——全部使用内置默认配置");

    // ---- 装配线 ----
    common::network::EventLoop loop(cfg.loop);          // 主 loop：acceptor + 定时器 + 游戏逻辑
    adapters::TimerWheelAdapter timers(loop);           // Port#2 实现

    // ---- 存储线程池 + 账号管线（15 号 01 文档 D1 / D2；须在 server.start 前装配）----
    sightline::StorageIO storageIO;
    storageIO.start(&loop);
    std::shared_ptr<sightline::storage::MysqlPool> mysqlPool;
    std::shared_ptr<sightline::app::IAccountRepository> accountRepo;
    std::shared_ptr<sightline::app::MySqlMatchRepository> matchRepo;
    std::shared_ptr<sightline::storage::RedisConnection> rankingRedis;
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
        matchRepo   = std::make_shared<sightline::app::MySqlMatchRepository>(mysqlPool);
        logger.info("[StorageIO] MySQL " + mc.host + ":" + std::to_string(mc.port) +
                    " db=" + mc.database + " user=" + mc.user, __FILE__, __LINE__);
    }
    if (luaOk)
    {
        sightline::storage::RedisConfig rc;
        rc.host     = vm.getString("database", "redis_host", rc.host);
        rc.port     = static_cast<int>(vm.getNumber("database", "redis_port", rc.port));
        rc.password = vm.getString("database", "redis_password", rc.password);
        rankingRedis = std::make_shared<sightline::storage::RedisConnection>(rc);
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
    rooms.attachMatchPersistence(matchRepo,             // 战绩落库管线
        [&storageIO](std::function<void()> job) { storageIO.post(std::move(job)); },
        [&sessions](app::PlayerId pid) { return sessions.accountIdOf(pid); });
    rooms.attachRanking(rankingRedis);                   // D4 排行榜

    // 适配层 + Port#1 实现：主从 Reactor（IO 多线程，逻辑单线程跳回主 loop）
    adapters::GameServer::Options net_opts;
    net_opts.io_threads = cfg.io_threads;
    net_opts.conn.high_water_mark = cfg.high_water_mark;
    net_opts.conn.rate_bytes_per_sec = cfg.send_rate_bytes_per_sec;
    adapters::GameServer server(loop, sessions, rooms, cfg.port, net_opts);
    proxy.bind(server);
    server.start();

    // ---- GM 管理 API（03 文档）：HTTP 线程收请求，queueInLoop 跳主 loop 取数/操作 ----
    auto applyConfigFromLua = [&] {                 // 同步热载（须在主 loop 线程调用）
        if (vm.hotReload(configPath))
        {
            cfg.applyLua(vm);
            rooms.updateConfig(cfg.room);     // 热应用：新开局按新规则（进行中对局不变）
            sessions.updateConfig(cfg.session);
            logger.info("config reloaded: win_kills=" +
                        std::to_string(cfg.room.room_rules.kills_to_win) +
                        " (新开局生效)", __FILE__, __LINE__);
        }
    };
    auto reloadConfigFn = [&] {                     // 异步包装：控制台/Admin API 从外部线程触发
        g_loop->queueInLoop(applyConfigFromLua);
    };

    // config.lua 文本改写：逐行只替换命中键的"值 token"，注释与结构原样保留。
    // 全部键命中才写盘（hits 不齐 = 拒绝半写），失败返回 false。
    auto rewriteLuaValues = [](const std::string& path,
                               const std::vector<std::pair<std::string, std::string>>& kv) -> bool {
        std::ifstream in(path);
        if (!in) return false;
        std::vector<std::string> lines;
        for (std::string ln; std::getline(in, ln); ) lines.push_back(ln);
        in.close();

        static const std::regex sec_re("^\\s*(\\w+)\\s*=\\s*\\{");
        std::string cur_sec;
        int hits = 0;
        for (auto& line : lines)
        {
            std::smatch m;
            if (std::regex_search(line, m, sec_re)) { cur_sec = m[1]; continue; }
            for (const auto& [dotted, val] : kv)
            {
                const size_t dot = dotted.find('.');
                if (dot == std::string::npos || dotted.substr(0, dot) != cur_sec) continue;
                const std::string key = dotted.substr(dot + 1);
                const std::regex key_re("^\\s*" + key + "\\s*=\\s*(-?[0-9.]+)(.*)$");
                std::smatch km;
                if (std::regex_search(line, km, key_re))
                {
                    line = line.substr(0, km.position(1)) + val + km[2].str();
                    ++hits;
                }
            }
        }
        if (hits != static_cast<int>(kv.size())) return false;
        std::ofstream out(path, std::ios::trunc);
        for (size_t i = 0; i < lines.size(); ++i) out << lines[i] << (i + 1 < lines.size() ? "\n" : "");
        return out.good();
    };

    // 可热调参数白名单（main 作为组合根知道哪些键能热改；database/admin 段需重启，不开放）
    auto get_config = [&] {
        return std::vector<std::pair<std::string, std::string>>{
            {"game.win_kills",                std::to_string(cfg.room.room_rules.kills_to_win)},
            {"game.respawn_ms",               std::to_string(cfg.room.respawn_delay_ms)},
            {"game.max_players",              std::to_string(cfg.room.room_rules.max_players)},
            {"network.heartbeat_timeout_ms",  std::to_string(cfg.session.heartbeat_timeout_ms)},
            {"network.scan_interval_ms",      std::to_string(cfg.session.scan_interval_ms)},
        };
    };
    auto set_config = [&](const std::vector<std::pair<std::string, std::string>>& kv) -> std::string {
        static const std::set<std::string> allowed = {
            "game.win_kills", "game.respawn_ms", "game.max_players",
            "network.heartbeat_timeout_ms", "network.scan_interval_ms"};
        // 安全范围：面板可写 ≠ 可乱写——heartbeat=3ms 这类值会把会话"合法地"全踢光
        static const std::map<std::string, std::pair<int, int>> limits = {
            {"game.win_kills",                {1,     100}},
            {"game.respawn_ms",               {100,   60000}},
            {"game.max_players",              {2,     64}},
            {"network.heartbeat_timeout_ms",  {3000,  120000}},
            {"network.scan_interval_ms",      {200,   10000}},
        };
        for (const auto& [k, v] : kv)
        {
            if (!allowed.count(k)) return "非法配置键: " + k;
            const int val = std::atoi(v.c_str());
            const auto& [lo, hi] = limits.at(k);
            if (val < lo || val > hi)
                return "数值超出安全范围: " + k + "（允许 " +
                       std::to_string(lo) + "~" + std::to_string(hi) + "）";
        }
        if (!rewriteLuaValues(configPath, kv)) return "config.lua 写入失败（键不存在或文件不可写）";
        applyConfigFromLua();                 // 此闭包在主 loop 里执行（routeLocked），同步热载
        return "";
    };

    adapters::AdminApi adminApi(sessions, rooms, server,
                                [&loop](std::function<void()> f) { loop.queueInLoop(std::move(f)); },
                                cfg.admin_token, reloadConfigFn, get_config, set_config,
                                resolveRepoPath("www/admin/index.html").string());
    adapters::AdminHttpServer adminHttp;
    if (cfg.admin_port != 0)
    {
        adminHttp.start(cfg.admin_port, [&adminApi](const adapters::HttpRequest& r) {
            return adminApi.handle(r);
        });
        logger.info("[Admin] GM API on http://0.0.0.0:" + std::to_string(cfg.admin_port) +
                    "  (面板 / ，API /api/*，Bearer 鉴权)", __FILE__, __LINE__);
    }

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
                reloadConfigFn();   // 与 GM Admin API 共用同一热载实现
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
    adminHttp.stop();      // 先停 HTTP 线程：它还会 queueInLoop，须早于 loop 析构
    storageIO.stop();
    logger.info("Sightline stopped", __FILE__, __LINE__);
    return 0;
}
