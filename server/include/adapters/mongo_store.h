// mongo_store.h —— ep08 步骤 5：MongoDB 战报 sink（文档库·best-effort）
// 驱动选型（§5.4 实勘）：apt 无 mongocxx → 走 libmongoc C 驱动（设计草案预案 B：
// 「降级 mongoc C 驱动裸写 BSON，多 100 行」——libmongoc-dev 1.26 本机已装）。
// 连接串 SIGHTLINE_MONGO_URI（默认 mongodb://127.0.0.1:27017）；挂=丢弃计数+告警（best-effort）。
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace adapters {

// 战报文档（§5.6 schema 的 C++ 侧 POD——BSON 构造在 .cpp）
struct WarReport {
    std::string matchId;                    // _id：UTC 戳+会话序号（C3）
    std::string configVersion;
    std::uint64_t startedAtUtcMs = 0;
    std::uint64_t endedAtUtcMs = 0;
    std::uint64_t durationMs = 0;
    int endReason = 0;
    struct RosterE { std::string name; int spawnIndex = 0; };
    struct ScoreE  { std::string name; int kills = 0; int deaths = 0; };
    struct KillE   { std::uint64_t tOffsetMs = 0; std::string killer, victim; };
    std::vector<RosterE> roster;
    std::vector<ScoreE>  scores;
    std::vector<KillE>   killTimeline;
};

class MongoStore {
public:
    MongoStore() = default;
    ~MongoStore();

    MongoStore(const MongoStore&) = delete;
    MongoStore& operator=(const MongoStore&) = delete;

    bool Connect(const std::string& uri);   // 空 uri=不连（返回 false·横幅 ⏸）
    void Disconnect();
    bool Connected() const { return coll_ != nullptr; }

    bool InsertWarReport(const WarReport& r);   // 文档级原子写（整份一次 insert）

    const std::string& LastError() const { return lastErr_; }

private:
    void* client_ = nullptr;      // mongoc_client_t*（头零 mongoc 依赖——void* 过界）
    void* coll_ = nullptr;        // mongoc_collection_t*（sightline.war_reports）
    std::string lastErr_;
};

} // namespace adapters
