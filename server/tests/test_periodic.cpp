// 周期定时器停摆复现：runEvery(1000ms) 应持续触发；观察第几条后停
#include <chrono>
#include <cstdio>
#include "net/event_loop.h"

using common::network::EventLoop;
using Clock = std::chrono::steady_clock;

int main() {
    EventLoop loop;
    auto t0 = Clock::now();
    auto ms = [&]{ return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-t0).count(); };

    int fired = 0;
    loop.runEvery(1000, [&]{
        ++fired;
        std::printf("[%5ld ms] periodic #%d\n", ms(), fired);
        std::fflush(stdout);
        if (fired >= 30) loop.quit();   // 30 秒足够暴露停摆
    });
    // 混入一次性定时器（真实服务器里心跳重生都在用，模拟干扰）
    for (int i = 0; i < 5; ++i) {
        loop.runAfter(300 + i * 700, []{ std::printf("  (one-shot)\n"); std::fflush(stdout); });
    }
    loop.loop();
    std::printf("total fired = %d (expect 30)\n", fired);
    return 0;
}
