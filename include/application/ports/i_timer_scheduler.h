// application/ports/i_timer_scheduler.h —— Port #2：定时器
// 实现在 adapters/TimerWheelAdapter（透传 EventLoop 的分层时间轮 + timerfd）。
// 用例层的超时逻辑（心跳踢人/重生倒计时）借此可脱离 timerfd 单测（FakeScheduler）。

#pragma once
#include <functional>
#include <cstdint>

namespace sightline::app {

class ITimerScheduler {
public:
    virtual ~ITimerScheduler() = default;

    // ms 毫秒后执行 cb（回调在逻辑线程执行）；返回定时器 id，0 = 失败
    virtual uint64_t runAfter(int ms, std::function<void()> cb) = 0;
    virtual void cancel(uint64_t id) = 0;
};

} // namespace sightline::app
