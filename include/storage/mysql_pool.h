// mysql_pool.h —— libmysqlclient RAII 封装（服务端 01 文档 4.1）
//
// 三层：MysqlResult(结果守卫) / MysqlConnection(连接 RAII+断线重连+转义) / MysqlPool(取借还)。
// 全部同步阻塞 API——只在 StorageIO 存储线程调用，禁止 epoll 线程使用。
// 安全约定：账号名在协议边界校验(≤32B)；SQL 一律走 escape() 转义，prepared statement 列入加固。

#pragma once

#include <mysql.h>
#include <string>
#include <memory>
#include <vector>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace sightline::storage {

struct MysqlConfig
{
    std::string host = "127.0.0.1";
    int         port = 3306;
    std::string user;
    std::string password;
    std::string database;
    int         pool_size = 4;
};

/** 结果集 RAII：析构自动 mysql_free_result（禁拷贝、可移动） */
class MysqlResult
{
public:
    explicit MysqlResult(MYSQL_RES* res) : res_(res) {}
    ~MysqlResult() { if (res_) mysql_free_result(res_); }
    MysqlResult(const MysqlResult&) = delete;
    MysqlResult& operator=(const MysqlResult&) = delete;
    MysqlResult(MysqlResult&& o) noexcept : res_(o.res_) { o.res_ = nullptr; }
    MysqlResult& operator=(MysqlResult&& o) noexcept
    {
        if (this != &o) { if (res_) mysql_free_result(res_); res_ = o.res_; o.res_ = nullptr; }
        return *this;
    }

    bool       valid() const { return res_ != nullptr; }
    my_ulonglong rowCount() const { return res_ ? mysql_num_rows(res_) : 0; }
    MYSQL_ROW  fetchRow() { return res_ ? mysql_fetch_row(res_) : nullptr; }

private:
    MYSQL_RES* res_ = nullptr;
};

/** 连接 RAII：构造连接、析构关闭；ensureConnected 防 MySQL 8 小时空闲断连 */
class MysqlConnection
{
public:
    explicit MysqlConnection(const MysqlConfig& cfg) : cfg_(cfg) { connect(); }
    ~MysqlConnection() { if (conn_) mysql_close(conn_); }

    MysqlConnection(const MysqlConnection&) = delete;
    MysqlConnection& operator=(const MysqlConnection&) = delete;

    bool connected() const { return conn_ != nullptr; }

    /** 最近一次 MySQL 错误文本（诊断用） */
    std::string lastError() const { return conn_ ? mysql_error(conn_) : "no connection"; }

    bool ensureConnected()
    {
        if (conn_ && mysql_ping(conn_) == 0) return true;
        return connect();
    }

    /** SELECT：失败返回 nullopt；成功返回结果守卫(可能 0 行) */
    std::optional<MysqlResult> query(const std::string& sql)
    {
        if (!ensureConnected()) return std::nullopt;
        if (mysql_query(conn_, sql.c_str()) != 0) return std::nullopt;
        if (MYSQL_RES* r = mysql_store_result(conn_)) return MysqlResult(r);
        return std::nullopt;   // 无结果集（不应发生在 SELECT）
    }

    /** INSERT/UPDATE/DELETE：成功返回 true */
    bool execute(const std::string& sql)
    {
        if (!ensureConnected()) return false;
        return mysql_query(conn_, sql.c_str()) == 0;
    }

    /** 转义（防注入）：用于必须拼接的标识/值 */
    std::string escape(const std::string& s)
    {
        if (!ensureConnected()) return s;
        std::vector<char> buf(s.size() * 2 + 1);
        const unsigned long n = mysql_real_escape_string_quote(conn_, buf.data(),
                                                               s.c_str(), static_cast<unsigned long>(s.size()), '\'');
        return std::string(buf.data(), n);
    }

private:
    bool connect()
    {
        if (conn_) { mysql_close(conn_); conn_ = nullptr; }
        conn_ = mysql_init(nullptr);
        if (!conn_) return false;
        mysql_options(conn_, MYSQL_SET_CHARSET_NAME, "utf8mb4");
        return mysql_real_connect(conn_, cfg_.host.c_str(), cfg_.user.c_str(),
                                  cfg_.password.c_str(), cfg_.database.c_str(),
                                  static_cast<unsigned int>(cfg_.port), nullptr, 0) != nullptr;
    }

    MYSQL*      conn_ = nullptr;
    MysqlConfig cfg_;
};

/** 连接池：acquire 借出（自定义删除器自动归还），业务零手动还 */
class MysqlPool
{
public:
    explicit MysqlPool(MysqlConfig cfg, int poolSize) : cfg_(std::move(cfg))
    {
        for (int i = 0; i < poolSize; ++i)
            idle_.push_back(std::make_shared<MysqlConnection>(cfg_));
    }

    std::shared_ptr<MysqlConnection> acquire()
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !idle_.empty(); });
        auto conn = idle_.front();
        idle_.pop_front();
        lk.unlock();
        // 借出者持有 shared_ptr 副本保活；归还 deleter 把连接放回空闲队列
        return std::shared_ptr<MysqlConnection>(conn.get(), [this, conn](MysqlConnection*) {
            std::lock_guard<std::mutex> lk2(mtx_);
            idle_.push_back(conn);
            cv_.notify_all();
        });
    }

private:
    MysqlConfig cfg_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::shared_ptr<MysqlConnection>> idle_;
};

} // namespace sightline::storage
