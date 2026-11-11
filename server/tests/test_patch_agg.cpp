// test_patch_agg.cpp —— ep09 步骤 4：PatchAggregator 两阶段收口单测（G5·ADR-13·纯逻辑零 IO）
// 断言面：①全成功（N/N 回执→成功 Notice·Idle）②任一失败→二阶段回退广播（LastGoodSource 重发）
//         ③回退收口→失败通知落定 ④500ms 超时=未回执视同失败→回退 ⑤收口中新 patch 拒收
//         ⑥空通道拒收 ⑦野回执（Idle 期）忽略
#include "application/patch_aggregator.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using sightline::application::PatchAggregator;
static int g_pass = 0;
#define CHECK(cond) do { if (cond) { ++g_pass; } else { \
    std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); std::exit(1); } } while (0)

namespace {
// 可编程 fake：记录广播（idx/name/source）与通知——LastGoodSource 返回固定恢复点
struct FakeSink final : PatchAggregator::Sink {
    struct Bc { std::size_t idx; std::string name, source; };
    std::vector<Bc> broadcasts;
    std::vector<std::string> notices;
    std::size_t shards = 2;
    const std::string goodSource = "old-damage-v1";

    void BroadcastPatch(std::size_t idx, const std::string& name,
                        const std::string& source) override {
        broadcasts.push_back({idx, name, source});
    }
    void Notice(const std::string& msg) override { notices.push_back(msg); }
    std::string LastGoodSource(const std::string&) override { return goodSource; }
    void AuditPatch(const std::string& name, bool ok, const std::string& detail,
                    std::uint64_t atUtcMs) override { audits.push_back({name, ok, detail}); }
    struct Au { std::string name; bool ok; std::string detail; };
    std::vector<Au> audits;
    std::size_t ShardCount() const override { return shards; }
};
} // namespace

int main() {
    // ── 断言 1：全成功——Broadcasting 收齐 N 回执→成功 Notice→Idle ──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        CHECK(agg.Begin("damage", 1000));
        CHECK(sink.broadcasts.size() == 2);
        CHECK(sink.broadcasts[0].source.empty());              // 首广播 source 空=VM 自读
        CHECK(agg.phase() == PatchAggregator::Phase::Broadcasting);
        agg.OnPatchResult(true, 1010);
        agg.OnPatchResult(true, 1020);
        CHECK(agg.phase() == PatchAggregator::Phase::Idle);
        CHECK(sink.notices.size() == 1 && sink.notices[0].find("成功") != std::string::npos);
        CHECK(agg.FaultCount() == 0);
    }

    // ── 断言 2：任一失败→二阶段回退（重发 LastGoodSource 到全部 shard）→回退收口 ──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        CHECK(agg.Begin("damage", 1000));
        agg.OnPatchResult(true, 1010);                        // shard0 成功（已换引用）
        agg.OnPatchResult(false, 1020);                       // shard1 失败（内部未换）
        CHECK(agg.phase() == PatchAggregator::Phase::Reverting);
        CHECK(sink.broadcasts.size() == 4);                    // 首广 2+回退 2
        CHECK(sink.broadcasts[2].source == "old-damage-v1");   // 回退源=LastGoodSource
        CHECK(sink.broadcasts[3].source == "old-damage-v1");
        CHECK(sink.notices.back().find("回退") != std::string::npos);
        agg.OnPatchResult(true, 1030);                        // 回退回执×2
        agg.OnPatchResult(true, 1040);
        CHECK(agg.phase() == PatchAggregator::Phase::Idle);    // 回退收口落定
        CHECK(sink.notices.back().find("已回退") != std::string::npos);
    }

    // ── 断言 3：500ms 超时——未回执视同失败→回退广播覆盖 ──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        CHECK(agg.Begin("damage", 1000));
        agg.OnFrameTick(1200);                                 // 200ms：未超时继续等
        CHECK(agg.phase() == PatchAggregator::Phase::Broadcasting);
        agg.OnFrameTick(1600);                                 // 600ms：超时→回退
        CHECK(agg.phase() == PatchAggregator::Phase::Reverting);
        CHECK(sink.broadcasts.size() == 4);
    }

    // ── 断言 4：收口中新 patch 拒收（幂等防护）──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        CHECK(agg.Begin("damage", 1000));
        const auto bcBefore = sink.broadcasts.size();
        CHECK(!agg.Begin("damage", 1010));                     // 拒收
        CHECK(sink.broadcasts.size() == bcBefore);             // 无新广播
        CHECK(sink.notices.back().find("收口") != std::string::npos);
    }

    // ── 断言 5：空通道（ShardCount=0）拒收 ──
    {
        FakeSink sink;
        sink.shards = 0;
        PatchAggregator agg{sink};
        CHECK(!agg.Begin("damage", 1000));
        CHECK(sink.notices.back().find("无 patch 通道") != std::string::npos);
    }

    // ── 断言 6：野回执（Idle 期迟到）忽略——不崩不误收口 ──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        agg.OnPatchResult(true, 9999);                         // Idle 期野回执
        CHECK(agg.phase() == PatchAggregator::Phase::Idle);
        CHECK(sink.notices.empty());
        // 正常 Begin 后仍可用（野回执不污染状态）
        CHECK(agg.Begin("damage", 10000));
        CHECK(agg.phase() == PatchAggregator::Phase::Broadcasting);
    }

    // ── 断言 7：Reverting 阶段超时兜底——回退广播未齐也强制落定（不悬死）──
    {
        FakeSink sink;
        PatchAggregator agg{sink};
        CHECK(agg.Begin("damage", 1000));
        agg.OnPatchResult(false, 1010);                        // shard0 失败
        agg.OnPatchResult(true, 1015);                         // shard1 成功——pending 归零收口判定
        CHECK(agg.phase() == PatchAggregator::Phase::Reverting);   // 任一失败→二阶段广播
        agg.OnFrameTick(2000);                                 // 回退期 500ms 超时（回执未齐）
        CHECK(agg.phase() == PatchAggregator::Phase::Idle);    // 强制收口落定（通知已发·不悬死）
    }

    std::printf("全部通过：%d 项断言 ✓（ep09 步骤 4 PatchAggregator 两阶段收口·纯逻辑）\n", g_pass);
    return 0;
}
