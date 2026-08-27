// 时间轮本体隔离测试：手动 tick（真实 sleep），绕过 EventLoop/timerfd
// 若停摆 → 轮子数学有病；若正常 → EventLoop 集成路径有病
#include <chrono>
#include <cstdio>
#include <thread>
#include "net/heap_scheduler.h"

using common::timer::HeapScheduler;
using Clock = std::chrono::steady_clock;

int main() {
    HeapScheduler wheel;   // 同步执行（不设 submitter）
    auto t0 = Clock::now();
    auto ms = [&]{ return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count(); };

    int periodic = 0, oneshot = 0;
    wheel.addTimer(std::chrono::milliseconds(1000), [&]{ ++periodic;
        std::printf("[%5ld] periodic #%d\n", ms(), periodic); std::fflush(stdout); },
        std::chrono::milliseconds(1000));                       // 周期 1s
    wheel.addTimer(std::chrono::milliseconds(1700), [&]{ ++oneshot;
        std::printf("[%5ld] one-shot\n", ms()); std::fflush(stdout); });
    wheel.addTimer(std::chrono::milliseconds(2500), [&]{ ++oneshot;
        std::printf("[%5ld] one-shot\n", ms()); std::fflush(stdout); });

    // 每 50ms 手动 tick 一次，跑 4 秒
    for (int i = 0; i < 90; ++i) {
        wheel.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("periodic=%d (expect 4) oneshot=%d (expect 2)\n", periodic, oneshot);
    return 0;
}
