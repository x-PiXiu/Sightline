// Sightline 服务器 —— 第 4 期 M2：主从 Reactor（装配根）
// 设计依据：docs/episodes/ep04/设计草案.md（§2.2）+ net_reactor 定稿图（图 = 代码一致）
// 拓扑：main = accept + 定时器 + 33ms 帧驱动 + 业务（单线程串行铁律，零锁）；
//       IO Worker ×N = 独立 EventLoop（收字节/切帧/EPOLLOUT 排空，连接归属线程固定）；
//       frame_queue（MPSC 控制帧族 Frame/Closed）= 唯一跨线程门——与客户端 AsyncTask 对称；
//       kIoThreads=0（环境变量 SIGHTLINE_IO_THREADS）→ 回落单线程拓扑（同一套代码两种形态，smoke 双过）
// 业务层零改动抵押：lobby/match 一行不碰（git diff 为证——验收四条之二）
#include <csignal>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>                                  // readlink（exe 相对路径解析）

#include "logger/logger.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/acceptor.h"
#include "net/client_connection.h"
#include "adapters/frame_codec.h"
#include "adapters/lobby_wiring.h"
#include "adapters/map_geometry.h"
#include "adapters/match_wiring.h"
#include "adapters/config_service.h"
#include "adapters/scripting_lua.h"   // 步骤 4：声明层零 lua 依赖（使用点受 HAS_LUA 门控）
#include "adapters/persistence.h"      // 步骤 5：full 模式持久化（含三 sink/WAL）

#ifndef SIGHTLINE_HAS_LUA
#define SIGHTLINE_HAS_LUA 0
#endif          // ep08 步骤 3：配置适配器（4 个端口同一对象）
#include "adapters/stdin_command_source.h"
#include "adapters/tcp_command_source.h"   // ep08：运维命令 TCP 口（与日志流分离）
#include "adapters/null_persistence.h"
#include "adapters/wire_sink.h"
#include "adapters/bot_driver_port.h"
#include "application/orchestrator.h"
#include "match/policies.h"     // v1.6：端口已迁 match/
#include "lobby/lobby.h"
#include "match/match_service.h"
#include "protocol/msg_ids.h"

namespace {
    // 8888 被系统 iphlpsvc 占用，2026-10-15 换 18888（实现细节，不入契约）；
    // SIGHTLINE_PORT 可覆盖——同机多实例/联调并存免改码（与 SIGHTLINE_IO_THREADS 同族）
    std::uint16_t listenPort() {
        if (const char* env = std::getenv("SIGHTLINE_PORT")) {
            const int p = std::atoi(env);
            if (p > 0 && p < 65536) return static_cast<std::uint16_t>(p);
        }
        return 18888;
    }

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

        // loop() 内绑定线程亲和；就绪/退出各报一行——子 reactor 活性的第一手日志证据
        void start(const std::string& name) {
            th = std::thread([this, name] {
                LOG_INFO("[Worker] " + name + " EventLoop 就绪（线程已绑定，开始收帧）");
                loop.loop();
                LOG_INFO("[Worker] " + name + " EventLoop 退出");
            });
        }
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

    // 几何合同路径候选链（2026-10-29 用户实勘：CLion 启动 CWD=构建目录，CWD 相对路径落空）——
    // env 显式 > CWD 相对 > exe 同级（仓库根布局：server/sightline_server）> exe 上级（构建子目录布局：
    // cmake-build-ubuntu/sightline_server）。首个存在者胜；全不存在返回 CWD 相对——保持"未找到→空遮挡表
    // +LOG_INFO 继续"既有语义（无图环境兼容 smoke/loadbot）
    std::string ResolveGeometryPath() {
        if (const char* env = std::getenv("SIGHTLINE_MAP_GEOMETRY")) return env;
        std::vector<std::string> candidates{"assets/maps/NewMap.json"};
        std::array<char, 4096> buf{};
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n > 0) {
            buf[static_cast<std::size_t>(n)] = '\0';
            const std::string exe(buf.data());
            const std::size_t slash = exe.find_last_of('/');
            const std::string exeDir = (slash == std::string::npos) ? std::string(".") : exe.substr(0, slash);
            candidates.push_back(exeDir + "/assets/maps/NewMap.json");
            candidates.push_back(exeDir + "/../assets/maps/NewMap.json");
        }
        for (const std::string& c : candidates) {
            if (std::ifstream f(c); f.good()) return c;
        }
        return candidates.front();
    }

    // ep08 步骤 3：配置表路径（§3.6 与几何合同同款候选链：env > CWD > exe 同级 > exe 上级）
    std::string ResolveConfigPath() {
        if (const char* env = std::getenv("SIGHTLINE_CONFIG")) return env;
        std::vector<std::string> candidates{"assets/scripts/game_config.lua"};
        std::array<char, 4096> buf{};
        const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
        if (n > 0) {
            buf[static_cast<std::size_t>(n)] = '\0';
            const std::string exe(buf.data());
            const std::size_t slash = exe.find_last_of('/');
            const std::string exeDir = (slash == std::string::npos) ? std::string(".") : exe.substr(0, slash);
            candidates.push_back(exeDir + "/assets/scripts/game_config.lua");
            candidates.push_back(exeDir + "/../assets/scripts/game_config.lua");
        }
        for (const std::string& c : candidates) {
            if (std::ifstream f(c); f.good()) return c;
        }
        return candidates.front();   // 不存在 = 内建默认配置（v0-builtin），lite 裸跑
    }

    // 步骤 4：脚本目录 = 配置文件所在目录（同一白名单解析链的 parent——路径只解析一次）
    std::string ResolveScriptDir() {
        const std::string cfg = ResolveConfigPath();
        const auto pos = cfg.find_last_of("/\\");
        return pos == std::string::npos ? std::string("assets/scripts") : cfg.substr(0, pos);
    }
} // namespace

// §5.7.1 双模式开关：--db-profile=X > --with-db 简写 > SIGHTLINE_DB_PROFILE > 默认 lite。
// 装配决策只在组合根（红线：用例/sink 内部不得读 profile）。
std::string ResolveDbProfile(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--db-profile=", 0) == 0) return a.substr(13);   // sizeof("--db-profile=")-1，避免依赖 <cstring>
        if (a == "--with-db") return "full";
    }
    if (const char* e = std::getenv("SIGHTLINE_DB_PROFILE")) return e;
    return "lite";
}

std::string EnvOr(const char* key, const char* defv) {
    if (const char* v = std::getenv(key)) return v;
    return defv;
}

std::string ResolveWalPath() {
    return EnvOr("SIGHTLINE_WAL", "assets/db/audit.wal");
}

int main(int argc, char** argv) {
    // ===== main 只干装配（最脏的一层，整洁架构最外圈）=====
    // 1. 日志装配
    common::logger::Logger::getInstance().addSink(
        std::make_unique<common::logger::ConsoleSink>());
    // 日志级别环境可调（SIGHTLINE_LOG_LEVEL=INFO/DEBUG/TRACE，默认 DEBUG）——
    // 30Hz 高频帧（位置上报v2/心跳）收帧行已降 TRACE，默认 DEBUG 即消音；
    // 开火链路（[开火·请求]→[射击判定]/[射击拒绝]）走 INFO：任意档位整链可见
    common::logger::LogLevel lvl = common::logger::LogLevel::DEBUG;
    if (const char* env = std::getenv("SIGHTLINE_LOG_LEVEL")) {
        const std::string lv(env);
        if (lv == "TRACE") lvl = common::logger::LogLevel::TRACE;
        else if (lv == "INFO" || lv == "info") lvl = common::logger::LogLevel::INFO;
        else if (lv == "WARN" || lv == "warn") lvl = common::logger::LogLevel::WARN;
    }
    common::logger::Logger::getInstance().setLogLevel(lvl);
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
        workers.back()->start("worker-" + std::to_string(i));
    }

    // 4. main 事件循环（accept/定时器/业务永远在这条线程——单线程串行铁律）
    common::network::EventLoop loop;

    // 5. 业务用例 + 出站适配器 + 连接表（先于帧驱动构造，供回调捕获）
    sightline::lobby::LobbyService lobby;
    sightline::match::MatchService match;
    adapters::ConnTable connections;
    adapters::FrameWriter writer{connections, lobby};
    std::map<int, int> fdWorker;                         // fd → worker 下标（选择计数的 main 侧账本）

    // ===== ep08 步骤 3：配置 / 端口 / 编排器（组合根只做 new + 注入，不含业务节律）=====
    adapters::ConfigService config(ResolveConfigPath());     // IConfigSource + 四张只读视图（五端口）
    sightline::match::BuiltinDamagePolicy damagePolicy;      // v1.6：策略端口已迁 match/（消费方拥有）
    sightline::match::BuiltinBotPolicy botPolicy;
    // ===== ep08 步骤 5：持久化双模式（§5.7——装配只在此处，用例层零感知）=====
    const std::string dbProfile = ResolveDbProfile(argc, argv);
    std::unique_ptr<sightline::application::IPersistencePorts> persistence;
    adapters::RealPersistence* realP = nullptr;
    std::unique_ptr<adapters::AuditWal> liteWal;             // lite：审计→WAL（§5.7.2）
    std::string dbBanner = "[DB] profile=lite（持久化未启用）";   // 真实态（L475 配置横幅引用——曾硬编码 lite 误导）
#if SIGHTLINE_HAS_DB
    if (dbProfile == "full") {
        adapters::RealPersistenceConfig rpc;
        rpc.mysqlDsn = EnvOr("SIGHTLINE_MYSQL_DSN", "127.0.0.1:3306:sightline:sightline:sightline");   // 面板 MySQL（sightline 专用账号·db_up.sh 建号）
        rpc.redisUri = EnvOr("SIGHTLINE_REDIS_URI", "127.0.0.1:6379");
        rpc.mongoUri = EnvOr("SIGHTLINE_MONGO_URI", "mongodb://127.0.0.1:27017");   // 宝塔 MongoDB 8.0.32（无密码·§5.5 默认）
        rpc.walPath  = ResolveWalPath();
        auto rp = std::make_unique<adapters::RealPersistence>(std::move(rpc));
        rp->Start();                                          // worker 内连三库（同线程生与用）
        rp->SetConfigVersion(config.Version());
        const auto replayed = rp->ReplayWal();                // 上次 spill/lite 审计 → MySQL 补写
        realP = rp.get();
        LOG_INFO("[DB] profile=full · " + realP->HealthLine() +
                 (replayed ? " · WAL回放=" + std::to_string(replayed) : ""));
        dbBanner = "[DB] profile=full（三库持久化·健康见上横幅）";
        persistence = std::move(rp);
    } else
#endif
    {
        if (dbProfile == "full")
            LOG_WARNING("[DB] 本构建未启用 DB（SIGHTLINE_HAS_DB=0）——回退 lite");
        liteWal = std::make_unique<adapters::AuditWal>(ResolveWalPath());
        persistence = std::make_unique<adapters::NullPersistence>(liteWal.get());
        LOG_INFO("[DB] profile=lite · 持久化未启用（对局照常）· 审计→" + liteWal->Path());
    }
    adapters::WireSinkAdapter wireSink(writer);              // FrameWriter → IWireSink
    adapters::StdinCommandSource commands;                  // reload / rollback / version / help
    // ep08 步骤 9：运维命令 TCP 口（与日志流分离——前台刷屏不再淹没 stdin）；
    //   env SIGHTLINE_CMD_PORT 可改/置 0 关闭。stdin 与 TCP 双源同队列（A1：非主线程只入队）。
    adapters::TcpCommandListener cmdPort(commands,
        static_cast<unsigned>(EnvOr("SIGHTLINE_CMD_PORT", "18889").empty()
                                  ? 18889 : std::atoi(EnvOr("SIGHTLINE_CMD_PORT", "18889").c_str())));
    adapters::BotDriver botDriver;                          // ep07 人机驱动（主线程·零锁）
    adapters::BotDriverPort botDriverPort{botDriver, match, lobby};   // → IBotDriver 端口
    // v1.6 收口②/G2：策略与配置视图直注对局用例/驱动器（真接线）。
    // 步骤 4（§10.2.6）：HAS_LUA 构建换 Lua 策略——Builtin 保留为 pcall 失败/脚本缺失的
    // 内建兜底（E2）与对拍基准；bot_ai.lua 步 8 已建（等价性对拍绿后默认切 Lua）。
    botDriver.SetBotTuning(&config);
#if SIGHTLINE_HAS_LUA
    auto luaRt = adapters::LuaRuntime::Create(
        adapters::LuaRuntimeConfig{ResolveScriptDir(), 32u * 1024 * 1024});
    std::unique_ptr<adapters::LuaDamagePolicy> luaDmg;
    std::unique_ptr<adapters::LuaBotPolicy> luaBot;
    adapters::LuaRuntime* patchChan = nullptr;
    if (luaRt) {                       // E2：状态都起不来 → 整体落回内建（服务照起）
        luaDmg = std::make_unique<adapters::LuaDamagePolicy>(luaRt, &config);
        luaBot = std::make_unique<adapters::LuaBotPolicy>(luaRt);
        match.SetDamagePolicy(luaDmg.get());
        botDriver.SetBotPolicy(luaBot.get());
        patchChan = luaRt.get();       // patch 命令通道（§10.2.6 唯一新增注入位）
        LOG_INFO("[Lua] 策略层已接（damage=lua·bot=lua·patch=可用）");
    } else {
        match.SetDamagePolicy(&damagePolicy);
        botDriver.SetBotPolicy(&botPolicy);
        LOG_WARNING("[Lua] 运行时创建失败——全量内建兜底");
    }
#else
    match.SetDamagePolicy(&damagePolicy);
    botDriver.SetBotPolicy(&botPolicy);
#endif
    sightline::application::ApplicationOrchestrator app{
        sightline::application::OrchestratorDeps{
            .now = nowMs,
            .config = config,
            .combat = config,
            .tuning = config,
            .rules = config,
            .commands = commands,
            .wire = wireSink,
            .archive = *persistence,
            .audit = *persistence,
            .board = *persistence,
            .version = config,
#if SIGHTLINE_HAS_LUA
            .patches = patchChan,      // 步骤 4：patch <name> 命令经此端口（编排器不知 Lua 存在）
#endif
            .match = match,
            .botDriver = botDriverPort,
            .lobby = lobby,
        }};
    config.Subscribe([&app, realP](const sightline::application::ConfigChanged& e) {
        app.OnConfigChanged(e);                             // G4：审计 + G5：30 号广播（步骤 6）
#if SIGHTLINE_HAS_DB
        if (realP) realP->SetConfigVersion(e.version);      // 步骤 5：后续对局落库盖新章（符号在受守卫 TU）
#endif
    });

    // 5.5 几何合同装载（D5-08v2：编辑器烘焙产物，构建期数据——拒绝运行时上报）。
    //     语义分层：文件存在但非法 → fail-fast（装配错误不留活口）；文件不存在 → 空遮挡表 +
    //     LOG_INFO 继续（无图环境兼容 smoke/loadbot——hitscan 无遮挡，烘焙后重启生效）
    {
        const std::string path = ResolveGeometryPath();
        if (auto geometry = adapters::LoadMapGeometry(path)) {
            match.SetGeometry(*geometry);
            LOG_INFO("[几何合同] 装载 path=" + path +
                     " 盒=" + std::to_string(geometry->occluders.boxes.size()) +
                     " 球=" + std::to_string(geometry->occluders.spheres.size()) +
                     " 命中体 r=" + std::to_string(geometry->hitbox.radius) +
                     " h=" + std::to_string(geometry->hitbox.halfHeight));
            // 出生点净空自检（2026-11-03 联调实锤：4 箱全压出生点 → 人机原地不动+子弹先中箱皮）
            for (const std::size_t ix : sightline::match::BlockedSpawnPoints(*geometry)) {
                const auto& sp = geometry->spawnPoints[ix];
                LOG_WARNING("[几何合同] 出生点 " + std::to_string(ix + 1) + " @(" +
                            std::to_string(sp.x) + "," + std::to_string(sp.y) +
                            ") 被掩体压住（含命中体 34cm 膨胀带）——人机会原地不动、子弹先中箱皮打不中它；"
                            "请在关卡把该掩体挪开（箱缘离出生点 ≥70cm）后重新烘焙");
            }
        } else if (std::ifstream f(path); f.good()) {
            LOG_ERROR("[几何合同] " + path + " 存在但非法（schema 校验失败）——装配错误，拒绝启动");
            return 1;                                    // fail-fast：装配错误不留活口（D5-08v2）
        } else {
            LOG_INFO("[几何合同] 未找到 " + path + " → 空遮挡表（hitscan 无遮挡；烘焙后重启生效）");
        }
    }

    // 6. MessageDispatcher：msgid → 处理器注册表（新消息只注册，不改旧代码）
    //    注册顺序契约：wireMatch 先注册（msgid 10 的对局离场钩子须先于大厅离开做 fd→PlayerId 翻译）
    adapters::MessageDispatcher dispatcher;
    adapters::MatchWiring matchWiring{dispatcher, match, lobby, writer, &botDriver, nowMs};   // ADR-1 单钟
    // ③ 号事件源：20 号 Fire 事件经回调进编排器，与 ①/② 在同一订阅点扇出（B1 修复）
    matchWiring.SetMatchEventSink([&app](std::vector<sightline::match::MatchEvent>&& es) {
        app.OnWiringEvents(std::move(es));                  // v1.6：Fire/StartMatch/RemovePlayer 三源统一点
    });
    matchWiring.Register();
    adapters::wireLobby(dispatcher, lobby, writer, connections);

    // 7. 帧驱动：33ms 一帧——消费唯一跨线程门（Frame/Closed 控制帧族）+ 登录宽限清理 + 快照广播
    //    定时源沿革：多层时间轮投递病理（1ms 自旋×256ms 整圈交错，快照拍距被拉到 ~300ms=拉扯/瞬移
    //    根因，探针实测档案见 devlog）→ 2026-10-21 先以内核周期 timerfd 直驱止血 → 同日用户令定案
    //    「直接改造为纯堆」：TimerHeap 到期真相唯一（堆顶精确装弹，无槽位量化/无整圈轮候/无堆顶滞留），
    //    帧驱动回归 runEvery 抽象——不再绕过定时器体系。
    std::string lastWorkerSt;                             // 子 reactor 状态行：空串=未打过——首拍自证，变化才再落行
    auto tickBody = [&] {
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
        writer.deliver(lobby.SweepLoginTimeouts(nowMs()));   // 宽限超时 → KICKED(2)+断开（框架层）
        app.OnFrame();                                       // ep08：业务帧节律归用例层编排器
        static std::string lastNotice;                       // 命令 / 配置通知由适配器落日志，用例层零 IO
        if (!app.Notice().empty() && app.Notice() != lastNotice) {
            LOG_INFO("[配置] " + app.Notice());
            lastNotice = app.Notice();
        }

        // 子 reactor 状态行：首拍一行（开机自证"帧循环在跑、计数管线通"）+ 计数变化才再落行——
        // 空闲零噪音，连接进出/均分变化各有存照（逐连接明细另有 新连接/连接关闭 行，此行只管账面）
        std::string st = "[Worker 状态]";
        for (int i = 0; i < static_cast<int>(workers.size()); ++i)
            st += " worker-" + std::to_string(i) + " 在线=" + std::to_string(workers[i]->connCount.load());
        st += " 总在线=" + std::to_string(connections.size());
        if (st != lastWorkerSt) { LOG_INFO(st); lastWorkerSt = st; }

        // 优雅关闭（D2-14/D3-19）：关服语义在用例，main 只搬运——广播 KICKED(3) 后退出事件循环
        if (gShutdownRequested) {
            LOG_INFO("[关服] 广播 KICKED(3) → " + std::to_string(connections.size()) + " 个在线连接后退出");
            writer.deliver(lobby.ShutdownBroadcast());
            app.Shutdown();          // ep08：停止接帧
#if SIGHTLINE_HAS_DB
            if (realP) realP->Shutdown();   // 步骤 5（B6）：停 ingest → 2s 排水 → join worker
#endif
            loop.quit();
        }
    };
    loop.runEvery(33, tickBody);                           // 定时源=纯堆定时器（TimerHeap），节奏实测 33.0ms 整

    // 8. Acceptor：接客 → 选 worker（最少连接计数 argmin——main 单线程所有权，零锁零原子）
    //    对象 main 构造（连接表/发送视角），Channel 注册投归属线程 Start()（epoll 操作永不跨线程）
    net::Acceptor acceptor(&loop, common::network::InetAddress(listenPort()));
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

    LOG_INFO("Sightline 服务器启动，监听 :" + std::to_string(listenPort()) + "（" +
             (workers.empty() ? "单线程拓扑"
                              : ("主从 Reactor · IO workers=" + std::to_string(workers.size()))) +
             " · 帧队列 · 心跳 · 登录+房间+对局）");
    LOG_INFO("[配置] " + config.StatusLine() + " · " + dbBanner);
    loop.loop();                                              // 主线程进入事件循环

    // 9. 收尾顺序（Channel 摘除必须归属线程）：先请 worker 关完连接，再停 worker，最后对象析构（无 Channel 裸对象任意线程安全）
    for (auto& kv : connections) kv.second->ownerLoop()->queueInLoop([c = kv.second] { c->handleClose(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));   // 等 worker 跑完 close（粗但够用；精确化留给压测期）
    for (auto& w : workers) w->stop();

    LOG_INFO("Sightline 服务器退出");
    return 0;
}
