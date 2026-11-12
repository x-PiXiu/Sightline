// worker_pool.h —— ep09 步骤 3 刀 B：ITickDispatcher 的 PinnedWorker 实现（企业主流形态）
// 架构（草案 §4.5）：n 线程 × 错相 8ms × 30Hz 节拍——每 worker 独调自己 shard 的
//   ProcessShard（drain cmdQ→dataQ→Tick 本桶房）+ 本桶 bot（E3 直调，事件 PublishToMerge 入合流）；
//   主线程 OnFrameTick=DrainMerge 收产物→编排器 Fanout（按 roomId 分发零改动）。
// 四条铁律（ADR-9/11）：①房哈希固定绑 shard（roomId%n·不迁移）⇒ 房内单线程无锁
//   ②worker 状态私有 ③跨线程只走 Mailbox/outbox（值语义）④共享资产只读。
// G6：loop catch-all → fault 房 AbortMatch+fault 事件（noexcept 纪律·不碰异常后的 session 状态）。
// G11：Stop 六阶段（停收→drain 残余→末拍→join）——顺序反了丢事件，测试锁定（test_workers）。
// ADR-1：钟由 loop 统一给（now 参数化——session 内禁止内联取钟，ep07 双钟教训制度化）。
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "adapters/bot_driver.h"
#include "lobby/lobby.h"
#include "match/match_shards.h"
#include "application/ports.h"

namespace adapters {

class WorkerPool final : public sightline::application::ITickDispatcher {
public:
    static constexpr std::uint64_t kTickPeriodMs = 33;          // 30Hz
    static constexpr std::uint64_t kPhaseShiftMs = 8;           // 错相（压测定档·草案 §4.5）

    WorkerPool(sightline::match::MatchShards& shards,
               BotDriver& bot,
               sightline::lobby::LobbyService& lobby,
               std::uint32_t workerCount)
        : shards_(shards), bot_(bot), lobby_(lobby),
          threads_(workerCount ? workerCount : 1),
          faultCount_{0} {}

    ~WorkerPool() override { Stop(); }

    void Start() override {
        bool expect = false;
        if (!running_.compare_exchange_strong(expect, true)) return;
        for (std::size_t i = 0; i < threads_.size(); ++i)
            threads_[i] = std::thread([this, i] { Loop(i); });
    }

    /** G11 启停序（P1-4 阶段化·草案 §4.9 的 worker 侧四段·每段幂等）：
     *  ① 停收（running=false→workers 退出循环）② join（线程全停）
     *  ③ 残余消费（主线程代跑：drain cmdQ/dataQ+末拍 ProcessShard——join 后单线程安全）
     *  ④ 尾包入合流（调用方随后 DrainMerge 冲走——main 在 persistence Shutdown 前）
     *  顺序反了丢事件——test_workers 断言锁定（§6.3）。 */
    void Stop() override {
        bool expect = true;
        if (!running_.compare_exchange_strong(expect, false)) return;   // ① 停收（幂等）
        for (auto& t : threads_)
            if (t.joinable()) t.join();                                 // ② join
        for (std::size_t i = 0; i < shards_.shardCount(); ++i) {        // ③④ 残余+尾包
            std::vector<sightline::match::MatchEvent> tail;
            shards_.ProcessShard(i, NowMs_(), tail);
            shards_.PublishToMerge(i, std::move(tail));
        }
    }

    /** 主线程每帧：收全部 worker 产物（合流区）——Fanout 由编排器统一（按 roomId 零改动） */
    std::vector<sightline::application::MatchEvent> OnFrameTick(std::uint64_t) override {
        return shards_.DrainMerge();
    }

    std::uint64_t FaultCount() const { return faultCount_.load(std::memory_order_relaxed); }

private:
    void Loop(std::size_t idx) {
        // 错相：worker idx 偏移 idx*8ms——峰谷错开 CPU 平滑；各房仍 30Hz 客户端无感
        auto next = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(idx * kPhaseShiftMs);
        std::uint64_t tickNo = 0;
        while (running_.load(std::memory_order_relaxed)) {
            try {
                const auto nowMs = NowMs_();
                std::vector<sightline::match::MatchEvent> evs;
                shards_.ProcessShard(idx, nowMs, evs);           // cmdQ→dataQ→Tick 本桶房（G2/G1/E8 在内）
                bot_.TickShard(nowMs, shards_, lobby_, idx, evs);// 本桶 bot 直调（E3——零一拍假延迟）
                shards_.PublishToMerge(idx, std::move(evs));     // 产物入合流（锁瞬时）
            } catch (...) {
                FaultClose_(idx);                                // G6：关房不连坐（noexcept）
            }
            tickNo++;
            // 节拍：sleep_until 下一拍（ADR-1——now 由 loop 统一给，session 禁内联取钟）
            next += std::chrono::milliseconds(kTickPeriodMs);
            std::this_thread::sleep_until(next);
            if (tickNo == 1)                                     // 首拍后重锚（防首拍处理超期连追）
                next = std::chrono::steady_clock::now();
        }
    }

    /** G6（v1.2 裁决四条）：noexcept——不碰 session（异常后状态不可信）；
     *  动作=本 shard 全部活跃房 AbortMatch（worker 自己的 shard·安全）+横幅计数；
     *  半轮事件语义=客户端收部分快照→对拍对 fault 房豁免（异常路径不保证确定性）。 */
    void FaultClose_(std::size_t idx) noexcept {
        try {
            shards_.FaultAbortShard(idx);
            faultCount_.fetch_add(1, std::memory_order_relaxed);
        } catch (...) { /* 吞——noexcept 内不得再抛 */ }
    }

    static std::uint64_t NowMs_() {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

    sightline::match::MatchShards& shards_;
    BotDriver& bot_;
    sightline::lobby::LobbyService& lobby_;
    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> faultCount_{0};
};

} // namespace adapters
