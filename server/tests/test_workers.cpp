// test_workers.cpp —— ep09 步骤 3 刀 B：WorkerPool 并发验收（真线程·TSan L1 靶场）
// 断言面：①cmdQ StartMatch 经 worker 执行→Merge 收 MatchStart（双房跨 shard）
//         ②快照流 30Hz 产出（worker 自驱 Tick）③Mailbox 输入在 worker 生效（Move→位置）
//         ④Stop join 完成+幂等（G11）⑤fault 计数通道存活（G6——正常路径 faultCount=0）
// 本测试=TSan 前置纪律（§6.2 L1）的默认靶场：worker 相关改动必跑本文件。
#include "adapters/worker_pool.h"
#include "adapters/serial_dispatcher.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <map>
#include <thread>

using namespace sightline::match;
using FakeLocatorTable = std::shared_ptr<std::map<std::uint32_t,
                                                  std::pair<std::uint32_t, std::uint32_t>>>;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {
bool WaitForMatchStart(adapters::WorkerPool& pool, MatchShards& shards,
                       std::uint32_t roomIdA, std::uint32_t roomIdB) {
    bool a = false, b = false;                             // 跨轮保持（两房事件可能不同轮到达）
    for (int i = 0; i < 200; ++i) {                        // ≤2s 轮询（worker 30Hz 自驱）
        for (const auto& e : pool.OnFrameTick(0)) {
            if (e.type == MatchEvent::Type::MatchStart) {
                if (e.roomId == roomIdA) a = true;
                if (e.roomId == roomIdB) b = true;
            }
        }
        if (a && b) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::fprintf(stderr, "[diag] timeout rooms=%zu dropped=%llu a=%d b=%d\n",
                 shards.ActiveRooms(), (unsigned long long)shards.TotalDropped(), a, b);
    return false;
}
} // namespace

int main() {
    // ── 断言 1~3：双房跨 shard 并发开局+快照流+Mailbox 输入生效 ──
    {
        FakeLocatorTable table = std::make_shared<std::map<
            std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>();
        // roomId 由 lobby 分配（CreateRoom 后动态回填——不写死）
        std::uint32_t roomIdA = 0, roomIdB = 0;
        MatchShards shards{ShardDeps{.locate = [table](std::uint32_t pid,
                                                       std::uint32_t& rid, std::uint32_t& ep) {
                                          const auto it = table->find(pid);
                                          if (it == table->end()) return false;
                                          rid = it->second.first;
                                          ep = it->second.second;
                                          return true;
                                      }},
                           MatchShards::kDefaultShardCount,
                           MatchShards::Mode::Worker};
        adapters::BotDriver botDriver{shards.shardCount()};
        sightline::lobby::LobbyService lobby;
        adapters::WorkerPool pool{shards, botDriver, lobby, shards.shardCount()};

        pool.Start();                                       // 4 worker（错相 8ms）
        // 真 lobby 建房（自愈的 HasRoom 门需要——房间必须在 lobby 注册，否则 worker 自愈清场）
        (void)lobby.Login(801, "hostA");
        const auto crA = lobby.CreateRoom(801, "roomA");
        CHECK(!crA.empty() && crA[0].type == sightline::lobby::EvType::CreateOk);
        roomIdA = crA[0].roomId;
        (*table)[100] = {roomIdA, 1};
        (void)lobby.Login(802, "hostB");
        const auto crB = lobby.CreateRoom(802, "roomB");
        CHECK(!crB.empty() && crB[0].type == sightline::lobby::EvType::CreateOk);
        roomIdB = crB[0].roomId;
        (*table)[200] = {roomIdB, 1};
        (void)shards.StartMatchVia(roomIdA, {{100, "p100", 0}});
        (void)shards.StartMatchVia(roomIdB, {{200, "p200", 0}});   // 跨 shard——cmdQ 路由
        CHECK(WaitForMatchStart(pool, shards, roomIdA, roomIdB));  // 两房 MatchStart 均从合流出

        // Mailbox：Move 经 Route→worker drain→session 位置生效（跨线程管线端到端）
        //（先激活再验快照——未首报实体不进快照，ep08 语义）
        MatchInput mv;
        mv.kind = MatchInput::Kind::Move;
        mv.x = 120.f; mv.y = 240.f; mv.z = 90.f; mv.atMs = 5000;
        CHECK(shards.Route(100, std::move(mv)));
        bool applied = false;
        for (int i = 0; i < 100 && !applied; ++i) {
            (void)pool.OnFrameTick(0);
            auto* s = shards.Find(roomIdA);
            if (auto* e = s ? s->EntityById(100) : nullptr)
                applied = e->bHasReport &&
                          std::fabs(e->x - 120.f) < 0.01f && std::fabs(e->y - 240.f) < 0.01f;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(applied);

        // 快照流（激活后）：worker 自驱 Tick（30Hz）——Merge 里有 Snapshot 事件
        //（宽限 2s：Merge 时序随线程调度抖动——跨轮累计计数）
        int snapCount = 0;
        for (int i = 0; i < 200 && snapCount == 0; ++i) {
            for (const auto& e : pool.OnFrameTick(0))
                if (e.type == MatchEvent::Type::Snapshot) ++snapCount;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(snapCount > 0);

        CHECK(pool.FaultCount() == 0);                      // 正常路径零 fault（G6 通道在位）
        pool.Stop();                                        // G11：停收→join
        pool.Stop();                                        // 幂等
    }

    std::printf("全部通过：%d 项断言 ✓（ep09 步骤 3 WorkerPool 并发·真线程）\n", g_pass);
    return 0;
}
