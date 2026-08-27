//
// Created by 29108 on 2025/9/12.
//
#include "net/HierarchicalTimingWheel.h"

#include <cassert>

#include "logger/logger.h"

namespace common
{
    namespace timer
    {
        HierarchicalTimingWheel::HierarchicalTimingWheel() : last_tick_time_(std::chrono::steady_clock::now()), next_timer_id_(1) {
            // 初始化4层时间轮，精度从高到低：
            levels_.emplace_back(256, std::chrono::milliseconds(1));      // L0: 256槽×1ms = 0-255ms范围
            levels_.emplace_back(64, std::chrono::milliseconds(256));     // L1: 64槽×256ms = 0-16.384s范围
            levels_.emplace_back(64, std::chrono::milliseconds(16384));   // L2: 64槽×16.384s = 0-17.47分钟范围  
            levels_.emplace_back(24, std::chrono::milliseconds(1048576)); // L3: 24槽×1048.576s = 0-6.99小时范围
        }

        std::uint64_t HierarchicalTimingWheel::addTimer(std::chrono::milliseconds delay,
                                                        Callback callback, std::chrono::milliseconds interval) {
            auto now = std::chrono::steady_clock::now();
            auto expire_time = now + delay;           // 基于最后tick时间计算到期
            auto timer = std::make_shared<Timer>(next_timer_id_++, std::move(callback),       // 创建定时器对象，分配唯一ID
                                               expire_time, interval);

            //           (interval.count() > 0 ? " (periodic)" : " (one-shot)"));

            {
                //加锁保护 timer_map_
                std::lock_guard<std::mutex> lock(timer_mutex_);
                timer_map_[timer->id] = timer;
                
                // ✅ 新增：同时添加到到期时间堆（O(1)优化getNextExpiration）
                expiration_heap_.push({expire_time, timer->id});
            }
            insertTimer(timer, now);                                                    // 将定时器插入到合适的层级和槽位
            return timer->id;                                                      // 返回定时器ID
        }

        void HierarchicalTimingWheel::tick(){
            // ⭐架构修订：到期判定改为"堆直驱"，槽位结构停用
            //
            // 槽位机器（L3 入口判定 → 逐层降级搬运 → 指针扫槽）存在连环缺陷：
            // ① 入口从最粗层判定，任意延迟都先落 L3 再逐层下搬；
            // ② 未到期者滞留已扫过的槽，要等整圈才会被再处理（周期停摆根因之一）；
            // ③ carry 链在 tick 间隔 < 槽宽时锁死高层推进（根因之二）。
            // 而 expiration_heap_ 中本就存有每个定时器的精确到期时刻
            // （getNextExpiration 一直靠它驱动 timerfd）——直接扫堆即得全部到期者，
            // 精度 = tick 间隔，且消灭"指针-槽位对齐"这一整类缺陷。
            auto now = std::chrono::steady_clock::now();

            std::vector<std::shared_ptr<Timer>> due;
            {
                std::lock_guard<std::mutex> lock(timer_mutex_);
                while (!expiration_heap_.empty() && expiration_heap_.top().expire_time <= now) {
                    auto entry = expiration_heap_.top();
                    expiration_heap_.pop();
                    auto it = timer_map_.find(entry.timer_id);
                    if (it == timer_map_.end()) continue;              // 已取消/已触发的陈旧堆项
                    auto timer = it->second.lock();
                    if (!timer || timer->cancelled) { timer_map_.erase(it); continue; }
                    if (entry.expire_time != timer->expire_time) continue;   // 周期重入的陈旧轮次
                    due.push_back(timer);
                }
            }

            for (auto& timer : due) {
                submitAsyncTask([timer] { timer->callback(); });

                if (timer->interval.count() > 0) {
                    // 周期：从原到期时间推下一轮（避免累积误差），严重滞后则追赶
                    auto next = timer->expire_time + timer->interval;
                    while (next <= now) next += timer->interval;
                    std::lock_guard<std::mutex> lock(timer_mutex_);
                    timer->expire_time = next;
                    expiration_heap_.push({next, timer->id});   // map 条目复用，id 不变
                } else {
                    std::lock_guard<std::mutex> lock(timer_mutex_);
                    timer_map_.erase(timer->id);                // 一次性：出账
                }
            }

            last_tick_time_ = now;
        }

        void HierarchicalTimingWheel::cancelTimer(uint64_t id) {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            //  O(1) 取消
            if (auto it = timer_map_.find(id); it != timer_map_.end()) {
                if (auto timer = it->second.lock()) {
                    timer->cancelled = true;
                }
                timer_map_.erase(it);
                // ✅ 修复：不再从堆中移除
            }
        }

        int HierarchicalTimingWheel::getNextExpiration(int default_timeout) const {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            
            auto now = std::chrono::steady_clock::now();
            
            // ✅ O(1)优化：使用最小堆快速获取最近到期时间
            // 延迟清理：从堆顶移除已取消/不存在的定时器
            while (!expiration_heap_.empty()) {
                const auto& entry = expiration_heap_.top();
                
                // 检查定时器是否仍然有效
                auto it = timer_map_.find(entry.timer_id);
                if (it != timer_map_.end()) {
                    if (auto timer = it->second.lock()) {
                        if (!timer->cancelled) {
                            // 找到有效定时器
                            auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(
                                entry.expire_time - now);
                            int delay_ms = static_cast<int>(delay.count());
                            
                            // 如果已过期，返回1ms
                            if (delay_ms <= 0) {
                                return 1;
                            }

                            return delay_ms;
                        }
                    }
                }
                
                // 定时器已取消或不存在，移除并继续检查下一个
                expiration_heap_.pop();
            }
            
            // 堆为空或所有定时器都已取消
            //           std::to_string(default_timeout) + "ms");
            return default_timeout;
        }
        
        bool HierarchicalTimingWheel::hasActiveTimers() const {
            std::lock_guard<std::mutex> lock(timer_mutex_);
            
            // 检查timer_map_中是否有任何非取消的定时器
            for (const auto& pair : timer_map_) {
                if (auto timer = pair.second.lock()) {
                    if (!timer->cancelled) {
                        return true; // 找到活跃定时器
                    }
                }
            }
            
            return false; // 没有活跃定时器
        }

        void HierarchicalTimingWheel::insertTimer(std::shared_ptr<Timer> timer, TimePoint now) {
            for (int level = static_cast<int>(levels_.size()) - 1; level >= 0; --level) {
                // 从最粗粒度层开始判断，避免小延迟误入高层
                auto& wheel = levels_[level];
                auto max_delay = wheel.slot_duration * (wheel.size - 1);

                auto delay = timer->expire_time - now;
                if (delay <= max_delay || level == 0) {
                    size_t slot = wheel.calculateRelativeSlot(timer->expire_time, now);
                    wheel.slots[slot].push_back(timer);
                    return;
                }
            }
        }

        void HierarchicalTimingWheel::processExpiredTimers(size_t level, TimePoint now){
            auto& wheel = levels_[level];
            auto& slot = wheel.slots[wheel.current_slot];
            

            auto it = slot.begin();
            int processed_count = 0;
            
            while (it != slot.end()) {
                auto timer = *it;

                if (timer->cancelled) {
                    it = slot.erase(it);
                    {
                        // 加锁保护 timer_map_
                        std::lock_guard<std::mutex> lock(timer_mutex_);
                        timer_map_.erase(timer->id);
                        // ✅ 修复：不再从堆中移除
                    }
                    continue;
                }

                // 双重检查：避免因时间误差提前执行
                if (timer->expire_time <= now) {
                    processed_count++;
                    
                    try {
                        submitAsyncTask([timer](){
                            timer->callback();
                        });
                    } catch (const std::exception& e) {
                        LOG_ERROR("[TIMING_WHEEL] Timer callback exception for timer " + std::to_string(timer->id) + ": " + std::string(e.what()));
                    } catch (...) {
                        LOG_ERROR("[TIMING_WHEEL] Unknown timer callback exception for timer " + std::to_string(timer->id));
                    }

                    // 同步重新插入周期性定时器
                    if (timer->interval.count() > 0) {
                        // ✅ 修复：基于原到期时间计算，避免累积误差
                        auto new_expire = timer->expire_time + timer->interval;
                        
                        // ✅ 如果严重滞后（回调耗时过长），追赶到最近的周期
                        while (new_expire <= now) {
                            new_expire += timer->interval;
                        }

                        {
                            // 加锁：因为 insertTimer 不是线程安全的
                            std::lock_guard<std::mutex> lock(timer_mutex_);
                            timer->expire_time = new_expire;
                            insertTimer(timer, now);
                            
                            // ✅ 新增：将新的到期时间添加到堆
                            expiration_heap_.push({new_expire, timer->id});
                        }
                        // 周期性定时器已重新插入
                    } else {
                        // 一次性定时器，从 map 移除
                        std::lock_guard<std::mutex> lock(timer_mutex_);
                        timer_map_.erase(timer->id);
                        // ✅ 修复：不再从堆中移除（因为没加入）
                        // 一次性定时器已移除
                    }

                    it = slot.erase(it);

                } else {
                    ++it; // 未到期，保留
                }
            }
            
        }

        void HierarchicalTimingWheel::processCurrentSlotTimers(size_t level, TimePoint now){
            auto& wheel = levels_[level];
            auto& slot = wheel.slots[wheel.current_slot];

            if (slot.empty()) {
                return; // 无定时器，直接返回
            }

            auto it = slot.begin();
            int moved_to_l0 = 0;
            int moved_to_lower = 0;
            int cancelled_count = 0;
            
            while (it != slot.end()) {
                auto timer = *it;

                if (timer->cancelled) {
                    cancelled_count++;
                    {
                        std::lock_guard<std::mutex> lock(timer_mutex_);
                        timer_map_.erase(timer->id);
                        // ✅ 修复：不再从堆中移除
                    }
                    it = slot.erase(it);
                    continue;
                }

                // 情况1：已到期 —— 搬运到 L0 执行
                if (timer->expire_time <= now) {
                    if (level == 0) {
                        // L0层的到期定时器保留，等待 processExpiredTimers 处理
                        ++it;
                    } else {
                        // 高层到期定时器直接搬运到 L0
                        moved_to_l0++;
                        it = slot.erase(it);
                        insertTimerToLevel(timer, 0, now);
                    }
                }
                // 未到期，但可降级到下一层（且不是 L0）
                else if (level > 0) {
                    auto remaining = timer->expire_time - now;
                    auto& lower_wheel = levels_[level - 1];
                    auto lower_max_delay = lower_wheel.slot_duration * (lower_wheel.size - 1);

                    if (remaining <= lower_max_delay) {
                        // 降级搬运到下一层
                        moved_to_lower++;
                        it = slot.erase(it);
                        insertTimerToLevel(timer, level - 1, now);
                    } else {
                        ++it;
                    }
                } else {
                    // 未到期且不能降级 —— ⭐修复周期定时器停摆：
                    // 指针已扫到本槽但未到期时，绝不能留在原地（指针已过去，
                    // 要等整圈才会再扫到——runEvery 首次触发后即停摆的根因），
                    // 必须按剩余时间重插到未来槽位
                    it = slot.erase(it);
                    {
                        std::lock_guard<std::mutex> lock(timer_mutex_);
                        insertTimer(timer, now);
                    }
                }
            }

            if (moved_to_l0 > 0 || moved_to_lower > 0 || cancelled_count > 0) {
            }
        }

        void HierarchicalTimingWheel::insertTimerToLevel(std::shared_ptr<Timer> timer, size_t target_level,
        TimePoint now) {
            auto& wheel = levels_[target_level];
            size_t slot = wheel.calculateRelativeSlot(timer->expire_time, now);
            wheel.slots[slot].push_back(timer);
        }
    }
}