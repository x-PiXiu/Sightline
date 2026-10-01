//
// Created by 29108 on 2025/9/12.
//

#ifndef HIERARCHICALTIMINGWHEEL_H
#define HIERARCHICALTIMINGWHEEL_H
#include <chrono>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>
#include <mutex>
#include <unordered_map>  // ✅ 修改：用于timer_map_
#include <queue>  // ✅ 新增：用于expiration_heap_（最小堆）

namespace common
{
    namespace timer
    {
        class HierarchicalTimingWheel {
        public:
            using Callback = std::function<void()>;                    // 定时器回调函数类型别名
            using TimePoint = std::chrono::steady_clock::time_point;   // 时间点类型别名，使用稳定时钟
            using Duration = std::chrono::milliseconds;

            struct Timer {
                uint64_t id;                                           // 定时器唯一标识符
                Callback callback;                                     // 定时器到期时执行的回调函数
                TimePoint expire_time;                                 // 定时器到期的绝对时间点
                Duration interval;                   // 周期性定时器的间隔时间，0表示一次性定时器
                bool cancelled = false;                                // 标记定时器是否已被取消

                // Timer构造函数：初始化所有成员变量
                Timer(uint64_t id, Callback cb, TimePoint expire, std::chrono::milliseconds intv = std::chrono::milliseconds::zero())
                    : id(id), callback(cb), expire_time(expire), interval(intv) {}
            };

            // ✅ 新增：到期时间追踪结构（O(1)优化）
            struct ExpirationEntry {
                TimePoint expire_time;
                uint64_t timer_id;
                
                bool operator>(const ExpirationEntry& other) const {
                    return expire_time > other.expire_time;  // 最小堆：小的在顶部
                }
            };

        private:
            // 时间轮层级结构体：每一层都是一个独立的时间轮
            struct WheelLevel {
                std::vector<std::list<std::shared_ptr<Timer>>> slots;      // 该层的所有槽位，每个槽位存储定时器链表
                size_t current_slot;                                       // 该层当前指针所在的槽位索引
                Duration slot_duration;                  // 该层每个槽位代表的时间长度
                size_t size;                             // 该层的槽位总数
                TimePoint base_time;  // 本层的"当前圈"起始时间（动态更新）

                // WheelLevel构造函数：初始化一层时间轮
                WheelLevel(size_t wheel_size, std::chrono::milliseconds duration)
                    : slots(wheel_size),                                   // 创建指定数量的槽位
                      current_slot(0),                                     // 指针从第0个槽位开始
                      slot_duration(duration),                             // 设置每个槽位的时间长度
                      size(wheel_size),                                    // 记录槽位总数
                      base_time(std::chrono::steady_clock::now()) {}

                //  推进一层，返回是否转完一圈（用于触发上层）
                bool advance(TimePoint now) {
                    //  时间补偿：计算自 base_time 以来应推进多少槽
                    auto elapsed = now - base_time;
                    if (elapsed < slot_duration) return false; // 未到下一个槽

                    uint64_t ticks = elapsed / slot_duration;
                    if (ticks == 0) return false;

                    // 支持跳 tick（系统卡顿、tick 延迟时）
                    for (uint64_t i = 0; i < ticks && i < size; ++i) {
                        current_slot = (current_slot + 1) % size;
                        if (current_slot == 0) {
                            base_time = now; // 重置 base_time，避免累积误差
                            return true;     // 转完一圈，需要进位
                        }
                    }
                    //  更新 base_time，避免浮点误差累积
                    base_time += ticks * slot_duration;
                    return false;
                }

                //  计算"相对于本层当前圈"的槽位（不是全局时间！）
                size_t calculateRelativeSlot(TimePoint expire, TimePoint /* now */) const {
                    if (expire <= base_time) return current_slot; // 已过期，放入当前槽立即处理

                    auto delay = expire - base_time;
                    uint64_t slots_away = delay / slot_duration;
                    return (current_slot + slots_away) % size;
                }
            };

            TimePoint last_tick_time_;  // 上次tick时间（相对基准）
            std::unordered_map<uint64_t, std::weak_ptr<Timer>> timer_map_; // 存储所有层级的时间轮
            std::vector<WheelLevel> levels_;
            uint64_t next_timer_id_;                                       // 下一个定时器的唯一ID

            // 保护 timer_map_ 和 insertTimer 的互斥锁（异步线程可能访问）
            mutable std::mutex timer_mutex_;

            // 异步任务提交接口（由外部 EventLoop 注入）
            std::function<void(std::function<void()>)> async_task_submitter_ = nullptr;

            // ✅ 新增：到期时间追踪最小堆（O(1)优化getNextExpiration）
            // 设计原则：
            // 1. 只存储到期时间和ID，不存储完整Timer对象（避免双重存储）
            // 2. 堆顶永远是最近到期的定时器
            // 3. getNextExpiration()只需访问堆顶 → O(1)
            // 4. 定时器取消时，不立即从堆中删除（延迟清理）
            mutable std::priority_queue<
                ExpirationEntry, 
                std::vector<ExpirationEntry>, 
                std::greater<ExpirationEntry>
            > expiration_heap_;

        public:
            // 构造函数：初始化多层时间轮
            HierarchicalTimingWheel();

            // 添加定时器到分层时间轮中
            std::uint64_t addTimer(std::chrono::milliseconds delay, Callback callback,
                                   std::chrono::milliseconds interval = std::chrono::milliseconds::zero());

            // 推进分层时间轮
            void tick();

            // 取消指定ID的定时器
            void cancelTimer(uint64_t id);

            void setAsyncTaskSubmitter(std::function<void(std::function<void()>)> submitter) {
                async_task_submitter_ = std::move(submitter);
            }

            // 获取到下一次定时器到期的毫秒数，如果没有定时器则返回 default_timeout
            int getNextExpiration(int default_timeout = 100) const;
            
            // 检查是否有活跃的定时器
            bool hasActiveTimers() const;

        private:
            // 将定时器插入到合适的层级和槽位
            void insertTimer(std::shared_ptr<Timer> timer, TimePoint now);

            // ✅ 已删除：updateTimerHeap()（不再使用堆）

            void processExpiredTimers(size_t level, TimePoint now);

            // 处理指定层当前槽中的所有定时器（搬运或标记到期）
            void processCurrentSlotTimers(size_t level, TimePoint now);

            // 将定时器精准插入到指定层级（不从顶层重新判断）
            void insertTimerToLevel(std::shared_ptr<Timer> timer, size_t target_level, TimePoint now);

            // 封装异步任务提交逻辑（带降级处理）
            void submitAsyncTask(std::function<void()> task) {
                if (async_task_submitter_) {
                    async_task_submitter_(std::move(task));
                } else {
                    // 降级：同步执行（仅用于测试或未设置 submitter 时，生产环境应避免）
                    task();
                }
            }
        };
    }
}

#endif //HIERARCHICALTIMINGWHEEL_H