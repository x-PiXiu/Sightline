// test_dualmode_diff.cpp —— ep09 步骤 8（ADR-20 金标准·P1-5）：
// Serial vs PinnedWorker 双模式事件流**按房分片子流逐字节对拍**——并发正确性的可测定义。
// 口径（§6.1 修正）：比较**输入驱动事件**（MatchStart/EntityLeave/FireResult/Kill）按 roomId
//   分组逐字节一致；时间驱动事件（Snapshot/Respawn/Ended——数量/时刻随线程调度）不参与
//   （Canon 返回空=过滤）。配置 respawnMs=600000/killLimit=100 保证脚本窗口内不触发
//   时间驱动状态迁移（重生/终局），输入驱动流完全确定。
// 脚本：双房开局→双实体激活→4 连发击杀（25×4=100=满血）→离场——含 Must 类（Kill）路径。
#include "adapters/serial_dispatcher.h"
#include "adapters/worker_pool.h"
#include <chrono>
#include <cstdio>
#include <iostream>
#include <map>
#include <thread>

using namespace sightline::match;
using sightline::lobby::LobbyService;
using adapters::BotDriver;
using adapters::SerialDispatcher;
using adapters::WorkerPool;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {
// —— 可编程定位表（shared：构造后测试侧改表仍同步——模拟真实快照的原子换指针）——
using Table = std::shared_ptr<std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>;
Table MakeTable() {
    return std::make_shared<std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>();
}
bool LocateFrom(Table t, std::uint32_t pid, std::uint32_t& rid, std::uint32_t& ep) {
    const auto it = t->find(pid);
    if (it == t->end()) return false;
    rid = it->second.first; ep = it->second.second;
    return true;
}

// —— Canon：事件→字节串（输入驱动类型才非空；时间驱动返回空=过滤）——
std::string Canon(const MatchEvent& e) {
    const std::string ver = "|v=" + e.configVersion;      // G4：版本随事件（对拍顺带验证）
    switch (e.type) {
    case MatchEvent::Type::MatchStart: {
        std::string r = "MS|" + std::to_string(e.roomId);
        for (const auto& m : e.roster)
            r += "|" + std::to_string(m.playerId) + ":" + m.name +
                 ":" + std::to_string(m.spawnIndex);
        return r + ver;
    }
    case MatchEvent::Type::EntityLeave:
        return "EL|" + std::to_string(e.roomId) + "|" +
               std::to_string(e.leavePlayerId) + ver;
    case MatchEvent::Type::FireResult: {
        const auto& f = e.fire;
        return "FR|" + std::to_string(e.roomId) +
               "|" + std::to_string(f.shooterPid) +
               "|" + (f.bHit ? "1" : "0") +
               "|" + (f.bAccepted ? "1" : "0") +
               "|" + std::to_string(f.gate) +
               "|" + std::to_string(f.victimPid) +
               "|" + std::to_string(f.victimHp) +
               "|" + std::to_string(f.reloadRemainingMs) + ver;
    }
    case MatchEvent::Type::Kill:
        return "K|" + std::to_string(e.roomId) +
               "|" + std::to_string(e.killVictim) +
               "|" + std::to_string(e.killKiller) +
               "|" + std::to_string(e.respawnDelayMs) + ver;
    default:
        return {};                                   // 时间驱动（Snapshot/Respawn/Ended）过滤
    }
}

struct Collector {
    std::map<std::uint32_t, std::vector<std::string>> byRoom;   // 按房分片子流（ADR-20）
    bool sawLeave = false;
    void Add(const std::vector<MatchEvent>& evs) {
        for (const auto& e : evs) {
            if (e.type == MatchEvent::Type::EntityLeave) sawLeave = true;
            auto c = Canon(e);
            if (!c.empty()) byRoom[e.roomId].push_back(std::move(c));
        }
    }
};

// —— 同一脚本：两 rig 各跑一遍（输入序列/时间基准结构一致）——
template <class Disp>
void RunScript(MatchShards& shards, Disp& disp, LobbyService& lobby,
               Table table, Collector& c) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    // 建两房（顺序一致 → roomId 两 rig 相同）
    (void)lobby.Login(801, "ha");
    const auto crA = lobby.CreateRoom(801, "roomA");
    CHECK(!crA.empty() && crA[0].type == sightline::lobby::EvType::CreateOk);
    const std::uint32_t roomA = crA[0].roomId;
    (void)lobby.Login(802, "hb");
    const auto crB = lobby.CreateRoom(802, "roomB");
    CHECK(!crB.empty() && crB[0].type == sightline::lobby::EvType::CreateOk);
    const std::uint32_t roomB = crB[0].roomId;
    (*table)[100] = {roomA, 1};                          // 名册决定 session 实体归属；locate 只管路由
    (*table)[200] = {roomA, 1};                          // ⚠ 同房才能交战（跨房隔离=正确语义——
    (*table)[300] = {roomB, 1};                          //   不同房实体互不出现在对方候选集）

    // 开局（Serial 同步返回事件；Worker 经 cmdQ 异步——统一由轮询收）
    c.Add(shards.StartMatchVia(roomA, {{100, "p100", 0}, {200, "p200", 0}}));   // 交战房：双人
    c.Add(shards.StartMatchVia(roomB, {{300, "p300", 0}}));                     // 隔离房：单人

    // 激活：双实体首报（空 geometry → 无 rebase 门 → 首报即真值）
    MatchInput m1; m1.kind = MatchInput::Kind::Move;
    m1.x = 100.f; m1.y = 100.f; m1.z = 90.f; m1.yaw = 0.f; m1.atMs = now; m1.seq = 1;
    CHECK(shards.Route(100, std::move(m1)));
    MatchInput m2; m2.kind = MatchInput::Kind::Move;
    m2.x = 400.f; m2.y = 100.f; m2.z = 90.f; m2.atMs = now; m2.seq = 1;
    CHECK(shards.Route(200, std::move(m2)));
    // ⚠ 首报不写历史环（UpdateEntity else 分支才 push）——victim 须第二拍上报才可被
    //   回溯采样（SampleHistoryAt histCount=0 → nullopt → 无候选 → 永远 miss·实锤）
    m2.seq = 2;
    CHECK(shards.Route(200, std::move(m2)));

    // 4 连发击杀（射速 95ms 容差 → 间隔 100ms；25×4=100 满血见底）
    for (int i = 0; i < 4; ++i) {
        MatchInput f; f.kind = MatchInput::Kind::Fire;
        f.atMs = now + static_cast<std::uint64_t>(i) * 100;
        CHECK(shards.Route(100, std::move(f)));
        c.Add(disp.OnFrameTick(now + static_cast<std::uint64_t>(i) * 100));  // 推进+收集
    }

    // 离场（EntityLeave=完成信号）
    MatchInput r; r.kind = MatchInput::Kind::RemovePlayer;
    CHECK(shards.Route(100, std::move(r)));

    // 完成信号：本房 EL + 他房 MS 都到齐——⚠ 跨 shard 事件无全局序（两 worker 独立产，
    // EL 先于他房 MS 到达是合法调度），只等本房会漏收他房迟到事件（实锤）
    for (int i = 0; i < 400 && !(c.sawLeave && c.byRoom.size() >= 2); ++i) {
        c.Add(disp.OnFrameTick(now));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(c.sawLeave);                                 // 本房末事件必达（两模式同）
    CHECK(c.byRoom.size() >= 2);                       // 他房事件必达（跨 shard 合流）
}

std::string FirstDiff(const std::map<std::uint32_t, std::vector<std::string>>& a,
                      const std::map<std::uint32_t, std::vector<std::string>>& b) {
    for (const auto& [room, sa] : a) {
        const auto it = b.find(room);
        if (it == b.end()) return "room " + std::to_string(room) + " 缺失于 Worker 流";
        const auto& sb = it->second;
        if (sa.size() != sb.size())
            return "room " + std::to_string(room) + " 长度 " +
                   std::to_string(sa.size()) + " vs " + std::to_string(sb.size()) +
                   " | serial[0]=" + (sa.empty() ? "-" : sa[0]) +
                   " worker[0]=" + (sb.empty() ? "-" : sb[0]);
        for (std::size_t i = 0; i < sa.size(); ++i)
            if (sa[i] != sb[i])
                return "room " + std::to_string(room) + " #" + std::to_string(i) +
                       "\n  serial: " + sa[i] + "\n  worker: " + sb[i];
    }
    for (const auto& [room, sb] : b)
        if (!a.count(room)) return "room " + std::to_string(room) + " 缺失于 Serial 流";
    return {};
}
} // namespace

int main() {
    const auto makeCfg = [] {
        GameConfig c;
        c.weapons = {kDefaultWeaponParams, WeaponParams{}, WeaponParams{}, WeaponParams{}};
        c.weaponCount = 1;
        c.maxHp = 100;
        c.deathmatch.respawnMs = 60000;                 // ⚠ uint16 界（65535）——600000 会截断=10176
        c.deathmatch.killLimit = 100;                   // 不终局（Ended 属时间驱动·过滤）
        c.version = "v-diff";                           // G4：版本随事件进对拍
        return c;
    };
    const auto deps = [&](Table t) {
        ShardDeps d;
        d.currentConfig = makeCfg;
        d.locate = [t](std::uint32_t pid, std::uint32_t& rid, std::uint32_t& ep) {
            return LocateFrom(t, pid, rid, ep);
        };
        return d;
    };

    // ── Rig A：Serial ──
    Collector ca;
    {
        auto table = MakeTable();
        MatchShards shards{deps(table), 2, MatchShards::Mode::Serial};
        BotDriver bot{2};
        LobbyService lobby;
        SerialDispatcher disp{shards, bot, lobby};
        RunScript(shards, disp, lobby, table, ca);
    }

    // ── Rig B：PinnedWorker（真线程）──
    Collector cb;
    {
        auto table = MakeTable();
        MatchShards shards{deps(table), 2, MatchShards::Mode::Worker};
        BotDriver bot{2};
        LobbyService lobby;
        WorkerPool pool{shards, bot, lobby, 2};
        pool.Start();
        RunScript(shards, pool, lobby, table, cb);
        CHECK(pool.FaultCount() == 0);                  // G6：零 fault
        pool.Stop();                                    // G11 四段（幂等）
        pool.Stop();
    }

    // ── 金标准：按房分片子流逐字节一致（ADR-20）──
    CHECK(ca.byRoom.size() == 2);                       // 两房均有输入驱动事件
    std::size_t total = 0;
    for (const auto& [room, v] : ca.byRoom) total += v.size();
    CHECK(total >= 8);                                  // 2×MS + 4×FR + K + EL ≥ 8
    const auto diff = FirstDiff(ca.byRoom, cb.byRoom);
    if (!diff.empty()) {
        std::fprintf(stderr, "DIFF: %s\n", diff.c_str());
        CHECK(diff.empty());                            // 逐字节不一致=失败（含诊断）
    }

    std::printf("全部通过：%d 项断言 ✓（ep09 ADR-20 双模式对拍·按房子流逐字节一致·%zu 事件）\n",
                g_pass, total);
    return 0;
}
