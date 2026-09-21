// redis_conn.h —— hiredis RAII 封装（服务端 01 文档 4.2）：AUTH + 语义化方法
//
// 同步阻塞 API——只在 StorageIO 存储线程调用。
// 断线/密码：ensureConnected 内统一处理（重连后自动重发 AUTH）。

#pragma once

#include <hiredis/hiredis.h>
#include <string>
#include <vector>
#include <optional>
#include <utility>

namespace sightline::storage {

struct RedisConfig
{
    std::string host = "127.0.0.1";
    int         port = 6379;
    std::string password;                 // 空 = 无 AUTH
};

/** reply 的 RAII：析构自动 freeReplyObject（禁拷贝、可移动） */
class RedisReply
{
public:
    explicit RedisReply(redisReply* r) : r_(r) {}
    ~RedisReply() { if (r_) freeReplyObject(r_); }
    RedisReply(const RedisReply&) = delete;
    RedisReply& operator=(const RedisReply&) = delete;
    RedisReply(RedisReply&& o) noexcept : r_(o.r_) { o.r_ = nullptr; }
    RedisReply& operator=(RedisReply&& o) noexcept
    {
        if (this != &o) { if (r_) freeReplyObject(r_); r_ = o.r_; o.r_ = nullptr; }
        return *this;
    }

    bool valid() const { return r_ != nullptr; }
    bool isArray() const { return r_ && r_->type == REDIS_REPLY_ARRAY; }
    bool isString() const { return r_ && r_->type == REDIS_REPLY_STRING; }
    bool isInteger() const { return r_ && r_->type == REDIS_REPLY_INTEGER; }
    long long asInteger() const { return r_ ? r_->integer : 0; }
    std::string asString() const { return r_ ? std::string(r_->str, r_->len) : std::string(); }
    size_t elements() const { return r_ ? r_->elements : 0; }
    redisReply* element(size_t i) const { return (r_ && i < r_->elements) ? r_->element[i] : nullptr; }
    redisReply* raw() const { return r_; }

private:
    redisReply* r_ = nullptr;
};

/** 连接 RAII + AUTH + 语义化命令 */
class RedisConnection
{
public:
    explicit RedisConnection(const RedisConfig& cfg) : cfg_(cfg) { connect(); }
    ~RedisConnection() { if (c_) redisFree(c_); }

    RedisConnection(const RedisConnection&) = delete;
    RedisConnection& operator=(const RedisConnection&) = delete;

    bool connected() const { return c_ != nullptr && c_->err == 0; }

    bool ensureConnected()
    {
        if (c_ && c_->err == 0)
        {
            // 直接发 PING，不经 command()——command 与本方法互调会无限递归（栈溢出）
            RedisReply r(static_cast<redisReply*>(redisCommand(c_, "PING")));
            if (r.valid() && r.asString() == "PONG") return true;
        }
        return connect();
    }

    /** 通用命令（hiredis printf 风格格式化）。
     *  Redis 不可用 → 返回空 reply（valid()==false），调用方按降级处理；
     *  绝不带空上下文调 redisvCommand（段错误）。 */
    RedisReply command(const char* fmt, ...)
    {
        if (!ensureConnected()) return RedisReply(nullptr);
        va_list args;
        va_start(args, fmt);
        void* r = redisvCommand(c_, fmt, args);
        va_end(args);
        return RedisReply(static_cast<redisReply*>(r));
    }

    // ---- 语义化方法 ----
    void setEx(const std::string& key, const std::string& val, int ttlSec)
    {
        auto r = command("SET %s %s EX %d", key.c_str(), val.c_str(), ttlSec);
    }
    std::optional<std::string> get(const std::string& key)
    {
        auto r = command("GET %s", key.c_str());
        if (!r.valid() || !r.isString()) return std::nullopt;
        return r.asString();
    }
    void sadd(const std::string& key, const std::string& member) { auto r = command("SADD %s %s", key.c_str(), member.c_str()); }
    void srem(const std::string& key, const std::string& member) { auto r = command("SREM %s %s", key.c_str(), member.c_str()); }
    void zincrby(const std::string& key, double incr, const std::string& member)
    {
        auto r = command("ZINCRBY %s %f %s", key.c_str(), incr, member.c_str());
    }
    std::vector<std::pair<std::string,double>> zrevrange(const std::string& key, int start, int stop)
    {
        std::vector<std::pair<std::string,double>> out;
        auto r = command("ZREVRANGE %s %d %d WITHSCORES", key.c_str(), start, stop);
        if (r.valid())
        {
            for (size_t i = 0; i + 1 < r.raw()->elements; i += 2)
                out.emplace_back(std::string(r.raw()->element[i]->str, r.raw()->element[i]->len),
                                 r.raw()->element[i + 1]->str ? std::atof(r.raw()->element[i + 1]->str) : 0.0);
        }
        return out;
    }

private:
    bool connect()
    {
        if (c_) { redisFree(c_); c_ = nullptr; }
        c_ = redisConnect(cfg_.host.c_str(), cfg_.port);
        if (!c_ || c_->err) { if (c_) { redisFree(c_); c_ = nullptr; } return false; }
        if (!cfg_.password.empty())
        {
            auto r = static_cast<redisReply*>(redisCommand(c_, "AUTH %s", cfg_.password.c_str()));
            const bool ok = r && r->type != REDIS_REPLY_ERROR;
            if (r) freeReplyObject(r);
            if (!ok) { redisFree(c_); c_ = nullptr; return false; }
        }
        return c_ != nullptr;
    }

    redisContext* c_ = nullptr;
    RedisConfig   cfg_;
};

} // namespace sightline::storage
