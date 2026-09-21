// i_match_repository.h —— 对局战绩仓库端口（服务端 02 文档：第四个端口的诞生）
//
// 诞生依据：战绩需要持久化（MySQL 汇总/明细）——第二实现（内存 fake / MySQL）出现，
// 端口按"需求第二实现时才建"的纪律诞生。Mongo 对局文档走独立端口（D4）。

#pragma once

#include <cstdint>
#include <vector>

namespace sightline::app {

struct MatchPlayerRow
{
    std::uint64_t account_id = 0;   // 0 = 游客（不入库，仅广播）
    std::uint16_t kills  = 0;
    std::uint16_t deaths = 0;
};

struct MatchRecordDb
{
    std::uint64_t db_match_id     = 0;   // save 后由实现回填（MySQL 自增）
    std::uint64_t match_seq       = 0;   // 进程内对局序号（广播用，与 DB 无关）
    std::uint8_t  mode            = 1;   // 1=Deathmatch
    std::uint64_t winner_account_id = 0; // 0 = 无胜者（全员离开）
    std::uint32_t duration_sec    = 0;
    std::vector<MatchPlayerRow> players;
};

struct MatchRecordRow
{
    std::uint64_t match_id = 0;
    std::uint8_t  mode = 1;
    bool          win = false;
    std::uint16_t kills = 0;
    std::uint16_t deaths = 0;
};

class IMatchRepository
{
public:
    virtual ~IMatchRepository() = default;

    /** 落库一局（match_record + match_player + player_stats 累加），返回 DB 自增 match_id */
    virtual std::uint64_t save(const MatchRecordDb& rec) = 0;

    /** 按账号查战绩列表（最近 N 条） */
    virtual std::vector<MatchRecordRow> queryByAccount(std::uint64_t account_id, int limit) = 0;
};

} // namespace sightline::app
