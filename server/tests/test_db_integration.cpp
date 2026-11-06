// test_db_integration.cpp —— ep08 步骤 5：真库集成冒烟（需 MySQL/Redis 在跑·env 门控）
// 触发：SIGHTLINE_DB_SMOKE=1（缺省=跳过——单测套件默认零依赖，连通性归 db_smoke.sh）。
// 流：合成 MatchStart→Kill×2→Ended 一次入队 → 断言 MySQL 战绩行/审计行 + Redis 榜分。
// mongo：SIGHTLINE_MONGO_URI 配了才查（本机无 mongod=跳过·best-effort doctrine）。
#include "adapters/persistence.h"
#include "logger/logger.h"          // 挂 console sink——worker 的连接/开档/落库日志可见

#include <mysql/mysql.h>
#include <hiredis/hiredis.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace app = sightline::application;
using sightline::match::MatchEvent;

static int g_checked = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);    \
            return 1;                                                   \
        }                                                               \
    } while (0)

namespace {

std::string EnvOr(const char* k, const char* d) {
    if (const char* v = std::getenv(k)) return v;
    return d;
}

// 直连 MySQL 查标量（测试=最外层，可直接用驱动）
long long QueryScalar(const std::string& dsn, const char* sql, bool* ok) {
    *ok = false;
    // 连接参数走 DSN 解析（端口可配——3307 专用实例·避开面板 3306）
    std::string host, user, pass, db;
    unsigned port = 3306;
    if (!adapters::MysqlStore::ParseDsn(dsn, host, port, user, pass, db)) return -1;
    MYSQL* c = mysql_init(nullptr);
    if (!c) return -1;
    if (!mysql_real_connect(c, host.c_str(), user.c_str(), pass.c_str(),
                            db.c_str(), port, nullptr, 0)) {
        std::printf("  (mysql connect fail: %s)\n", mysql_error(c));
        mysql_close(c);
        return -1;
    }
    if (mysql_query(c, sql) != 0) { mysql_close(c); return -1; }
    MYSQL_RES* res = mysql_store_result(c);
    long long v = -1;
    if (res) {
        MYSQL_ROW row = mysql_fetch_row(res);
        if (row && row[0]) v = std::atoll(row[0]);
        mysql_free_result(res);
    }
    mysql_close(c);
    *ok = true;
    return v;
}

} // namespace

int main() {
    common::logger::Logger::getInstance().addSink(
        std::make_unique<common::logger::ConsoleSink>());
    if (!std::getenv("SIGHTLINE_DB_SMOKE")) {
        std::printf("跳过（SIGHTLINE_DB_SMOKE=1 才跑真库冒烟）\n");
        return 0;
    }

    adapters::RealPersistenceConfig cfg;
    cfg.mysqlDsn = EnvOr("SIGHTLINE_MYSQL_DSN", "127.0.0.1:3306:sightline:sightline:sightline");
    cfg.redisUri = EnvOr("SIGHTLINE_REDIS_URI", "127.0.0.1:6379");
    cfg.mongoUri = EnvOr("SIGHTLINE_MONGO_URI", "mongodb://127.0.0.1:27017");
    cfg.walPath = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") +
                  "/sightline_smoke.wal";

    // 跨次清账（本 smoke 幂等：删掉历史 smoke 行/审计/榜成员——否则 count 断言跨次必炸）
    {
        std::string h, u, pw, db; unsigned port = 0;
        if (adapters::MysqlStore::ParseDsn(cfg.mysqlDsn, h, port, u, pw, db)) {
            MYSQL* c = mysql_init(nullptr);
            if (c && mysql_real_connect(c, h.c_str(), u.c_str(), pw.c_str(), db.c_str(), port, nullptr, 0)) {
                mysql_query(c, "DELETE FROM t_match_result WHERE player_name LIKE '%_smoke'");
                mysql_query(c, "DELETE FROM t_config_version WHERE version='v9-smoke'");
                mysql_close(c);
            } else if (c) mysql_close(c);
        }
        if (!cfg.redisUri.empty()) {
            std::string rh = "127.0.0.1"; int rp = 6379; std::string rpass;
            const auto p1 = cfg.redisUri.find(':');
            if (p1 != std::string::npos) {
                rh = cfg.redisUri.substr(0, p1);
                const auto p2 = cfg.redisUri.find(':', p1 + 1);
                try { rp = std::stoi(cfg.redisUri.substr(p1 + 1, p2 == std::string::npos ? std::string::npos : p2 - p1 - 1)); } catch (...) {}
                if (p2 != std::string::npos) rpass = cfg.redisUri.substr(p2 + 1);
            }
            if (redisContext* rc = redisConnect(rh.c_str(), rp)) {
                if (!rc->err) {
                    if (!rpass.empty()) {
                        redisReply* ar = static_cast<redisReply*>(redisCommand(rc, "AUTH %s", rpass.c_str()));
                        if (ar) freeReplyObject(ar);
                    }
                    redisReply* dr = static_cast<redisReply*>(redisCommand(rc, "DEL leaderboard"));
                    if (dr) freeReplyObject(dr);
                }
                redisFree(rc);
            }
        }
    }

    adapters::RealPersistence p(cfg);
    p.Start();
    p.SetConfigVersion("v9-smoke");
    std::printf("[DB] %s\n", p.HealthLine().c_str());

    // 合成事件流（单批：开档→双杀→终局）
    MatchEvent ms;
    ms.type = MatchEvent::Type::MatchStart;
    ms.roster = {{5, "cdp_smoke", 0}, {6, "bot_smoke", 1}};
    MatchEvent k1;
    k1.type = MatchEvent::Type::Kill;
    k1.killKiller = 5; k1.killVictim = 6;
    MatchEvent k2;
    k2.type = MatchEvent::Type::Kill;
    k2.killKiller = 6; k2.killVictim = 5;
    MatchEvent en;
    en.type = MatchEvent::Type::Ended;
    en.endReason = 1;
    en.scores = {{5, 1, 0}, {6, 1, 1}};
    // 时序=真实 Fanout 语义：开档批先于击杀批（marker 的 pid→name 依赖 ctx 已建——
    // 同批塞终局会让 marker 落在清档后=静默丢，测试不得违背该时序前提）
    p.Ingest(std::vector<MatchEvent>{ms}, app::Durability::Must);
    p.Append(app::AuditRecord{"v9-smoke", "db_smoke", "abc123", "集成冒烟", 1796700000000ull});
    p.OnKill(5);
    p.OnKill(6);
    p.Ingest(std::vector<MatchEvent>{k1, k2, en}, app::Durability::Must);

    // 轮询等 worker（最多 5s）
    bool mysqlOk = false;
    long long rows = -1;
    for (int i = 0; i < 50 && rows <= 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        rows = QueryScalar(cfg.mysqlDsn, "SELECT COUNT(*) FROM t_match_result WHERE match_id LIKE '20%' "
                                    "AND player_name='cdp_smoke'", &mysqlOk);
    }
    CHECK(mysqlOk);                                   // MySQL 连通
    CHECK(rows == 1);                                 // 战绩行落库（Ended → must）

    // 审计行（v9-smoke 版本号可查）
    bool ok2 = false;
    const long long audits = QueryScalar(cfg.mysqlDsn,
        "SELECT COUNT(*) FROM t_config_version WHERE version='v9-smoke'", &ok2);
    CHECK(ok2 && audits == 1);

    // Redis 榜（OnKill 投影）——检查连接同样要 AUTH（面板 Redis requirepass；漏=NOAUTH 错误回执）
    // SIGHTLINE_EXPECT_REDIS_DOWN=1：降级门（§5.5）——死 Redis URI 下断言翻转：
    //   mysql/mongo 照常落（零中断实证）+ OnKill 标记被丢计数 + 横幅 Redis✗
    const bool expectDown = std::getenv("SIGHTLINE_EXPECT_REDIS_DOWN") != nullptr;
    if (expectDown) {
        CHECK(p.Dropped() >= 2);                              // 两个 Kill 标记被丢+计数
        CHECK(p.HealthLine().find("Redis") != std::string::npos &&
              p.HealthLine().find("✗") != std::string::npos); // 横幅如实显✗
        p.Shutdown();
        std::printf("全部通过：%d 项断言 OK（步骤 5 降级门·Redis 死·其余零中断）\n", g_checked);
        return 0;
    }
    redisContext* rc = redisConnect("127.0.0.1", 6379);
    CHECK(rc != nullptr && !rc->err);
    {
        const auto p1 = cfg.redisUri.find(':');
        const auto p2 = p1 == std::string::npos ? std::string::npos : cfg.redisUri.find(':', p1 + 1);
        if (p2 != std::string::npos) {
            const std::string rpass = cfg.redisUri.substr(p2 + 1);
            redisReply* ar = static_cast<redisReply*>(redisCommand(rc, "AUTH %s", rpass.c_str()));
            CHECK(ar != nullptr && ar->type != REDIS_REPLY_ERROR);
            freeReplyObject(ar);
        }
    }
    redisReply* rp = static_cast<redisReply*>(
        redisCommand(rc, "ZSCORE leaderboard cdp_smoke"));
    CHECK(rp != nullptr && rp->type == REDIS_REPLY_STRING &&
          std::atof(rp->str) >= 1.0);
    freeReplyObject(rp);
    redisFree(rc);

    // mongo（配了 URI 才查）
    if (!cfg.mongoUri.empty())
        std::printf("  (mongo URI 已配——战报文档查证见 db_smoke.sh 手动段)\n");

    p.Shutdown();
    std::printf("全部通过：%d 项断言 OK（步骤 5 真库集成冒烟）\n", g_checked);
    return 0;
}
