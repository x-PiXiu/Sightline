// test_mysql_live.cpp —— 真库直连诊断（临时工具）：构造→插入→查询 全链打印错误
#include "storage/mysql_pool.h"
#include <cstdio>

int main()
{
    sightline::storage::MysqlConfig cfg;
    cfg.host     = "172.17.153.223";
    cfg.port     = 3306;
    cfg.user     = "root";
    cfg.password = "123456";
    cfg.database = "sightline";

    sightline::storage::MysqlConnection conn(cfg);
    std::printf("connected=%d lastError=%s\n", (int)conn.connected(), conn.lastError().c_str());

    const std::string insert =
        "INSERT INTO account (account, pass_hash, nickname) VALUES ('dbglive', '" +
        std::string(64, 'a') + "', 'dbglive')";
    if (!conn.execute(insert))
        std::printf("insert failed: %s\n", conn.lastError().c_str());
    else
        std::printf("insert OK\n");

    auto res = conn.query("SELECT account_id, account FROM account");
    if (!res || !res->valid()) { std::printf("query failed\n"); return 1; }
    while (true)
    {
        MYSQL_ROW row = res->fetchRow();
        if (!row) break;
        std::printf("row: id=%s account=%s\n", row[0] ? row[0] : "?", row[1] ? row[1] : "?");
    }
    return 0;
}
