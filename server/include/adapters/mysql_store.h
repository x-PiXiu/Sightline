// mysql_store.h —— ep08 步骤 5：MySQL sink（真相库·must·libmysqlclient C API）
// 只做连接与搬运（谦卑对象）：SQL 构造/转义是格式层；写失败=返回 false 由 worker 记健康位。
// 连接串 SIGHTLINE_MYSQL_DSN = host:port:user:pass:db（例 127.0.0.1:3306:sightline:sightline:sightline）。
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "application/config_views.h"   // AuditRecord（用例层 POD）

struct MYSQL;   // mysql.h 前置声明（头不 include mysql.h——唯一 include 处=mysql_store.cpp）

namespace adapters {

struct MatchResultRow {                  // Ended 落库的一行（player_name 已由 persistence 译好）
    std::string name;
    int kills = 0;
    int deaths = 0;
};

class MysqlStore {
public:
    MysqlStore() = default;
    ~MysqlStore();                        // 断开连接

    MysqlStore(const MysqlStore&) = delete;
    MysqlStore& operator=(const MysqlStore&) = delete;

    // DSN 解析（纯函数·可单测）：host:port:user:pass:db；空串=默认本机
    static bool ParseDsn(const std::string& dsn, std::string& host, unsigned& port,
                         std::string& user, std::string& pass, std::string& db);

    bool Connect(const std::string& dsn);          // 幂等：已连直接 true；⚠ 调用线程须先 ThreadAttach
    void Disconnect();
    // libmysqlclient 线程纪律：每个使用连接的线程 enter/exit 一次（persistence worker 负责）
    static void ThreadAttach();                    // mysql_thread_init
    static void ThreadExit();                      // mysql_thread_end
    bool Connected() const { return conn_ != nullptr; }

    bool AppendAudit(const sightline::application::AuditRecord& r);
    bool InsertMatchResults(const std::string& matchId,
                            const std::vector<MatchResultRow>& rows,
                            std::uint64_t endedAtUtcMs,
                            const std::string& configVersion);
    // G7 Rebuild 数据源：按玩家汇总 kills（真相对 Redis 投影）
    bool QueryKillsByPlayer(std::vector<std::pair<std::string, int>>& out);

    const std::string& LastError() const { return lastErr_; }

private:
    bool Exec(const std::string& sql);              // mysql_query + 错误记账
    std::string Escape(const std::string& s);       // mysql_real_escape_string（名字=客户端可控输入）

    MYSQL* conn_ = nullptr;                         // void* 语义：头文件零 mysql 依赖
    std::string host_, user_, pass_, db_;
    unsigned port_ = 3306;
    std::string lastErr_;
};

} // namespace adapters
