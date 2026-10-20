// net/heap_scheduler.cpp —— 定时器调度器实现（堆直驱，演化史见头文件）

#include "net/heap_scheduler.h"
#include <memory>
#include <chrono>
#include "logger/logger.h"

namespace common {
    namespace timer {

        HeapScheduler::HeapScheduler()
            : last_tick_time_(std::chrono::steady_clock::now()), next_timer_id_(1) {}

        uint64_t HeapScheduler::addTimer(Duration delay, Callback callback, Duration interval) {
            auto now = std::chrono::steady_clock::now();
            auto expire_time = now + delay;
            auto timer = std::make_shared<Timer>(next_timer_id_++, std::move(callback),
                                                 expire_time, interval);
            {
                std::lock_guard<std::mutex> lock(timer_mutex_);
                timer_map_[timer->id] = timer;
                expiration_heap_.push({expire_time, timer->id});
            }
            return timer->id;
        }

        void HeapScheduler::tick() {
            auto now = std::chrono::steady_clock::now();

            // 取出全部已到期的有效定时器
            std::vector<std::shared_ptr<Timer>> due;
            {
                std::lock_guard<std::mutex> lock(timer_mutex_);
                while (!expiration_heap_.empty() && expiration_heap_.top().expire_time <= now) {
                    auto entry = expiration_heap_.top();
                    expiration_heap_.pop();
                    auto it = timer_map_.find(entry.timer_id);
                    if (it == timer_map_.end()) continue;                    // 已取消/已触发的陈旧堆项
                    auto timer = it->second;   // map 持强引用，必有有效对象
                    if (timer->cancelled) { timer_map_.erase(it); continue; }
                    if (entry.expire_time != timer->expire_time) continue;   // 周期重入的陈旧轮次
                    due.push_back(timer);
                }
            }

            for (auto& timer : due) {
                submitAsyncTask([timer] { timer->callback(); });

                if (timer->interval.count() > 0) {
                    // 周期：从原到期时间推下一轮（防累积误差），严重滞后则追赶
                    auto next = timer->expire_time + timer->interval;
                    while (next <= now) next += timer->interval;
                    std::lock_guard<std::mutex> lock(timer_mutex_);
                    timer->expire_time = next;
                    expiration_heap_.push({next, timer->id});
                } else {
                    std::lock_guard<std::mutex> lock(timer_mutex_);
                    timer_map_.erase(timer->id);
                }
            }

            last_tick_time_ = now;
        }

        void HeapScheduler::cancelTimer(uint64_t id) {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            if (auto it = timer_map_.find(id); it != timer_map_.end()) {
                if (auto timer = it->second) timer->cancelled = true;
                timer_map_.erase(it);
                // 堆中条目延迟清理（tick / getNextExpiration 扫到时丢弃）
            }
        }

        int HeapScheduler::getNextExpiration(int default_timeout) const {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            auto now = std::chrono::steady_clock::now();
            while (!expiration_heap_.empty()) {
                const auto& entry = expiration_heap_.top();
                auto it = timer_map_.find(entry.timer_id);
                if (it != timer_map_.end()) {
                    const auto& timer = it->second;   // map 持强引用
                    if (!timer->cancelled && entry.expire_time == timer->expire_time) {
                        int delay_ms = static_cast<int>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                entry.expire_time - now).count());
                        return delay_ms <= 0 ? 1 : delay_ms;
                    }
                }
                // 陈旧条目：弹出继续
                expiration_heap_.pop();
            }
            return default_timeout;
        }

        bool HeapScheduler::hasActiveTimers() const {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            for (const auto& pair : timer_map_) {
                if (!pair.second->cancelled) return true;
            }
            return false;
        }

    } // namespace timer
} // namespace common
