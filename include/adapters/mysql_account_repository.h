// mysql_account_repository.h —— IAccountRepository 的 MySQL 实现（服务端 02 文档）
//
// header-only 适配器：持有 MysqlPool，全部 SQL 走 escape 转义。
// 只在 StorageIO 存储线程调用（同步阻塞 API）。

#pragma once

#include "application/ports/i_account_repository.h"
#include "storage/mysql_pool.h"
#include "logger/logger.h"

namespace sightline::app {

class MySqlAccountRepository final : public IAccountRepository
{
public:
    explicit MySqlAccountRepository(std::shared_ptr<storage::MysqlPool> pool)
        : pool_(std::move(pool)) {}

    std::optional<AccountRecord> load(const std::string& account) override
    {
        auto conn = pool_->acquire();
        const std::string esc = conn->escape(account);
        auto res = conn->query(
            "SELECT a.account_id, a.account, a.nickname, a.pass_hash, "
            "IFNULL(s.wins,0), IFNULL(s.losses,0), IFNULL(s.kills,0), IFNULL(s.deaths,0) "
            "FROM account a LEFT JOIN player_stats s ON s.account_id = a.account_id "
            "WHERE a.account='" + esc + "'");
        if (!res || res->rowCount() == 0) return std::nullopt;

        MYSQL_ROW r = res->fetchRow();
        AccountRecord rec;
        rec.accountId = r[0] ? std::strtoull(r[0], nullptr, 10) : 0;
        rec.account   = r[1] ? r[1] : "";
        rec.nickname  = r[2] ? r[2] : "";
        rec.passHash  = r[3] ? r[3] : "";
        rec.wins      = r[4] ? std::atoi(r[4]) : 0;
        rec.losses    = r[5] ? std::atoi(r[5]) : 0;
        rec.kills     = r[6] ? std::atoi(r[6]) : 0;
        rec.deaths    = r[7] ? std::atoi(r[7]) : 0;
        return rec;
    }

    bool create(const AccountRecord& record) override
    {
        auto conn = pool_->acquire();
        const std::string escAcc  = conn->escape(record.account);
        const std::string escNick = conn->escape(record.nickname);
        const std::string escHash = conn->escape(record.passHash);
        const bool ok = conn->execute(
            "INSERT INTO account (account, pass_hash, nickname) VALUES ('" +
            escAcc + "', '" + escHash + "', '" + escNick + "')");
        if (!ok) LOG_ERROR(std::string("[AccountRepo] create 失败: ") + conn->lastError());
        return ok;
    }

    void addStats(std::uint64_t accountId, int wins, int losses, int kills, int deaths) override
    {
        auto conn = pool_->acquire();
        conn->execute(
            "INSERT INTO player_stats (account_id, wins, losses, kills, deaths) VALUES (" +
            std::to_string(accountId) + "," + std::to_string(wins) + "," + std::to_string(losses) + "," +
            std::to_string(kills) + "," + std::to_string(deaths) + ") "
            "ON DUPLICATE KEY UPDATE wins=wins+" + std::to_string(wins) +
            ", losses=losses+" + std::to_string(losses) +
            ", kills=kills+" + std::to_string(kills) +
            ", deaths=deaths+" + std::to_string(deaths));
    }

private:
    std::shared_ptr<storage::MysqlPool> pool_;
};

} // namespace sightline::app
