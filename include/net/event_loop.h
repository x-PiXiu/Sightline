//
// EventLoop：one loop per thread 的 Reactor 核心
// 迁移自毕设网络库（2026-08 瘦身：删除 networkConfig 热更新全套，
// 配置收敛为启动时一次性注入的 EventLoopConfig，网络层不再依赖 ConfigManager/ThreadPool）
//

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "epoll.h"
#include "heap_scheduler.h"

namespace common {
    namespace network {

        // 事件循环核心参数（原 280 行 networkConfig 的替代品）
        struct EventLoopConfig {
            int epoll_timeout = 100;                // epoll_wait 基础超时(ms)，实际由最近定时器动态压低
            int init_event_list_size = 48;          // 事件数组初始大小
            int max_events = 1024;                  // 单次 wait 最大事件数
            bool enable_epoll_resize_optimization = true;
            double epoll_resize_factor = 1.5;

            void validate() const {
                if (epoll_timeout < 0 || init_event_list_size <= 0 || max_events <= 0) {
                    throw std::runtime_error("Invalid EventLoopConfig: bad epoll parameters");
                }
                if (enable_epoll_resize_optimization && epoll_resize_factor <= 1.0) {
                    throw std::runtime_error("Invalid EventLoopConfig: resize_factor must be > 1.0");
                }
            }
        };

        class Channel;

        class EventLoop {
        public:
            typedef std::function<void()> Functor;
            typedef std::vector<Channel*> ChannelList;

            explicit EventLoop(const EventLoopConfig& config = EventLoopConfig());
            ~EventLoop();

            // 主循环（阻塞，直到 quit()）
            void loop();

            // 退出循环（线程安全）
            void quit();

            bool isLooping() const { return looping_.load(); }

            // 在 IO 线程执行回调；若在其他线程调用则入队并用 eventfd 唤醒
            void runInLoop(const Functor& cb);
            void queueInLoop(const Functor& cb);

            // 定时器：透传给分层时间轮，回调最终在 IO 线程执行
            uint64_t runAfter(int ms, std::function<void()> cb);   // 一次性
            uint64_t runEvery(int ms, std::function<void()> cb);   // 周期性
            void cancelTimer(uint64_t id);

            // 唤醒阻塞中的 epoll_wait（写 eventfd）
            void wakeup();

            // Channel 注册（由 Channel::update() 间接调用）
            void updateChannel(Channel* channel);
            void removeChannel(Channel* channel);
            bool hasChannel(Channel* channel);

            bool isInLoopThread() const;
            void assertInLoopThread() const {
                if (!isInLoopThread()) {
                    abortNotInLoopThread();
                }
            }

            // 可观测：timerfd_settime 系统调用累计次数（心跳热路径诊断）
            uint64_t timerfdSettimeCount() const { return timerfd_settime_count_.load(std::memory_order_relaxed); }

        private:
            EventLoopConfig config_;

            std::atomic<bool> looping_;
            std::atomic<bool> quit_;
            mutable std::atomic<std::thread::id> threadId_;

            std::unique_ptr<Epoll> poller_;

            int wakeupFd_;                        // eventfd：跨线程唤醒
            std::unique_ptr<Channel> wakeupChannel_;

            ChannelList activeChannels_;
            std::mutex mutex_;                    // 保护 pendingFunctors_
            std::vector<Functor> pendingFunctors_;
            std::atomic<bool> callingPendingFunctors_;

            std::unique_ptr<timer::HeapScheduler> scheduler_;   // 定时器（堆直驱）  // 定时器（堆直驱到期）
            int timerFd_;                         // timerfd：把定时器挂进 epoll 统一事件源
            std::unique_ptr<Channel> timerChannel_;
            std::chrono::steady_clock::time_point timerfd_deadline_ =
                std::chrono::steady_clock::time_point::max();   // 当前 timerfd 睡向的时刻（跳过无谓 syscall）
            std::atomic<uint64_t> timerfd_settime_count_{0};

            void initTimer();                     // 按需创建 timerfd + Channel
            void initializeEpoll();

            void handleRead();                    // eventfd 读（清空唤醒计数）
            void handleTimerfdRead();             // timerfd 读 → 驱动时间轮 tick
            void setInitialTimerfdTimeout();
            void resetTimerfd();                  // 按最近到期定时器重设超时

            void doPendingFunctors();
            void abortNotInLoopThread() const;
        };
    }
}

#endif //EVENT_LOOP_H
