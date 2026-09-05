// adapters/timer_wheel_adapter.h —— ITimerScheduler 的实现：
// 透传 EventLoop 的堆调度器（runAfter/runEvery → HeapScheduler + timerfd）
// 适配成用例层认识的接口。约 30 行代码让全部超时逻辑可脱离 timerfd 单测。

#pragma once
#include "application/ports/i_timer_scheduler.h"
#include <functional>
#include "net/event_loop.h"

namespace sightline::adapters {

class TimerSchedulerAdapter : public app::ITimerScheduler {
public:
    explicit TimerSchedulerAdapter(common::network::EventLoop& loop) : loop_(loop) {}

    uint64_t runAfter(int ms, std::function<void()> cb) override {
        return loop_.runAfter(ms, std::move(cb));
    }
    uint64_t runEvery(int ms, std::function<void()> cb) override {
        return loop_.runEvery(ms, std::move(cb));
    }
    void cancel(uint64_t id) override { loop_.cancelTimer(id); }

private:
    common::network::EventLoop& loop_;
};

// 兼容旧名（历史代码引用）
using TimerWheelAdapter = TimerSchedulerAdapter;

} // namespace sightline::adapters
