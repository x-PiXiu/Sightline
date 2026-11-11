// 构建守卫：无三库客户端环境（SIGHTLINE_HAS_DB=0）下本文件编译为空 TU——
// 头文件声明零驱动依赖（前置声明），使用点（main 的 full 分支）同受宏门控。
#if defined(SIGHTLINE_HAS_DB) && SIGHTLINE_HAS_DB
// redis_store.cpp —— ep08 步骤 5：Redis sink 实现（唯一 include hiredis/hiredis.h 的翻译单元）
#include "adapters/redis_store.h"

#include "logger/logger.h"

#include <hiredis/hiredis.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace adapters {

RedisStore::~RedisStore() { Disconnect(); }

bool RedisStore::Connect(const std::string& uri) {
    if (ctx_) return true;
    std::string host = "127.0.0.1";
    int port = 6379;
    std::string pass;
    if (!uri.empty()) {
        // host:port[:password]——三段式（面板 Redis requirepass）
        const auto p1 = uri.find(':');
        if (p1 != std::string::npos) {
            host = uri.substr(0, p1);
            const auto p2 = uri.find(':', p1 + 1);
            try { port = std::stoi(uri.substr(p1 + 1, (p2 == std::string::npos ? std::string::npos : p2 - p1 - 1))); }
            catch (...) { return false; }
            if (p2 != std::string::npos) pass = uri.substr(p2 + 1);
        } else {
            host = uri;
        }
    }
    struct timeval tv{2, 0};               // 2s 超时——挂掉的库不能拖住 worker
    ctx_ = redisConnectWithTimeout(host.c_str(), port, tv);
    if (!ctx_ || ctx_->err) {
        lastErr_ = ctx_ ? ctx_->errstr : "alloc failed";
        LOG_WARNING("[Redis] 连接失败 " + host + ":" + std::to_string(port) + " — " + lastErr_);
        if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
        return false;
    }
    if (!pass.empty()) {                    // AUTH（面板 Redis requirepass）
        redisReply* ar = static_cast<redisReply*>(redisCommand(ctx_, "AUTH %s", pass.c_str()));
        const bool authed = ar && ar->type != REDIS_REPLY_ERROR;
        if (!authed) lastErr_ = ar ? (ar->str ? ar->str : "AUTH error") : "AUTH null";
        if (ar) freeReplyObject(ar);
        if (!authed) { LOG_WARNING("[Redis] AUTH 失败 — " + lastErr_); Disconnect(); return false; }
    }
    LOG_INFO("[Redis] 已连接 " + host + ":" + std::to_string(port) + (pass.empty() ? "" : "（已认证）"));
    return true;
}

void RedisStore::Disconnect() {
    if (ctx_) { redisFree(ctx_); ctx_ = nullptr; }
}

bool RedisStore::Cmd(const char* fmt, ...) {
    if (!ctx_) return false;
    va_list ap;
    va_start(ap, fmt);
    redisReply* r = reinterpret_cast<redisReply*>(redisvCommand(ctx_, fmt, ap));
    va_end(ap);
    if (!r) {                            // 连接层错误（ctx_->err 已置位）
        lastErr_ = ctx_ ? ctx_->errstr : "null reply";
        return false;
    }
    const bool ok = (r->type != REDIS_REPLY_ERROR);
    if (!ok) lastErr_ = r->str ? r->str : "redis error";
    freeReplyObject(r);
    return ok;
}

bool RedisStore::OnKill(const std::string& playerName) {
    // ZINCRBY key +1 member——名字作 member（榜单可读；MySQL 是真相可重建）
    return Cmd("ZINCRBY leaderboard 1 %s", playerName.c_str());
}

bool RedisStore::SetScore(const std::string& playerName, long long score) {
    return Cmd("ZADD leaderboard %lld %s", score, playerName.c_str());
}

bool RedisStore::ClearLeaderboard() {
    return Cmd("DEL leaderboard");
}

bool RedisStore::Ban(const std::string& name, std::uint32_t ttlSec) {
    return Cmd("SETEX ban:%s %u 1", name.c_str(), static_cast<unsigned>(ttlSec));
}

bool RedisStore::SetSession(std::uint32_t accountId, std::uint32_t ttlSec) {
    return Cmd("SETEX sess:%u %u 1", accountId, static_cast<unsigned>(ttlSec));
}

bool RedisStore::IsBanned(const std::string& name) {
    if (!ctx_) return false;
    redisReply* r = reinterpret_cast<redisReply*>(
        redisCommand(ctx_, "EXISTS ban:%s", name.c_str()));
    if (!r) { lastErr_ = ctx_ ? ctx_->errstr : "null reply"; return false; }
    const bool banned = (r->type == REDIS_REPLY_INTEGER && r->integer == 1);
    freeReplyObject(r);
    return banned;
}

} // namespace adapters

#endif // SIGHTLINE_HAS_DB
