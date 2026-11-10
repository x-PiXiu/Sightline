// test_shards.cpp —— ep09 步骤 2：多房分桶容器 + Mailbox（G1/G3/E8 契约断言）
// 断言面：路由投递→DrainAndApply 生效 / Move newest-wins / epoch 在途判旧 / E8 终局输入全丢 /
//         dataQ 背压计数 / Fire 事件合流 / RemovePlayer FIFO / 控制面通路
// 全部零网络零库（纯用例层——整洁架构：本测试=用例可直接实例化的体检报告）
#include "match/match_shards.h"
#include <cassert>
#include <cstdio>
#include <cmath>
#include <map>

using namespace sightline::match;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {
// 快照测试替身：pid→(roomId, epoch) 可编程（G1 判旧/路由路径的确定性驱动）
struct FakeLocator {
    // shared 表（引用语义）——容器构造拷贝本仿函数后，测试侧改表仍同步（模拟真实快照的原子换指针）
    std::shared_ptr<std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>> table =
        std::make_shared<std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>>>();
    bool operator()(std::uint32_t pid, std::uint32_t& roomId, std::uint32_t& epoch) const {
        const auto it = table->find(pid);
        if (it == table->end()) return false;
        roomId = it->second.first;
        epoch = it->second.second;
        return true;
    }
};

MatchInput MakeMove(std::uint32_t pid, float x, float y) {
    MatchInput in;
    in.kind = MatchInput::Kind::Move;
    in.x = x; in.y = y; in.z = 90.f; in.atMs = 1000;
    return in;
}
} // namespace

int main() {
    // ── 断言 1：Route→DrainAndApply→session 位置生效 ──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 2};
        auto& s = shards.OpenRoom(7);
        s.StartMatch(7, {{10, "p10", 0}, {11, "p11", 0}});
        CHECK(shards.Route(10, MakeMove(10, 100.f, 200.f)));
        const auto evs = shards.DrainAndApply(1033);
        CHECK(evs.empty());                                    // Move 无事件产出（ADR-24）
        const auto* e = s.EntityById(10);
        CHECK(e && e->bHasReport);
        CHECK(std::fabs(e->x - 100.f) < 0.01f && std::fabs(e->y - 200.f) < 0.01f);
    }

    // ── 断言 2：Move newest-wins——同 pid 未消费两连发 → 取新 ──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        auto& s = shards.OpenRoom(7);
        s.StartMatch(7, {{10, "p10", 0}});
        (void)shards.Route(10, MakeMove(10, 1.f, 1.f));
        (void)shards.Route(10, MakeMove(10, 500.f, 600.f));    // 覆盖旧值
        (void)shards.DrainAndApply(1033);
        const auto* e = s.EntityById(10);
        CHECK(e && std::fabs(e->x - 500.f) < 0.01f && std::fabs(e->y - 600.f) < 0.01f);
    }

    // ── 断言 3：epoch 在途判旧——快照换房后旧输入丢（G1 幽灵输入根除）──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        auto& s = shards.OpenRoom(7);
        s.StartMatch(7, {{10, "p10", 0}});
        CHECK(shards.Route(10, MakeMove(10, 100.f, 100.f)));   // 投递（快照 (7,1)）
        (*loc.table)[10] = {8, 2};                                 // 玩家已换房（快照变更）
        (void)shards.DrainAndApply(1033);                       // drain：Route 时填的 (7,1) ≠ 最新 (8,2) → 丢
        CHECK(!s.EntityById(10)->bHasReport);                   // 幽灵位置未写入
        CHECK(shards.ActiveRooms() == 1);
    }

    // ── 断言 4：E8——IsEnded 的 session 输入全丢（killLimit=1 一发杀→自然终局·结算背景期）──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        auto& s = shards.OpenRoom(7);
        GameConfig c;                                          // 一发杀 + 一杀终局（其余默认=ep07 常量）
        c.weapons[0].damage = 100;
        c.deathmatch.killLimit = 1;
        s.SetConfig(c);
        s.StartMatch(7, {{10, "p10", 0}, {11, "p11", 0}});
        (*loc.table)[11] = {7, 1};                             // 受害者路由登记（漏=Route 丢投递）
        auto m10 = MakeMove(10, 100.f, 100.f); m10.yaw = 0.f; (void)shards.Route(10, m10);
        auto m11 = MakeMove(11, 400.f, 100.f); (void)shards.Route(11, m11);   // 11 在 10 正东
        (void)shards.DrainAndApply(1033);
        MatchInput fire;
        fire.kind = MatchInput::Kind::Fire;
        fire.atMs = 1100;
        CHECK(shards.Route(10, std::move(fire)));
        const auto evs = shards.DrainAndApply(1100);           // 一发杀 11（damage=100·hp→0）
        bool sawKill = false;
        for (const auto& e : evs) {
            if (e.type == MatchEvent::Type::FireResult) {
                const auto& dg = e.fireDiag;
                std::fprintf(stderr, "[diag] gate=%u acc=%d nearPid=%u perp=%.1f near=(%.0f,%.0f,%.0f) inv=%d rayO=(%.0f,%.0f,%.0f) rayD=(%.2f,%.2f,%.2f)\n",
                             (unsigned)e.fire.gate, e.fire.bAccepted ? 1 : 0, dg.nearPid,
                             dg.nearPerp, dg.nearX, dg.nearY, dg.nearZ, dg.nearInvuln ? 1 : 0,
                             dg.oX, dg.oY, dg.oZ, dg.dX, dg.dY, dg.dZ);
            }
if (e.type == MatchEvent::Type::Kill && e.killKiller == 10 && e.killVictim == 11)
                sawKill = true;
        }
        CHECK(sawKill);                                        // 击杀事件从 Mailbox 通路产出
        (void)s.Tick(1200);                                    // 结束判定归 Tick（ADR-1 节拍内）
        CHECK(s.IsEnded());
        CHECK(shards.Route(10, MakeMove(10, 999.f, 999.f)));   // 背景期仍可投递（管道活着）
        (void)shards.DrainAndApply(1200);
        const auto* e = s.EntityById(10);
        CHECK(e && std::fabs(e->x - 999.f) > 1.f);             // E8：位置停在终局前值（输入全丢）
    }

    // ── 断言 5：dataQ 背压——不可丢类（Fire）满则拒+计数（G8）──
    //   注：Move newest-wins 下永不满队（同 pid 覆盖）——背压上界只对 FIFO 类生效
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        (void)shards.OpenRoom(7);
        std::size_t accepted = 0;
        for (int i = 0; i < 5000; ++i) {                       // 超 kDataQCap=4096
            MatchInput in;
            in.kind = MatchInput::Kind::Fire;
            in.atMs = static_cast<std::uint64_t>(1000 + i);
            if (shards.Route(10, std::move(in))) ++accepted;
        }
        CHECK(accepted == MatchShards::kDataQCap);             // 恰满即拒
        CHECK(shards.TotalDropped() == 5000 - MatchShards::kDataQCap);   // 丢弃计数聚合
    }

    // ── 断言 6：Fire 输入→事件合流（21 号载荷源走 drain 通路）──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        auto& s = shards.OpenRoom(7);
        s.StartMatch(7, {{10, "p10", 0}, {11, "p11", 0}});
        // 两实体都激活（首报），共一拍——10 对 11 开火
        auto first = MakeMove(10, 100.f, 100.f); (void)shards.Route(10, first);
        auto first2 = MakeMove(11, 150.f, 100.f); (void)shards.Route(11, first2);
        (void)shards.DrainAndApply(1033);
        MatchInput fire;
        fire.kind = MatchInput::Kind::Fire;
        fire.atMs = 1066;
        CHECK(shards.Route(10, std::move(fire)));
        const auto evs = shards.DrainAndApply(1066);
        bool sawFire = false;
        for (const auto& e : evs)
            if (e.type == MatchEvent::Type::FireResult) { sawFire = true; CHECK(e.fire.shooterPid == 10); }
        CHECK(sawFire);                                        // 判定事件从 Mailbox 通路产出（B1 三源统一）
    }

    // ── 断言 7：RemovePlayer FIFO——EntityLeave 事件产出 ──
    {
        FakeLocator loc;
        (*loc.table)[10] = {7, 1};
        MatchShards shards{ShardDeps{.locate = loc}, 1};
        auto& s = shards.OpenRoom(7);
        s.StartMatch(7, {{10, "p10", 0}});
        MatchInput in;
        in.kind = MatchInput::Kind::RemovePlayer;
        CHECK(shards.Route(10, std::move(in)));
        const auto evs = shards.DrainAndApply(1033);
        bool sawLeave = false;
        for (const auto& e : evs)
            if (e.type == MatchEvent::Type::EntityLeave && e.leavePlayerId == 10) sawLeave = true;
        CHECK(sawLeave);
    }

    // ── 断言 8：控制面通路——RouteCommand 入队不炸（v1 Patch 占位·步骤 4 接聚合）──
    {
        MatchShards shards{ShardDeps{}, 1};
        ShardCommand c;
        c.kind = ShardCommand::Kind::Patch;
        c.roomId = 3;
        CHECK(shards.RouteCommand(std::move(c)));
        (void)shards.DrainAndApply(1033);                      // cmdQ 先于 dataQ（空数据面）
        CHECK(true);
    }

    std::printf("全部通过：%d 项断言 ✓（ep09 步骤 2 多房容器+Mailbox·零 IO）\n", g_pass);
    return 0;
}
