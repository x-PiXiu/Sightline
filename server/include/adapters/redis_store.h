// redis_store.h —— ep08 步骤 5：Redis sink（热数据投影·hiredis C API）
// 击杀榜 ZSET（MySQL 是真相·Redis 是投影——§5.4 doctrine，REBUILD 可重建）+
// 封禁通道（ep08 只建通·IBanStore 语义，ep09 消费）。
// 连接串 SIGHTLINE_REDIS_URI = host:port[:password]（默认 127.0.0.1:6379；面板 Redis 带 requirepass 时补第三段）。
#pragma once
#include <cstdint>
#include <string>

struct redisContext;   // hiredis 前置声明（头不 include hiredis.h）

namespace adapters {

class RedisStore {
public:
    RedisStore() = default;
    ~RedisStore();                        // 断开

    RedisStore(const RedisStore&) = delete;
    RedisStore& operator=(const RedisStore&) = delete;

    bool Connect(const std::string& uri); // host:port；幂等
    void Disconnect();
    bool Connected() const { return ctx_ != nullptr; }

    // 击杀榜投影：ZINCRBY leaderboard +1 member=name（§5.4）
    bool OnKill(const std::string& playerName);
    // 榜重建（G7：persistence 编排 mysql 读→此处写；score=覆盖式 ZADD）
    bool SetScore(const std::string& playerName, long long score);
    bool ClearLeaderboard();

    // 封禁通道（IBanStore 语义·ep09 消费）：TTL 秒级临时封禁
    bool Ban(const std::string& name, std::uint32_t ttlSec);
    /** ep09 G9：登录会话键（sess:<accountId> EX 86400——BestEffort 失败不阻塞） */
    bool SetSession(std::uint32_t accountId, std::uint32_t ttlSec);
    bool IsBanned(const std::string& name);

    const std::string& LastError() const { return lastErr_; }

private:
    bool Cmd(const char* fmt, ...);       // redisCommand 包装 + 错误记账（须在 .cpp 用 va）

    redisContext* ctx_ = nullptr;
    std::string lastErr_;
};

} // namespace adapters
