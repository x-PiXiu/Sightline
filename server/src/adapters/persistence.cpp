// 构建守卫：无三库客户端环境（SIGHTLINE_HAS_DB=0）下本文件编译为空 TU——
// 头文件声明零驱动依赖（前置声明），使用点（main 的 full 分支）同受宏门控。
#if defined(SIGHTLINE_HAS_DB) && SIGHTLINE_HAS_DB
// persistence.cpp —— ep08 步骤 5：full 模式持久化实现
// 结构：main 只 Ingest/Append/OnKill/SetConfigVersion（入队）→ worker 单线程独占三库连接
//       → 投影（MatchStart 开档 / Kill 时间线+榜 / Ended 战绩+战报）→ 失败按 durability 分流
//       （审计/战绩 Must：MySQL 失败→WAL 不丢；战报 best-effort：丢+计数；榜：可 REBUILD）。
#include "adapters/persistence.h"

#include "logger/logger.h"

#include <chrono>
#include <ctime>
#include <sstream>

namespace adapters {
namespace app = sightline::application;

namespace {
constexpr std::size_t kWipeGraceMs = 2000;   // B6 停机排水期限

std::string UtcStamp(std::uint64_t utcMs) {
    const std::time_t t = static_cast<std::time_t>(utcMs / 1000);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}
} // namespace

// ===================== 生命周期 =====================

RealPersistence::RealPersistence(RealPersistenceConfig cfg)
    : cfg_(std::move(cfg)), wal_(cfg_.walPath) {}

RealPersistence::~RealPersistence() { Shutdown(); }

void RealPersistence::Start() {
    // worker 内先做初始连接（连接的创建与使用同线程——避免跨线程交接连接对象）
    worker_ = std::thread([this] { WorkerLoop(); });
    // 等初始连接落定（上限 1s）——否则横幅与健康位竞态，可能打出全 ✗/半绿的假状态
    for (int i = 0; i < 100 && !started_.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

void RealPersistence::Shutdown() {
    if (stopping_.exchange(true)) return;   // 幂等
    cv_.notify_all();
    if (worker_.joinable()) worker_.join(); // B6：排水（2s 期限）在 worker 侧判定
}

std::string RealPersistence::HealthLine() const {
    const char* y = "✓", *n = "✗";
    std::string mongo = cfg_.mongoUri.empty() ? "⏸未配URI" :
                        (healthMongo_.load() ? y : n);
    return std::string("MySQL ") + (healthMysql_.load() ? y : n) +
           " · Mongo " + mongo +
           " · Redis " + (healthRedis_.load() ? y : n) +
           " · 丢弃=" + std::to_string(dropped_.load()) +
           " 溢写=" + std::to_string(spilled_.load()) +
           " WAL=" + std::to_string(wal_.LineCount()) + "行";
}

void RealPersistence::SetConfigVersion(std::string v) {
    std::lock_guard<std::mutex> lk(verMx_);
    configVersion_ = std::move(v);
}

// ===================== 入口（main 线程·只入队）=====================

// ===== ep09 步骤 5（G9/ADR-16）：账号域入队侧（主线程零阻塞——入队微秒级）=====
void RealPersistence::Register(int fd, const std::string& account, const std::string& pass) {
    std::lock_guard<std::mutex> lk(accMx_);
    if (accQ_.size() >= 256) {
        app::AccountResult r; r.kind = app::AccountResult::Kind::Auth; r.fd = fd;
        r.ok = false; r.reason = app::AuthReason::Unavailable; r.name = account;
        { std::lock_guard<std::mutex> rk(resultMx_); resultQ_.push_back(std::move(r)); }
        return;
    }
    AccReq q; q.kind = AccReq::Kind::Register; q.fd = fd; q.account = account; q.pass = pass;
    accQ_.push_back(std::move(q));
}
void RealPersistence::Login(int fd, const std::string& account, const std::string& pass) {
    std::lock_guard<std::mutex> lk(accMx_);
    if (accQ_.size() >= 256) {
        app::AccountResult r; r.kind = app::AccountResult::Kind::Auth; r.fd = fd;
        r.ok = false; r.reason = app::AuthReason::Unavailable; r.name = account;
        { std::lock_guard<std::mutex> rk(resultMx_); resultQ_.push_back(std::move(r)); }
        return;
    }
    AccReq q; q.kind = AccReq::Kind::Login; q.fd = fd; q.account = account; q.pass = pass;
    accQ_.push_back(std::move(q));
}
void RealPersistence::QueryStats(int fd, std::uint32_t accountId, std::uint32_t limit) {
    std::lock_guard<std::mutex> lk(accMx_);
    AccReq q; q.kind = AccReq::Kind::Query; q.fd = fd; q.accountId = accountId; q.limit = limit;
    accQ_.push_back(std::move(q));                     // 查询必收（低频 1B 请求）
}
std::vector<app::AccountResult> RealPersistence::DrainAuthResults() {
    std::deque<app::AccountResult> local;
    { std::lock_guard<std::mutex> rk(resultMx_); local.swap(resultQ_); }
    return std::vector<app::AccountResult>(std::make_move_iterator(local.begin()),
                                           std::make_move_iterator(local.end()));
}

void RealPersistence::Ingest(std::span<const app::MatchEvent> events, app::Durability d) {
    if (events.empty() || stopping_.load()) return;
    // 投影过滤（§5.2）：只留开档/击杀/终局——快照/重生/离场是 wire 数据，不入持久化流
    std::vector<app::MatchEvent> keep;
    keep.reserve(events.size());
    for (const auto& e : events)
        if (e.type == app::MatchEvent::Type::MatchStart ||
            e.type == app::MatchEvent::Type::Kill ||
            e.type == app::MatchEvent::Type::Ended)
            keep.push_back(e);
    if (keep.empty()) return;

    {
        std::lock_guard<std::mutex> lk(statsMx_);
        // 容量保护（§5.3）：满=丢+计数（对局优先于落库；触达条件=worker 卡死 1024 事件）
        if (statsDepth_ + keep.size() > cfg_.statsCapacity) {
            dropped_.fetch_add(keep.size());
            return;
        }
        statsQ_.push_back(StatsItem{ItemKind::Events, d, std::move(keep), 0});
        statsDepth_ += statsQ_.back().evs.size();
    }
    cv_.notify_one();
}

void RealPersistence::Append(const app::AuditRecord& r) {
    if (stopping_.load()) return;
    {
        std::lock_guard<std::mutex> lk(auditMx_);
        auditQ_.push_back(r);              // 无损（不设界）——失败由 worker 转 WAL
    }
    cv_.notify_one();
}

void RealPersistence::OnKill(std::uint32_t killerPid) {
    if (stopping_.load()) return;
    {
        std::lock_guard<std::mutex> lk(statsMx_);
        StatsItem it;
        it.kind = ItemKind::KillMarker;
        it.cls = app::Durability::Must;
        it.pid = killerPid;
        if (statsDepth_ + 1 > cfg_.statsCapacity) { dropped_.fetch_add(1); return; }
        statsQ_.push_back(std::move(it));
        statsDepth_ += 1;
    }
    cv_.notify_one();
}

void RealPersistence::Rebuild() {
    rebuildReq_.store(true);
    cv_.notify_one();
}

std::string RealPersistence::MakeMatchId(std::uint64_t utcMs, std::uint64_t seq) {
    std::ostringstream os;
    os << UtcStamp(utcMs) << '-' << (seq < 1000 ? "0" : "") << (seq < 100 ? "0" : "")
       << seq;
    return os.str();
}

// ===================== worker（唯一碰库线程）=====================

void RealPersistence::WorkerLoop() {
    MysqlStore::ThreadAttach();               // 本线程全程持有 mysql 线程态
    // 初始连接（连接的生与用同线程）
    healthMysql_.store(mysql_.Connect(cfg_.mysqlDsn));
    healthRedis_.store(redis_.Connect(cfg_.redisUri));
    if (!cfg_.mongoUri.empty())
        healthMongo_.store(mongo_.Connect(cfg_.mongoUri));
    else
        healthMongo_.store(false);
    started_.store(true);                       // 初始三连完成（横幅可读真实位）

    auto lastRetry = std::chrono::steady_clock::now();
    bool sawStopping = false;
    auto stopAt = std::chrono::steady_clock::time_point{};

    for (;;) {
        {
            std::unique_lock<std::mutex> lk(statsMx_);
            cv_.wait_for(lk, std::chrono::milliseconds(500), [this] {
                return stopping_.load() || !statsQ_.empty() ||
                       !auditQ_.empty() || !accQ_.empty() || rebuildReq_.load();
            });
        }
        if (stopping_.load()) {
            if (!sawStopping) { sawStopping = true; stopAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(kWipeGraceMs); }
            // 排水判定：队列空 或 超 2s 期限
            bool empty;
            { std::lock_guard<std::mutex> lk(statsMx_); empty = statsQ_.empty(); }
            { std::lock_guard<std::mutex> lk(auditMx_); empty = empty && auditQ_.empty(); }
            { std::lock_guard<std::mutex> lk(accMx_); empty = empty && accQ_.empty(); }
            if (empty) break;
            if (std::chrono::steady_clock::now() > stopAt) {
                LOG_WARNING("[DB] 停机排水超时（2s）——剩余项计数丢弃 dropped=" +
                            std::to_string(dropped_.load()));
                break;
            }
        }

        // ---- 0) 账号队列（E6 交互优先：用户在等 33/35 回执——先于审计/统计全清）----
        //  PBKDF2/IO 全在本 worker（G9）；产出 AccountResult 回流→主线程 DeliverAuth（E9 fd 寻址）
        for (;;) {
            AccReq q;
            { std::lock_guard<std::mutex> lk(accMx_);
              if (accQ_.empty()) break;
              q = std::move(accQ_.front()); accQ_.pop_front(); }
            app::AccountResult r;
            r.fd = q.fd;
            if (q.kind == AccReq::Kind::Query) {
                r.kind = app::AccountResult::Kind::Stats;
                std::vector<MysqlStore::StatRow> rows;
                r.ok = mysql_.QueryStatsByAccount(q.accountId, q.limit, rows);
                if (!r.ok) r.reason = app::AuthReason::Unavailable;
                else { r.reason = app::AuthReason::Ok; r.accountId = q.accountId;
                       for (auto& row : rows)
                           r.rows.push_back({row.matchId,
                                             static_cast<std::uint8_t>(row.kills),
                                             static_cast<std::uint8_t>(row.deaths),
                                             row.endedAt}); }
            } else {
                r.kind = app::AccountResult::Kind::Auth;
                r.name = q.account;
                if (!mysql_.Connected()) {                 // G9：库故障→Unavailable（账号域可降级）
                    r.ok = false; r.reason = app::AuthReason::Unavailable;
                } else if (q.kind == AccReq::Kind::Register) {
                    const std::string hash = hasher_.Hash(q.pass);
                    std::uint32_t aid = 0;
                    const auto rr = hash.empty()
                        ? MysqlStore::RegResult::Error
                        : mysql_.RegisterAccount(q.account, hash, aid);
                    if (rr == MysqlStore::RegResult::Ok) {
                        r.ok = true; r.reason = app::AuthReason::Ok;
                        r.accountId = aid; r.mode = 1;
                    } else {
                        r.ok = false;
                        r.reason = rr == MysqlStore::RegResult::Duplicate
                                       ? app::AuthReason::AlreadyExists
                                       : app::AuthReason::Unavailable;
                    }
                } else {                                   // Login：SELECT→Verify→Redis 会话
                    std::string hash; std::uint32_t aid = 0;
                    if (!mysql_.LookupAccount(q.account, hash, aid)) {
                        r.ok = false; r.reason = app::AuthReason::NotFound;
                    } else if (!hasher_.Verify(q.pass, hash)) {
                        r.ok = false; r.reason = app::AuthReason::WrongPass;
                    } else {
                        r.ok = true; r.reason = app::AuthReason::Ok;
                        r.accountId = aid; r.mode = 1;
                        redis_.SetSession(aid, 86400);     // G9：BestEffort——失败不阻塞登录
                    }
                }
            }
            { std::lock_guard<std::mutex> rk(resultMx_); resultQ_.push_back(std::move(r)); }
        }

        // ---- 1) 审计队列（无损：失败→WAL）----
        for (;;) {
            app::AuditRecord r;
            { std::lock_guard<std::mutex> lk(auditMx_);
              if (auditQ_.empty()) break;
              r = std::move(auditQ_.front()); auditQ_.pop_front(); }
            if (mysql_.AppendAudit(r)) {
                healthMysql_.store(true);
            } else {
                healthMysql_.store(false);
                if (wal_.AppendAudit(r)) spilled_.fetch_add(1);   // 审计不丢：转 WAL
                else LOG_ERROR("[DB] 审计双失败（MySQL+WAL）——本条丢失 version=" + r.version);
            }
        }

        // ---- 2) 统计/战报队列（投影路由）----
        std::vector<StatsItem> batch;
        {
            std::lock_guard<std::mutex> lk(statsMx_);
            while (!statsQ_.empty()) { batch.push_back(std::move(statsQ_.front())); statsQ_.pop_front(); }
            statsDepth_ = 0;
        }
        for (auto& it : batch) {
            if (it.kind == ItemKind::KillMarker) {
                // 榜投影：pid→name（当前名册）→ Redis ZINCRBY（可重建·失败计数）
                std::string name;
                { for (const auto& r : ctx_.roster) if (r.pid == it.pid) { name = r.name; break; } }
                if (name.empty()) { /* 未知 pid：跳过（不计失败——名册外无从投影） */ }
                else if (redis_.OnKill(name)) healthRedis_.store(true);
                else { healthRedis_.store(false); dropped_.fetch_add(1); }
                continue;
            }
            HandleEvents(it.evs);
        }

        // ---- 3) REBUILD（G7：mysql 读 → redis 覆盖写——编排在本适配器内）----
        if (rebuildReq_.exchange(false)) {
            std::vector<std::pair<std::string, int>> kills;
            if (mysql_.Connected() && mysql_.QueryKillsByPlayer(kills)) {
                healthMysql_.store(true);
                if (redis_.ClearLeaderboard()) {
                    for (const auto& [name, k] : kills)
                        if (!redis_.SetScore(name, k)) { healthRedis_.store(false); break; }
                    healthRedis_.store(healthRedis_.load() && true);
                    LOG_INFO("[DB] 榜已从 t_match_result 重建（" + std::to_string(kills.size()) + " 人）");
                } else healthRedis_.store(false);
            } else {
                healthMysql_.store(false);
                LOG_WARNING("[DB] REBUILD 失败（MySQL 不可用）——保持现榜");
            }
        }

        // ---- 4) 掉线重连（5s 节流——health 由每次写操作自证）----
        const auto now = std::chrono::steady_clock::now();
        if (now - lastRetry > std::chrono::seconds(5)) {
            lastRetry = now;
            if (!mysql_.Connected()) healthMysql_.store(mysql_.Connect(cfg_.mysqlDsn));
            if (!redis_.Connected()) healthRedis_.store(redis_.Connect(cfg_.redisUri));
            if (!cfg_.mongoUri.empty() && !mongo_.Connected())
                healthMongo_.store(mongo_.Connect(cfg_.mongoUri));
        }
    }

    // 线程退出：连接归创建线程关闭
    mysql_.Disconnect();
    redis_.Disconnect();
    mongo_.Disconnect();
    MysqlStore::ThreadExit();
}

// ===================== 投影（worker 线程内·单局缓冲）=====================

void RealPersistence::HandleEvents(const std::vector<app::MatchEvent>& evs) {
    for (const auto& e : evs) {
        switch (e.type) {
        case app::MatchEvent::Type::MatchStart: {
            ctx_ = MatchCtx{};                                  // 开档（单局边界·ep04）
            ctx_.active = true;
            ctx_.startedAtUtcMs = UtcNowMs_();
            { std::lock_guard<std::mutex> lk(verMx_); ctx_.configVersion = configVersion_; }
            static std::uint64_t seq = 0;                        // 会话内序号（C3）
            ctx_.matchId = MakeMatchId(ctx_.startedAtUtcMs, ++seq);
            for (const auto& r : e.roster) ctx_.roster.push_back({r.playerId, r.name, r.spawnIndex});
            LOG_INFO("[DB] 战报开档 match=" + ctx_.matchId +
                     " 名册=" + std::to_string(ctx_.roster.size()));
            break;
        }
        case app::MatchEvent::Type::Kill: {
            if (!ctx_.active) break;
            auto nameOf = [this](std::uint32_t pid) -> std::string {
                for (const auto& r : ctx_.roster) if (r.pid == pid) return r.name;
                return "pid" + std::to_string(pid);
            };
            const std::uint64_t now = UtcNowMs_();
            WarReport::KillE k;
            k.tOffsetMs = now > ctx_.startedAtUtcMs ? now - ctx_.startedAtUtcMs : 0;
            k.killer = nameOf(e.killKiller);
            k.victim = nameOf(e.killVictim);
            ctx_.timeline.push_back(std::move(k));
            ctx_.lastKillOffsetMs = k.tOffsetMs;
            break;
        }
        case app::MatchEvent::Type::Ended: {
            if (!ctx_.active) break;
            const std::uint64_t now = UtcNowMs_();
            WalMatchResult rows;
            rows.matchId = ctx_.matchId;
            rows.configVersion = ctx_.configVersion;
            rows.endedAtUtcMs = now;
            for (const auto& sc : e.scores) {
                std::string name = "pid" + std::to_string(sc.playerId);
                for (const auto& r : ctx_.roster) if (r.pid == sc.playerId) { name = r.name; break; }
                rows.rows.push_back({name, static_cast<int>(sc.kills), static_cast<int>(sc.deaths)});
            }
            // mysql 入参是 MatchResultRow（与 WAL 的 Row 结构同形不同型——显式转换）
            std::vector<MatchResultRow> mysqlRows;
            mysqlRows.reserve(rows.rows.size());
            for (const auto& r : rows.rows) mysqlRows.push_back({r.name, r.kills, r.deaths});
            // 战绩行（must）：MySQL 失败 → WAL（不丢·full 启动 ReplayWal 补写）
            if (mysql_.InsertMatchResults(rows.matchId, mysqlRows, rows.endedAtUtcMs,
                                          rows.configVersion)) {
                healthMysql_.store(true);
            } else {
                healthMysql_.store(false);
                if (wal_.AppendMatchResults(rows)) spilled_.fetch_add(1);
                else LOG_ERROR("[DB] 战绩双失败（MySQL+WAL）match=" + rows.matchId);
            }
            // 战报文档（best-effort）：失败=丢+计数（MySQL 行是真相·§5.4 doctrine）
            if (mongo_.Connected()) {
                WarReport rep;
                rep.matchId = ctx_.matchId;
                rep.configVersion = ctx_.configVersion;
                rep.startedAtUtcMs = ctx_.startedAtUtcMs;
                rep.endedAtUtcMs = now;
                rep.durationMs = now - ctx_.startedAtUtcMs;
                rep.endReason = e.endReason;
                for (const auto& r : ctx_.roster)
                    rep.roster.push_back({r.name, r.spawn});
                for (const auto& r : rows.rows) rep.scores.push_back({r.name, r.kills, r.deaths});
                rep.killTimeline = ctx_.timeline;
                if (mongo_.InsertWarReport(rep)) healthMongo_.store(true);
                else { healthMongo_.store(false); dropped_.fetch_add(1); }
            } else if (!cfg_.mongoUri.empty()) {
                dropped_.fetch_add(1);                          // 配了 URI 但连不上：计数
            }
            LOG_INFO("[DB] 对局落库 match=" + ctx_.matchId +
                     " 比分行=" + std::to_string(rows.rows.size()) +
                     " 时间线=" + std::to_string(ctx_.timeline.size()) +
                     " mysql=" + (healthMysql_.load() ? "ok" : "FAIL") +
                     " spilled=" + std::to_string(spilled_.load()));
            ctx_ = MatchCtx{};                                  // flush 完清档
            break;
        }
        default:
            break;                                              // 入口已过滤——理论不可达
        }
    }
}

app::QueueState RealPersistence::State() const {
    app::QueueState st;
    { std::lock_guard<std::mutex> lk(statsMx_); st.depth = statsDepth_; }
    st.capacity = cfg_.statsCapacity;
    st.health = {healthMysql_.load(), healthMongo_.load(), healthRedis_.load()};
    // st.cls 不在此定——编排器按批次分类后覆写（收口⑤：执行者报状态，决策归策略）
    return st;
}

std::size_t RealPersistence::ReplayWal() {
    // full 启动回放：A 行→审计、E 行→战绩；任一失败=保留 WAL（下次再试），全成→截断
    std::size_t n = 0;
    bool allOk = true;
    for (const auto& line : wal_.ReadAuditLines()) {
        app::AuditRecord r;
        if (!AuditWal::DecodeAudit(line, r)) continue;
        if (mysql_.AppendAudit(r)) ++n; else { allOk = false; break; }
    }
    if (allOk) {
        for (const auto& m : wal_.ReadMatchResults()) {
            std::vector<MatchResultRow> rows;
            for (const auto& r : m.rows) rows.push_back({r.name, r.kills, r.deaths});
            if (mysql_.InsertMatchResults(m.matchId, rows, m.endedAtUtcMs, m.configVersion)) ++n;
            else { allOk = false; break; }
        }
    }
    if (allOk && n > 0) { wal_.Truncate(); LOG_INFO("[DB] WAL 回放完成 " + std::to_string(n) + " 条→MySQL"); }
    return n;
}

// UtcNowMs 本地实现（审计/开档墙钟——与游戏内 steady 单钟分立·§3.4）
std::uint64_t RealPersistence::UtcNowMs_() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace adapters

#endif // SIGHTLINE_HAS_DB
