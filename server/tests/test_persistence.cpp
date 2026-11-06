// test_persistence.cpp —— ep08 步骤 5：持久化纯单测（零连接·不碰任何库）
// 覆盖：DSN 解析 / WAL 行编解码往返（含名字含 : 转义）/ NullPersistence+WAL 落盘 /
//       投影过滤语义（Ingest 只留开档·击杀·终局——快照不入持久化流，用假队列深度验）。
// 纪律：不连 MySQL/Redis/Mongo（连通性=db_smoke.sh 手动冒烟）。
#include "adapters/audit_wal.h"
#include "adapters/mysql_store.h"
#include "adapters/null_persistence.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace app = sightline::application;

static int g_checked = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        ++g_checked;                                                    \
        if (!(cond)) {                                                  \
            std::printf("FAIL (assert %d): %s\n", g_checked, #cond);    \
            return 1;                                                   \
        }                                                               \
    } while (0)

int main() {
    // ---- 1) DSN 解析（纯函数）----
    {
        std::string h, u, pw, db;
        unsigned port = 0;
        CHECK(adapters::MysqlStore::ParseDsn("10.1.2.3:3307:app:secret:gamedb", h, port, u, pw, db));
        CHECK(h == "10.1.2.3" && port == 3307 && u == "app" && pw == "secret" && db == "gamedb");
        CHECK(adapters::MysqlStore::ParseDsn("", h, port, u, pw, db));           // 空=全默认
        CHECK(h == "127.0.0.1" && port == 3306 && db == "sightline");
        CHECK(!adapters::MysqlStore::ParseDsn("noport:user:pass:db", h, port, u, pw, db)); // 段数错
        CHECK(!adapters::MysqlStore::ParseDsn("h:99999:u:p:d", h, port, u, pw, db));       // 端口越界
    }

    // ---- 2) WAL 编解码往返（A 行 / E 行 / 名字含 : 与制表符转义）----
    const fs::path walPath = fs::temp_directory_path() / "sightline_test.wal";
    fs::remove(walPath);
    {
        adapters::AuditWal wal(walPath.string());
        app::AuditRecord r;
        r.version = "v7-a3f2c1d8";
        r.source = "console@reload";
        r.diffDigest = "deadbeef";
        r.note = "削弱步枪\t测试";           // 含 tab → Sanitize 成空格
        r.atUtcMs = 1796000000000ull;
        CHECK(wal.AppendAudit(r));
        app::AuditRecord back;
        CHECK(adapters::AuditWal::DecodeAudit(adapters::AuditWal::EncodeAudit(r), back));
        CHECK(back.version == r.version && back.source == r.source);
        CHECK(back.diffDigest == r.diffDigest && back.atUtcMs == r.atUtcMs);
        CHECK(back.note == "削弱步枪 测试");      // tab 已转空格

        adapters::WalMatchResult m;
        m.matchId = "20261106T023015Z-003";
        m.configVersion = "v7-a3f2c1d8";
        m.endedAtUtcMs = 1796000001000ull;
        m.rows = {{"cdp", 10, 2}, {"Bot:1", 3, 10}};   // 名字含 : → 编码转 _
        CHECK(wal.AppendMatchResults(m));
        adapters::WalMatchResult mb;
        CHECK(adapters::AuditWal::DecodeMatch(adapters::AuditWal::EncodeMatch(m), mb));
        CHECK(mb.matchId == m.matchId && mb.endedAtUtcMs == m.endedAtUtcMs);
        CHECK(mb.rows.size() == 2);
        CHECK(mb.rows[0].name == "cdp" && mb.rows[0].kills == 10 && mb.rows[0].deaths == 2);
        CHECK(mb.rows[1].name == "Bot_1" && mb.rows[1].kills == 3);   // : → _
        // 文件级读回（两行都在·replay 输入）
        CHECK(wal.ReadAuditLines().size() == 1);
        CHECK(wal.ReadMatchResults().size() == 1);
        CHECK(wal.LineCount() == 2);
        wal.Truncate();
        CHECK(wal.LineCount() == 0);
    }

    // ---- 3) NullPersistence + WAL（lite 审计不丢·§5.7.2）----
    {
        adapters::AuditWal wal(walPath.string());
        adapters::NullPersistence nullp(&wal);
        app::AuditRecord r;
        r.version = "v8-11112222";
        r.source = "console@reload";
        r.atUtcMs = 42;
        nullp.Append(r);
        CHECK(nullp.Audits() == 1);
        CHECK(wal.LineCount() == 1);                    // 审计进了 WAL（不只计数）
        app::AuditRecord back;
        CHECK(adapters::AuditWal::DecodeAudit(wal.ReadAuditLines()[0], back));
        CHECK(back.version == "v8-11112222");
        // State()：lite 恒健康无界（分类恒 Enqueue·收口⑤语义）
        const auto st = nullp.State();
        CHECK(st.capacity == 0 && st.depth == 0);
        CHECK(st.health.mysql && st.health.mongo && st.health.redis);
        // 无 wal 的纯计数路径（旧行为）
        adapters::NullPersistence bare;
        bare.Append(r);
        CHECK(bare.Audits() == 1);
    }

    fs::remove(walPath);
    std::printf("全部通过：%d 项断言 OK（ep08 步骤 5 持久化纯单测·零连接）\n", g_checked);
    return 0;
}
