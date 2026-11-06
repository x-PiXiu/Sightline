// persistence.h —— ep08 步骤 5：full 模式持久化适配器（三水槽 + 双队列 + worker + 降级）
// §5.2 事件订阅（G8：用例层零持久化知识——MatchEvent 流已是边界，不开第二接口）；
// §5.3 双队列分级：审计无损（失败→WAL）/ 统计有界 1024（BestEffort 满→丢+计数）；
// §5.5 降级：任一库挂=该 sink 丢弃计数+告警、对局零中断；health 经 State() 自报给编排器分类。
// 单线程纪律：本适配器内部=1 个 worker 线程独占三库连接；Ingest/Append/OnKill 只入队（main 线程）。
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "adapters/audit_wal.h"
#include "adapters/mongo_store.h"
#include "adapters/mysql_store.h"
#include "adapters/redis_store.h"
#include "application/ports.h"

namespace adapters {

struct RealPersistenceConfig {
    std::string mysqlDsn;                  // SIGHTLINE_MYSQL_DSN（host:port:user:pass:db）
    std::string redisUri;                  // SIGHTLINE_REDIS_URI（host:port）
    std::string mongoUri;                  // SIGHTLINE_MONGO_URI（mongodb://...；空=不连）
    std::string walPath;                   // WAL 文件（spill + lite 审计共用）
    std::size_t statsCapacity = 1024;      // 统计队列上限（事件数·§5.3）
};

class RealPersistence final : public sightline::application::IPersistencePorts {
public:
    explicit RealPersistence(RealPersistenceConfig cfg);
    ~RealPersistence() override;

    RealPersistence(const RealPersistence&) = delete;
    RealPersistence& operator=(const RealPersistence&) = delete;

    // 启动（横幅前调）：同步连三库 → 起 worker；失败=health 置灰（降级常驻，不抛不退）
    void Start();
    // 停机三步（B6）：停 ingest → 2s 期限排水 → join（main 停机路径调）
    void Shutdown();

    // ---- IMatchArchive：事件流投影（worker 消费）----
    void Ingest(std::span<const sightline::application::MatchEvent> events,
                sightline::application::Durability d) override;
    sightline::application::QueueState State() const override;

    // ---- IAuditSink：配置审计（无损——失败转 WAL，审计不丢）----
    void Append(const sightline::application::AuditRecord& r) override;

    // ---- ILeaderboard：击杀榜投影（pid→name 由当前对局名册译；worker 写 Redis）----
    void OnKill(std::uint32_t killerPid) override;
    // G7：重建编排在本适配器内（mysql 读 → redis 覆盖写）——不许 store 互调
    void Rebuild() override;

    // 封禁通道（IBanStore 语义·ep09 消费；不入 IPersistencePorts，组合根按需取）
    void Ban(const std::string& name, std::uint32_t ttlSec);
    bool IsBanned(const std::string& name);

    // 配置版本登记（对局落库/战报盖章用——main 在装配与 ConfigChanged 时投喂）
    void SetConfigVersion(std::string v);

    // full 启动回放：WAL 里 A 行→审计、E 行→战绩，全成后截断（§5.3 补写）
    std::size_t ReplayWal();

    std::string HealthLine() const;        // §5.7 横幅：MySQL ✓ Mongo ✗ Redis ✓
    std::uint64_t Dropped() const { return dropped_.load(); }
    std::uint64_t Spilled() const { return spilled_.load(); }

private:
    enum class ItemKind { Events, KillMarker };
    struct StatsItem {
        ItemKind kind = ItemKind::Events;
        sightline::application::Durability cls = sightline::application::Durability::BestEffort;
        std::vector<sightline::application::MatchEvent> evs;
        std::uint32_t pid = 0;             // KillMarker 用
    };
    // 单局缓冲（ep04 单局架构边界——MatchStart 开档 → Ended flush）
    struct MatchCtx {
        bool active = false;
        std::string matchId;
        std::string configVersion;
        std::uint64_t startedAtUtcMs = 0;
        std::uint64_t killSeq = 0;         // 同秒多局去重（C3：UTC 戳+会话内序号）
        struct RE { std::uint32_t pid = 0; std::string name; int spawn = 0; };
        std::vector<RE> roster;                               // pid → name + 出生点序（MatchStart 带）
        std::vector<WalMatchResult::Row> scores;                     // Ended 时由事件填
        std::vector<WarReport::KillE> timeline;      // 时间线（Ended 时进战报）
        std::uint64_t lastKillOffsetMs = 0;
    };

    void WorkerLoop();
    void HandleEvents(const std::vector<sightline::application::MatchEvent>& evs); // worker 侧投影
    static std::string MakeMatchId(std::uint64_t utcMs, std::uint64_t seq);
    static std::uint64_t UtcNowMs_();                        // 墙钟（审计/开档·与 steady 分立）

    RealPersistenceConfig cfg_;
    MysqlStore mysql_;
    RedisStore redis_;
    MongoStore mongo_;
    AuditWal wal_;

    // 双队列
    mutable std::mutex auditMx_;
    std::deque<sightline::application::AuditRecord> auditQ_;      // 无损
    mutable std::mutex statsMx_;
    std::deque<StatsItem> statsQ_;
    std::size_t statsDepth_ = 0;                                  // 事件计数（容量判定）
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> rebuildReq_{false};

    // 健康位（worker 写 · State() 读）
    std::atomic<bool> healthMysql_{false};
    std::atomic<bool> healthMongo_{false};
    std::atomic<bool> healthRedis_{false};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> spilled_{0};

    MatchCtx ctx_;                         // worker 独占（只在 worker 线程读写）
    std::string configVersion_;            // main 写（装配/ConfigChanged）·worker 读（盖章）
    mutable std::mutex verMx_;
};

} // namespace adapters
