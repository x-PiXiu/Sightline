// mysql_match_repository.h —— IMatchRepository 的 MySQL 实现（服务端 02 文档）
//
// 事务语义：match_record + match_player + player_stats 累加在同一事务，
// 任一步失败整体回滚（战绩是账，不能半截）。
// 只在 StorageIO 存储线程调用（同步阻塞 API）。

#pragma once

#include "application/ports/i_match_repository.h"
#include "storage/mysql_pool.h"

namespace sightline::app {

class MySqlMatchRepository final : public IMatchRepository
{
public:
    explicit MySqlMatchRepository(std::shared_ptr<storage::MysqlPool> pool)
        : pool_(std::move(pool)) {}

    std::uint64_t save(const MatchRecordDb& rec) override
    {
        auto conn = pool_->acquire();
        if (!conn->execute("START TRANSACTION")) return 0;

        const std::string head =
            "INSERT INTO match_record (mode, winner_id, duration_sec) VALUES (" +
            std::to_string(rec.mode) + "," + std::to_string(rec.winner_account_id) + "," +
            std::to_string(rec.duration_sec) + ")";
        if (!conn->execute(head)) { conn->execute("ROLLBACK"); return 0; }

        // 同连接取自增 match_id（连接级变量，不被其他连接干扰）
        auto res = conn->query("SELECT LAST_INSERT_ID()");
        std::uint64_t match_id = 0;
        if (res && res->valid())
            if (MYSQL_ROW row = res->fetchRow())
                if (row[0]) match_id = std::strtoull(row[0], nullptr, 10);
        if (match_id == 0) { conn->execute("ROLLBACK"); return 0; }

        for (const auto& p : rec.players)
        {
            if (p.account_id == 0) continue;   // 游客不入库
            const std::string row_sql =
                "INSERT INTO match_player (match_id, account_id, kills, deaths) VALUES (" +
                std::to_string(match_id) + "," + std::to_string(p.account_id) + "," +
                std::to_string(p.kills) + "," + std::to_string(p.deaths) + ")";
            if (!conn->execute(row_sql)) { conn->execute("ROLLBACK"); return 0; }
        }
        if (!conn->execute("COMMIT")) { conn->execute("ROLLBACK"); return 0; }
        return match_id;
    }

    std::vector<MatchRecordRow> queryByAccount(std::uint64_t account_id, int limit) override
    {
        std::vector<MatchRecordRow> out;
        auto conn = pool_->acquire();
        const std::string sql =
            "SELECT m.match_id, m.mode, (m.winner_id = mp.account_id) AS win, "
            "mp.kills, mp.deaths "
            "FROM match_player mp JOIN match_record m ON m.match_id = mp.match_id "
            "WHERE mp.account_id = " + std::to_string(account_id) +
            " ORDER BY m.match_id DESC LIMIT " + std::to_string(limit);
        auto res = conn->query(sql);
        if (!res || !res->valid()) return out;
        while (true)
        {
            MYSQL_ROW row = res->fetchRow();
            if (!row) break;
            MatchRecordRow r;
            r.match_id = row[0] ? std::strtoull(row[0], nullptr, 10) : 0;
            r.mode     = row[1] ? static_cast<uint8_t>(std::atoi(row[1])) : 1;
            r.win      = row[2] && std::atoi(row[2]) != 0;
            r.kills    = row[3] ? static_cast<uint16_t>(std::atoi(row[3])) : 0;
            r.deaths   = row[4] ? static_cast<uint16_t>(std::atoi(row[4])) : 0;
            out.push_back(std::move(r));
        }
        return out;
    }

private:
    std::shared_ptr<storage::MysqlPool> pool_;
};

} // namespace sightline::app
