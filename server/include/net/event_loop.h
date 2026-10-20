//
// Created by 29108 on 2025/7/1.
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

        class Channel;

        class EventLoop {
        public:
            typedef std::function<void()> Functor;
            typedef std::vector<Channel*> ChannelList;

            EventLoop();
            ~EventLoop();

            //启动事件循环
            void loop();

            //退出事件循环
            void quit();

            // 检查事件循环是否正在运行
            bool isLooping() const { return looping_.load(); }

            //在IO线程中执行回调
            void runInLoop(const Functor& cb);

            //在下一次事件循环中执行回调
            void queueInLoop(const Functor& cb);

            // 添加定时器接口（透传给定时器堆）
            uint64_t runAfter(int ms, std::function<void()> cb);
            uint64_t runEvery(int ms, std::function<void()> cb);
            void cancelTimer(uint64_t id);

            // 唤醒事件循环
            void wakeup();

            // 更新Channel
            void updateChannel(Channel* channel);
            void removeChannel(Channel* channel);

            // 检查是否有指定Channel
            bool hasChannel(Channel* channel);

            // 检查是否在当前线程
            bool isInLoopThread() const;

            // 断言在循环线程中
            void assertInLoopThread() const {
                if (!isInLoopThread()) {
                    abortNotInLoopThread();
                }
            }

        private:

            std::atomic<bool> looping_;  // 事件循环运行标志
            std::atomic<bool> quit_;      // 退出标志
            mutable std::atomic<std::thread::id> threadId_;  // 绑定线程ID
            std::unique_ptr<Epoll> poller_;   // IO多路复用器

            int wakeupFd_;  // 唤醒文件描述符
            std::unique_ptr<Channel> wakeupChannel_;  // 唤醒通道

            ChannelList activeChannels_;  // 活跃通道列表
            std::mutex mutex_;  // 互斥锁（保护pendingFunctors_）
            std::vector<Functor> pendingFunctors_;  // 待执行回调队列
            std::atomic<bool> callingPendingFunctors_;  // 回调执行标志

        // 定时器实例（HeapScheduler 纯最小堆——2026-10-21 移植自毕设档案库：该实现当年即因
        // 时间轮指针-槽位对齐连环缺陷重构为堆直驱，与本仓轮病理同源；演化史见 heap_scheduler.h）
        std::unique_ptr<timer::HeapScheduler> heap_scheduler_;

            // 用于定时器堆的 timerfd
            int timerFd_; // 使用 timerfd_create 创建的文件描述符
            std::unique_ptr<Channel> timerChannel_; // 用于监听 timerfd 的 Channel

            void initTimer();

            //处理唤醒
            void handleRead();

            void handleTimerfdRead();

            void setInitialTimerfdTimeout();

            void resetTimerfd();

            //执行待处理回调
            void doPendingFunctors();

            //错误处理
            void abortNotInLoopThread() const;

        };
    }
}

#endif //EVENT_LOOP_H
