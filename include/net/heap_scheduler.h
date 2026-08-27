// net/heap_scheduler.h —— 定时器调度器（最小堆驱动，tick 扫描到期执行）
//
// 演化史：最初为四层分层时间轮；修复"周期定时器停摆"时到期判定重构为堆直驱
// （根因：槽位机器的指针-槽位对齐存在连环缺陷，见知识库笔记15），随后槽位
// 结构整体删除。保留 O(log n) 堆实现——万级定时器规模下与 O(1) 时间轮的
// 差异可忽略，而语义精确、代码量减半。名实相符是本次删除的唯一理由。

#ifndef HEAP_SCHEDULER_H
#define HEAP_SCHEDULER_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

namespace common {
    namespace timer {

        class HeapScheduler {
        public:
            using Callback = std::function<void()>;
            using TimePoint = std::chrono::steady_clock::time_point;
            using Duration = std::chrono::milliseconds;

            struct Timer {
                uint64_t id;
                Callback callback;
                TimePoint expire_time;      // 绝对到期时刻（堆排序键）
                Duration interval;          // 周期定时器的间隔，0 = 一次性
                bool cancelled = false;

                Timer(uint64_t id_, Callback cb, TimePoint expire,
                      Duration intv = std::chrono::milliseconds::zero())
                    : id(id_), callback(std::move(cb)), expire_time(expire), interval(intv) {}
            };

            // 最小堆条目：只存到期时刻与 id（延迟清理已取消/陈旧条目）
            struct ExpirationEntry {
                TimePoint expire_time;
                uint64_t timer_id;
                bool operator>(const ExpirationEntry& other) const {
                    return expire_time > other.expire_time;
                }
            };

            HeapScheduler();

            // 注册定时器；interval 非零为周期定时器（从原到期时间推下一轮，防累积误差）
            uint64_t addTimer(Duration delay, Callback callback,
                              Duration interval = std::chrono::milliseconds::zero());

            // 推进：执行所有 expire_time <= now 的定时器（回调经 submitter 提交）
            void tick();

            void cancelTimer(uint64_t id);

            // 回调执行方式注入（EventLoop 注入 runInLoop 保证回调在 loop 线程执行）
            void setAsyncTaskSubmitter(std::function<void(std::function<void()>)> submitter) {
                async_task_submitter_ = std::move(submitter);
            }

            // 距最近有效到期的毫秒数（驱动 timerfd 睡眠时长）；无定时器返回 default_timeout
            int getNextExpiration(int default_timeout) const;
            bool hasActiveTimers() const;

        private:
            void submitAsyncTask(std::function<void()> task) {
                if (async_task_submitter_) {
                    async_task_submitter_(std::move(task));
                } else {
                    task();   // 降级同步执行（仅测试或未设置 submitter 时）
                }
            }

            TimePoint last_tick_time_;
            std::unordered_map<uint64_t, std::shared_ptr<Timer>> timer_map_;  // id → 定时器（强引用：唯一持有者）
            // 惰性清理缓存：const 的 getNextExpiration 也要弹出陈旧条目
            mutable std::priority_queue<ExpirationEntry, std::vector<ExpirationEntry>,
                                        std::greater<ExpirationEntry>> expiration_heap_;
            uint64_t next_timer_id_ = 1;
            mutable std::mutex timer_mutex_;                                  // 保护 map/heap
            std::function<void(std::function<void()>)> async_task_submitter_;
        };

    } // namespace timer
} // namespace common

#endif // HEAP_SCHEDULER_H
