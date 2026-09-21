// application/room_service.h —— 房间用例：匹配进房、开战、移动广播、
// 服务器权威开火判定、重生调度、胜负广播、离场处理。
// 规则全部委托 domain::Room，本类只做编排（取房间→调实体→翻译结果为事件→选收件人广播）。

#pragma once
#include <chrono>
#include <unordered_map>
#include <string>
#include <vector>
#include <functional>
#include "application/dto.h"
#include "application/ports/i_game_channel.h"
#include "application/ports/i_timer_scheduler.h"
#include "application/ports/i_match_repository.h"
#include "storage/redis_conn.h"
#include "domain/room.h"
#include "domain/player.h"
#include "domain/item.h"

namespace sightline::app {

class RoomService {
public:
    struct Config {
        domain::RoomRules room_rules;    // 每间房的对战规则
        int respawn_delay_ms = 3000;     // 阵亡后重生倒计时
        int rematch_delay_ms = 5000;     // 结算后自动重开倒计时
    };

    RoomService(IGameChannel& channel, ITimerScheduler& timers)
        : RoomService(channel, timers, Config()) {}

    RoomService(IGameChannel& channel, ITimerScheduler& timers, const Config& config)
        : channel_(channel), timers_(timers), config_(config) {}

    /** Lua 热更入口：新开局按新规则（进行中对局不受影响，15 号 01 文档 reload） */
    void updateConfig(const Config& c) { config_ = c; }

    // ---- 进房/匹配：room_id=0 自动匹配，>0 加入指定房间；人齐自动开战 ----
    void handleJoin(PlayerId pid, const std::string& name, std::uint64_t account_id = 0,
                    std::uint32_t room_id = 0) {
        domain::Room* room = nullptr;
        if (room_id != 0)
        {
            auto it = rooms_.find(room_id);
            if (it != rooms_.end() && it->second.canJoin()) room = &it->second;
        }
        if (!room) room = findJoinableRoom();   // 自动匹配：优先加入既有可加入房间
        if (!room) {                            // 没有可加入的才建新房
            rooms_[next_room_id_] = domain::Room(next_room_id_, config_.room_rules);
            room = &rooms_[next_room_id_];
            ++next_room_id_;
        }
        domain::Player p;
        p.id = pid;
        p.name = name;
        p.account_id = account_id;            // D2：战绩落库的身份键（游客=0）
        if (!room->addPlayer(p)) return;      // 满员竞态，忽略（MVP 客户端可重试）

        player_room_[pid] = room->id();
        if (room->tryStart()) {
            startRoundFor(room);
        }
    }

    // ---- 移动：采纳 + 转播给房间其他人；顺带道具惰性检查（重生/拾取，P2）----
    void handleMove(PlayerId pid, const MoveCommand& cmd) {
        domain::Room* room = roomOf(pid);
        if (!room) return;
        if (!room->applyMove(pid, cmd.pos, cmd.yaw, cmd.flags, cmd.aim_pitch)) return;
        channel_.sendToAll(room->othersOf(pid),
                           MoveEvent{pid, cmd.pos, cmd.yaw, cmd.flags, cmd.aim_pitch});

        // 惰性驱动：道具重生检查 + 拾取判定与位置上报同频（10Hz），
        // 不需要独立定时器，也不需要 C2S_Pickup 消息
        const double now_ms = nowMs();
        for (const auto& item : room->tickItems(now_ms)) {
            broadcastItemSpawn(room, item);
        }
        if (auto pickup = room->tryPickup(pid, cmd.pos, now_ms)) {
            channel_.sendToAll(room->playerIds(), ItemTakenEvent{
                pickup->net_id, pickup->picker,
                static_cast<uint8_t>(pickup->type_id), pickup->picker_hp});
        }
    }

    // ---- 开火：domain 权威判定 → 结果广播；击杀触发重生/胜负调度 ----
    // 返回判定结果供适配层记录诊断(application 不接触日志,红线 2)
    domain::Room::FireResult handleFire(PlayerId pid, const FireCommand& cmd) {
        domain::Room* room = roomOf(pid);
        if (!room) return {};
        auto r = room->applyFire(pid, cmd.origin, cmd.dir);
        if (!r.valid) return r;

        // 命中与未中都广播（客户端用 miss 画弹道/弹孔）
        channel_.sendToAll(room->playerIds(),
            HitEvent{r.killer, r.victim, r.damage, r.victim_hp, r.victim_dead});

        if (r.victim_dead) {
            PlayerId victim = r.victim;
            scheduleRespawn(room->id(), victim);
        }
        if (r.game_over) {
            channel_.sendToAll(room->playerIds(), GameOverEvent{r.winner});
            scheduleRematch(room->id());
        }
        return r;
    }

    // 结算 N 秒后自动重开：玩家保留、状态复位、人够即再战（协议复用 RoomStart，零改动）
    void scheduleRematch(domain::RoomId room_id) {
        timers_.runAfter(config_.rematch_delay_ms, [this, room_id] {
            auto it = rooms_.find(room_id);
            if (it == rooms_.end()) return;         // 房间已回收（全走光）
            auto& room = it->second;
            if (room.rematch()) {
                startRoundFor(&room);
            }
            // 人不够（有玩家离开）：留在 Waiting 等新加入者补位
        });
    }

    // ---- 玩家彻底离开（断线/被踢），由 SessionService 回调进来 ----
    void handlePlayerGone(PlayerId pid) {
        domain::Room* room = roomOf(pid);
        player_room_.erase(pid);
        if (!room) return;
        auto out = room->removePlayer(pid);
        channel_.sendToAll(room->playerIds(), PlayerLeftEvent{pid});
        if (out.game_over) {
            channel_.sendToAll(room->playerIds(), GameOverEvent{out.winner});
            scheduleRematch(room->id());
        }
        if (room->playerCount() == 0) rooms_.erase(room->id());   // 空房即回收
    }

    // ---- 供查询 ----
    size_t roomCount() const { return rooms_.size(); }

    void attachMatchPersistence(std::shared_ptr<IMatchRepository> repo,
                                std::function<void(std::function<void()>)> postToStorage,
                                std::function<std::uint64_t(PlayerId)> accountLookup)
    {
        match_repo_  = std::move(repo);
        post_to_storage_ = std::move(postToStorage);
        account_lookup_ = std::move(accountLookup);
    }

    // ---- D4 大厅：房间列表 / 指定建房 / 战绩查询 ----

    struct RoomBrief {
        domain::RoomId room_id = 0;
        std::uint8_t   mode  = 1;
        std::uint32_t  cur_players = 0;
        std::uint32_t  max_players = 0;
        // GM 面板扩展字段（协议编码不读它们——线上包不变，加尾不改头）
        std::uint8_t   state = 0;          // domain::RoomState 枚举值
        std::uint32_t  elapsed_sec = 0;
    };

    std::vector<RoomBrief> listRooms() const {
        std::vector<RoomBrief> out;
        for (const auto& [id, room] : rooms_)
            out.push_back({id, 1, static_cast<std::uint32_t>(room.playerCount()),
                           static_cast<std::uint32_t>(room.rules().max_players),
                           static_cast<std::uint8_t>(room.state()),
                           static_cast<std::uint32_t>(room.elapsedSec())});
        return out;
    }

    /** GM 面板：玩家所在房间号（不在任何房间=0；主 loop 线程调用） */
    domain::RoomId roomIdOf(PlayerId pid) const {
        auto it = player_room_.find(pid);
        return it == player_room_.end() ? 0 : it->second;
    }

    domain::RoomId createRoom() {
        domain::Room& r = rooms_[next_room_id_] = domain::Room(next_room_id_, config_.room_rules);
        return next_room_id_++;
    }

    std::vector<MatchRecordRow> queryRecords(std::uint64_t account_id, int limit) {
        if (!match_repo_) return {};
        return match_repo_->queryByAccount(account_id, limit);
    }

    /** D4 排行榜：注入 Redis 连接（结算后 ZINCRBY 写入 lb:kills） */
    void attachRanking(std::shared_ptr<storage::RedisConnection> redis) { ranking_redis_ = std::move(redis); }

    /** 排行榜 TopN 查询 */
    std::vector<std::pair<std::uint64_t, std::uint16_t>> topKills(int topN = 10)
    {
        std::vector<std::pair<std::uint64_t, std::uint16_t>> out;
        if (!ranking_redis_) return out;
        auto r = ranking_redis_->zrevrange("lb:kills", 0, topN - 1);
        for (const auto& [member, score] : r)
            out.emplace_back(std::strtoull(member.c_str(), nullptr, 10),
                             static_cast<std::uint16_t>(score));
        return out;
    }

    // ---- 对局结算（D3 战绩）：MatchEnd 广播 + 异步落库 ----

    /** 判胜统一出口：GameOver 广播 + MatchEnd 广播 + 战绩落库（经存储线程，可空降级）。
     *  winner_pid = 0 表示无胜者（全员离开）。 */
    void settleAndBroadcast(domain::Room* room, PlayerId winner_pid) {
        channel_.sendToAll(room->playerIds(), GameOverEvent{winner_pid});

        const std::uint64_t seq = ++match_seq_;
        MatchEndEvent ev;
        ev.match_seq          = seq;
        ev.winner_account_id  = winner_pid ? account_lookup_(winner_pid) : 0;
        ev.duration_sec       = static_cast<std::uint16_t>(room->elapsedSec());
        for (const auto& row : room->scoreBoard())
            ev.scores.push_back(MatchScoreRow{ row.account_id,
                static_cast<std::uint16_t>(row.kills),
                static_cast<std::uint16_t>(row.deaths) });
        channel_.sendToAll(room->playerIds(), ev);

        if (match_repo_ && !ev.scores.empty())
        {
            MatchRecordDb rec;
            rec.match_seq          = seq;
            rec.mode               = 1;   // DM（模式参数化 D4 后由房间携带）
            rec.winner_account_id  = ev.winner_account_id;
            rec.duration_sec       = ev.duration_sec;
            rec.players.reserve(ev.scores.size());
            for (const auto& s : ev.scores)
                rec.players.push_back({s.account_id, s.kills, s.deaths});
            post_to_storage_([repo = match_repo_, rec] { repo->save(rec); });
        }
        scheduleRematch(room->id());
    }

private:
    domain::Room* roomOf(PlayerId pid) {
        auto it = player_room_.find(pid);
        if (it == player_room_.end()) return nullptr;
        auto rit = rooms_.find(it->second);
        return rit == rooms_.end() ? nullptr : &rit->second;
    }

    // 一局的统一入口：广播开局 + 道具布点（开局与重开共用，保证每局道具状态全新）
    void startRoundFor(domain::Room* room) {
        room->spawnItems();
        channel_.sendToAll(room->playerIds(), RoomStartEvent{room->playerIds()});
        for (const auto& item : room->items()) {
            broadcastItemSpawn(room, item);
        }
    }

    void broadcastItemSpawn(domain::Room* room, const domain::Item& item) {
        channel_.sendToAll(room->playerIds(), ItemSpawnEvent{
            item.net_id, static_cast<uint8_t>(item.type_id), item.pos});
    }

    // 单调钟毫秒——道具冷却的时基（application 管时间，domain 只接收 now）
    static double nowMs() {
        return std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    domain::Room* findJoinableRoom() {
        for (auto& [id, room] : rooms_) {
            if (room.canJoin()) return &room;
        }
        return nullptr;
    }

    void scheduleRespawn(domain::RoomId room_id, PlayerId pid) {
        timers_.runAfter(config_.respawn_delay_ms, [this, room_id, pid] {
            auto it = rooms_.find(room_id);
            if (it == rooms_.end()) return;
            auto& room = it->second;
            if (room.respawn(pid)) {
                const domain::Player* p = room.findPlayer(pid);
                channel_.sendToAll(room.playerIds(),
                                   RespawnEvent{pid, p->position, p->hp});
            }
        });
    }

    // 结算后的房间由"最后一个玩家离开"自然回收（handlePlayerGone 空房即删）

    IGameChannel& channel_;
    ITimerScheduler& timers_;
    Config config_;

    std::unordered_map<domain::RoomId, domain::Room> rooms_;
    std::unordered_map<PlayerId, domain::RoomId> player_room_;
    domain::RoomId next_room_id_ = 1;

    // ---- D3 战绩落库管线（main 经 attachMatchPersistence 注入；可空 = 不落库）----
    std::shared_ptr<IMatchRepository> match_repo_;
    std::function<void(std::function<void()>)> post_to_storage_;
    std::function<std::uint64_t(PlayerId)> account_lookup_;
    std::uint64_t match_seq_ = 0;

    // ---- D4 排行榜（结算后 ZINCRBY 写入 Redis ZSET；查询走 ZREVRANGE）----
    std::shared_ptr<storage::RedisConnection> ranking_redis_;

    /** 结算后写排行榜：每个有 account_id 的玩家 ZINCRBY kills */
    void recordRanking(const std::vector<MatchScoreRow>& scores) {
        if (!ranking_redis_) return;
        for (const auto& s : scores) {
            if (s.account_id)
                ranking_redis_->zincrby("lb:kills", static_cast<double>(s.kills),
                                        std::to_string(s.account_id));
        }
    }
};

} // namespace sightline::app
