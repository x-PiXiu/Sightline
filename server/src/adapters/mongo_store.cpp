// 构建守卫：无三库客户端环境（SIGHTLINE_HAS_DB=0）下本文件编译为空 TU——
// 头文件声明零驱动依赖（前置声明），使用点（main 的 full 分支）同受宏门控。
#if defined(SIGHTLINE_HAS_DB) && SIGHTLINE_HAS_DB
// mongo_store.cpp —— ep08 步骤 5：MongoDB 战报 sink（唯一 include mongoc 的翻译单元·方案 B）
#include "adapters/mongo_store.h"

#include "logger/logger.h"

#include <libmongoc-1.0/mongoc/mongoc.h>

#include <mutex>

namespace adapters {

namespace {
std::once_flag g_mongocInit;
void EnsureMongocInit() {
    std::call_once(g_mongocInit, [] { mongoc_init(); });   // 全局一次（进程级）
}
} // namespace

MongoStore::~MongoStore() { Disconnect(); }

bool MongoStore::Connect(const std::string& uri) {
    if (coll_) return true;
    if (uri.empty()) return false;             // 未配 URI = 不连（横幅 ⏸·降级态）
    EnsureMongocInit();
    const std::string& u = uri;            // 空 uri 已在上方拒（默认值由组合根给）
    bson_error_t err;
    mongoc_client_t* cli = mongoc_client_new(u.c_str());
    if (!cli) { lastErr_ = "mongoc_client_new 失败（URI=" + u + "）"; return false; }
    // 2s 超时——挂掉的库不能拖住 worker（§5.5 零中断）
    mongoc_client_set_appname(cli, "sightline");
    mongoc_collection_t* coll = mongoc_client_get_collection(cli, "sightline", "war_reports");
    // ping 探活（服务器挂时这里失败而不是首次 insert）
    bson_t* ping = BCON_NEW("ping", BCON_INT32(1));   // BCON_NEW=分配+初始化（裸 bson_t 须 bson_init——
                                                       // 此前漏 init=垃圾 flags→断言 abort·gdb 实锤）
    const bool ok = mongoc_client_command_simple(cli, "admin", ping, nullptr, nullptr, &err);
    bson_destroy(ping);
    if (!ok) {
        lastErr_ = err.message;
        LOG_WARNING("[Mongo] 探活失败 " + u + " — " + lastErr_);
        mongoc_collection_destroy(coll);
        mongoc_client_destroy(cli);
        return false;
    }
    client_ = cli;
    coll_ = coll;
    LOG_INFO("[Mongo] 已连接 " + u + "（sightline.war_reports）");
    return true;
}

void MongoStore::Disconnect() {
    if (coll_) { mongoc_collection_destroy(static_cast<mongoc_collection_t*>(coll_)); coll_ = nullptr; }
    if (client_) { mongoc_client_destroy(static_cast<mongoc_client_t*>(client_)); client_ = nullptr; }
    // mongoc_cleanup 不调：进程级单例，反复 init/cleanup 有已知竞态；进程退出即回收
}

bool MongoStore::InsertWarReport(const WarReport& r) {
    if (!coll_) return false;
    mongoc_collection_t* coll = static_cast<mongoc_collection_t*>(coll_);
    bson_t doc;
    bson_init(&doc);
    bson_append_utf8(&doc, "_id", -1, r.matchId.c_str(), -1);
    bson_append_utf8(&doc, "config_version", -1, r.configVersion.c_str(), -1);
    bson_append_int64(&doc, "started_at", -1, static_cast<int64_t>(r.startedAtUtcMs));
    bson_append_int64(&doc, "ended_at", -1, static_cast<int64_t>(r.endedAtUtcMs));
    bson_append_int64(&doc, "duration_ms", -1, static_cast<int64_t>(r.durationMs));
    bson_append_int32(&doc, "end_reason", -1, r.endReason);

    bson_t arr, child;
    // ⚠ 数组元素必须显式数字键：mongoc C 不把 "" 自动转索引——存成空键→服务端
    // "invalid document for insert: empty key" 拒收（扩展 JSON 看不出·mongosh 对照实锤）
    std::size_t aidx = 0;
    bson_append_array_begin(&doc, "roster", -1, &arr);
    for (const auto& e : r.roster) {
        const std::string k0 = std::to_string(aidx++);
        bson_append_document_begin(&arr, k0.c_str(), -1, &child);
        bson_append_utf8(&child, "name", -1, e.name.c_str(), -1);
        bson_append_int32(&child, "spawn_index", -1, e.spawnIndex);
        bson_append_document_end(&arr, &child);
    }
    bson_append_array_end(&doc, &arr);

    aidx = 0;
    bson_append_array_begin(&doc, "scores", -1, &arr);
    for (const auto& e : r.scores) {
        const std::string k1 = std::to_string(aidx++);
        bson_append_document_begin(&arr, k1.c_str(), -1, &child);
        bson_append_utf8(&child, "name", -1, e.name.c_str(), -1);
        bson_append_int32(&child, "kills", -1, e.kills);
        bson_append_int32(&child, "deaths", -1, e.deaths);
        bson_append_document_end(&arr, &child);
    }
    bson_append_array_end(&doc, &arr);

    aidx = 0;
    bson_append_array_begin(&doc, "kill_timeline", -1, &arr);
    for (const auto& e : r.killTimeline) {
        const std::string k2 = std::to_string(aidx++);
        bson_append_document_begin(&arr, k2.c_str(), -1, &child);
        bson_append_int64(&child, "t_offset_ms", -1, static_cast<int64_t>(e.tOffsetMs));
        bson_append_utf8(&child, "killer", -1, e.killer.c_str(), -1);
        bson_append_utf8(&child, "victim", -1, e.victim.c_str(), -1);
        bson_append_document_end(&arr, &child);
    }
    bson_append_array_end(&doc, &arr);
    bson_append_document_begin(&doc, "extra", -1, &child);   // F1 约定空位（未来模式差异）
    bson_append_document_end(&doc, &child);

    bson_error_t err;
    const bool ok = mongoc_collection_insert_one(coll, &doc, nullptr, nullptr, &err);
    if (!ok) {   // 先诊断再 destroy（此前 destroy 在前=dump 读死文档 UB）
        lastErr_ = err.message;
        LOG_WARNING("[Mongo] 战报写入失败 match=" + r.matchId + " — " + lastErr_);
        {   // 诊断：失败时打整份文档 JSON（定位空键/坏字段——一次性排障后可降 verbose）
            char* js = bson_as_relaxed_extended_json(&doc, nullptr);
            if (js) { LOG_WARNING(std::string("[Mongo] doc=") + js); bson_free(js); }
        }
        bson_destroy(&doc);
        return false;
    }
    bson_destroy(&doc);
    return true;
}

} // namespace adapters

#endif // SIGHTLINE_HAS_DB
