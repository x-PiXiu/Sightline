// 构建守卫：无三库客户端环境（SIGHTLINE_HAS_DB=0）下本文件编译为空 TU——
// 头文件声明零驱动依赖（前置声明），使用点（main 的 full 分支）同受宏门控。
#if defined(SIGHTLINE_HAS_DB) && SIGHTLINE_HAS_DB
// mysql_store.cpp —— ep08 步骤 5：MySQL sink 实现（唯一 include mysql/mysql.h 的翻译单元）
#include "adapters/mysql_store.h"

#include "logger/logger.h"

#include <mysql/mysql.h>

#include <cstdlib>
#include <chrono>
#include <functional>
#include <cstring>

namespace adapters {
namespace app = sightline::application;

void MysqlStore::ThreadAttach() { mysql_thread_init(); }
void MysqlStore::ThreadExit() { mysql_thread_end(); }

MysqlStore::~MysqlStore() { Disconnect(); }

bool MysqlStore::ParseDsn(const std::string& dsn, std::string& host, unsigned& port,
                          std::string& user, std::string& pass, std::string& db) {
    // host:port:user:pass:db ——五段冒号；空串=全默认（本机 3306/root//sightline）
    host = "127.0.0.1"; port = 3306; user = "root"; pass.clear(); db = "sightline";
    if (dsn.empty()) return true;
    std::string parts[5];
    std::size_t b = 0;
    for (int i = 0; i < 4; ++i) {          // 前 4 段取到第 4 个冒号（db 段可含冒号？不——db 不含）
        const auto p = dsn.find(':', b);
        if (p == std::string::npos) return false;
        parts[i] = dsn.substr(b, p - b);
        b = p + 1;
    }
    parts[4] = dsn.substr(b);
    if (parts[0].empty() || parts[2].empty() || parts[4].empty()) return false;
    host = parts[0];
    try { port = static_cast<unsigned>(std::stoul(parts[1])); } catch (...) { return false; }
    if (port == 0 || port > 65535) return false;
    user = parts[2];
    pass = parts[3];
    db = parts[4];
    return true;
}

bool MysqlStore::Connect(const std::string& dsn) {
    if (conn_) return true;
    if (!ParseDsn(dsn, host_, port_, user_, pass_, db_)) {
        lastErr_ = "DSN 解析失败（host:port:user:pass:db）: " + dsn;
        LOG_WARNING("[MySQL] " + lastErr_);
        return false;
    }
    conn_ = mysql_init(nullptr);            // ⚠ 调用方线程须已 ThreadAttach
    if (!conn_) { lastErr_ = "mysql_init 失败"; return false; }
    unsigned to = 5;
    mysql_options(conn_, MYSQL_OPT_CONNECT_TIMEOUT, &to);
    mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
    if (!mysql_real_connect(conn_, host_.c_str(), user_.c_str(), pass_.c_str(),
                            db_.c_str(), port_, nullptr, 0)) {
        lastErr_ = mysql_error(conn_);
        LOG_WARNING("[MySQL] 连接失败 " + host_ + ":" + std::to_string(port_) +
                    "/" + db_ + " — " + lastErr_);
        mysql_close(conn_);
        conn_ = nullptr;
        return false;
    }
    LOG_INFO("[MySQL] 已连接 " + host_ + ":" + std::to_string(port_) + "/" + db_);
    return true;
}

void MysqlStore::Disconnect() {
    if (conn_) {
        mysql_close(conn_);
        conn_ = nullptr;
    }
}

std::string MysqlStore::Escape(const std::string& s) {
    // 名字=客户端可控输入——注入防线；连接已设 utf8mb4，转义长度上界 2x+1
    if (!conn_) return {};
    std::string out(s.size() * 2 + 1, '\0');
    const unsigned long n = mysql_real_escape_string(conn_, out.data(), s.c_str(),
                                                     static_cast<unsigned long>(s.size()));
    out.resize(n);
    return out;
}

bool MysqlStore::Exec(const std::string& sql) {
    if (!conn_) return false;
    if (mysql_query(conn_, sql.c_str()) != 0) {
        lastErr_ = mysql_error(conn_);
        LOG_WARNING("[MySQL] 执行失败: " + lastErr_);
        return false;
    }
    // INSERT/SELECT 都可能带结果集——SELECT 必须 store_result 否则下次查询报 commands out of sync
    if (MYSQL_RES* res = mysql_store_result(conn_)) mysql_free_result(res);
    return true;
}

bool MysqlStore::AppendAudit(const app::AuditRecord& r) {
    if (!conn_) return false;   // 连接由 persistence worker 启动/重试时建立（不在此处隐式连）
    const std::string sql =
        "INSERT INTO t_config_version(version, source, diff_digest, note, created_at) VALUES('" +
        Escape(r.version) + "','" + Escape(r.source) + "','" + Escape(r.diffDigest) +
        "','" + Escape(r.note) + "'," + std::to_string(r.atUtcMs) + ")";
    return Exec(sql);
}

bool MysqlStore::InsertMatchResults(const std::string& matchId,
                                    const std::vector<MatchResultRow>& rows,
                                    std::uint64_t endedAtUtcMs,
                                    const std::string& configVersion) {
    if (!conn_) return false;
    if (rows.empty()) return true;
    std::string sql = "INSERT INTO t_match_result(match_id,player_name,kills,deaths,ended_at,config_version) VALUES";
    bool first = true;
    for (const auto& r : rows) {
        if (!first) sql += ",";
        first = false;
        sql += "('" + Escape(matchId) + "','" + Escape(r.name) + "'," +
               std::to_string(r.kills) + "," + std::to_string(r.deaths) + "," +
               std::to_string(endedAtUtcMs) + ",'" + Escape(configVersion) + "')";
    }
    return Exec(sql);
}

bool MysqlStore::QueryKillsByPlayer(std::vector<std::pair<std::string, int>>& out) {
    out.clear();
    if (!conn_) return false;
    if (mysql_query(conn_, "SELECT player_name, SUM(kills) FROM t_match_result "
                           "GROUP BY player_name ORDER BY SUM(kills) DESC") != 0) {
        lastErr_ = mysql_error(conn_);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return false;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr) {
        out.emplace_back(row[0] ? row[0] : "", row[1] ? std::atoi(row[1]) : 0);
    }
    mysql_free_result(res);
    return true;
}

// ===== ep09 步骤 5（刀 B）：账号域三方法 =====
bool MysqlStore::QueryRows(const std::string& sql,
                           const std::function<bool(std::uint32_t, char**)>& rowFn) {
    if (!conn_) return false;
    if (mysql_query(conn_, sql.c_str()) != 0) { lastErr_ = mysql_error(conn_); return false; }
    MYSQL_RES* res = mysql_store_result(conn_);
    if (!res) return false;
    const std::uint32_t cols = mysql_num_fields(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
        if (!rowFn(cols, row)) break;
    mysql_free_result(res);
    return true;
}

MysqlStore::RegResult MysqlStore::RegisterAccount(const std::string& name,
                                                  const std::string& passHash,
                                                  std::uint32_t& outAccountId) {
    if (!conn_) return RegResult::Error;
    // 查重前置（UNIQUE 撞库也可兜——显式查重给明确 reason）
    std::string existingHash; std::uint32_t existingId = 0;
    if (LookupAccount(name, existingHash, existingId)) return RegResult::Duplicate;
    const std::string sql = "INSERT INTO t_account (name, pass_hash, created_at) VALUES ('" +
                            Escape(name) + "', '" + Escape(passHash) + "', " +
                            std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count()) + ")";
    if (!Exec(sql)) return RegResult::Error;
    outAccountId = static_cast<std::uint32_t>(mysql_insert_id(conn_));
    return RegResult::Ok;
}

bool MysqlStore::LookupAccount(const std::string& name, std::string& outPassHash,
                               std::uint32_t& outAccountId) {
    bool found = false;
    QueryRows("SELECT id, pass_hash FROM t_account WHERE name='" + Escape(name) + "'",
              [&](std::uint32_t, char** cols) {
                  outAccountId = cols[0] ? static_cast<std::uint32_t>(std::atoi(cols[0])) : 0;
                  outPassHash = cols[1] ? cols[1] : "";
                  found = true;
                  return false;                      // 单行
              });
    return found;
}

bool MysqlStore::QueryStatsByAccount(std::uint32_t accountId, std::uint32_t limit,
                                     std::vector<StatRow>& out) {
    out.clear();
    return QueryRows("SELECT match_id, kills, deaths, ended_at FROM t_match_result "
                     "WHERE account_id=" + std::to_string(accountId) +
                     " ORDER BY ended_at DESC LIMIT " + std::to_string(limit),
                     [&](std::uint32_t, char** cols) {
                         StatRow r;
                         r.matchId = cols[0] ? static_cast<std::uint32_t>(std::strtoul(cols[0], nullptr, 10)) : 0;
                         r.kills = cols[1] ? static_cast<int>(std::atoi(cols[1])) : 0;
                         r.deaths = cols[2] ? static_cast<int>(std::atoi(cols[2])) : 0;
                         r.endedAt = cols[3] ? std::strtoull(cols[3], nullptr, 10) : 0;
                         out.push_back(r);
                         return true;
                     });
}

} // namespace adapters

#endif // SIGHTLINE_HAS_DB
