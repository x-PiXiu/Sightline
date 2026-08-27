// 时间轮真实精度测试：验证 runAfter(500/1500/3000ms) 的触发与时刻
#include <chrono>
#include <cstdio>
#include "net/event_loop.h"

using common::network::EventLoop;
using Clock = std::chrono::steady_clock;

int main() {
    EventLoop loop;
    auto t0 = Clock::now();
    auto ms = [&]{ return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count(); };

    loop.runAfter(500,  [&]{ std::printf("[%5ld ms] 500ms timer fired\n", ms()); });
    loop.runAfter(1500, [&]{ std::printf("[%5ld ms] 1500ms timer fired\n", ms()); });
    loop.runAfter(3000, [&]{ std::printf("[%5ld ms] 3000ms timer fired\n", ms()); std::fflush(stdout); loop.quit(); });
    loop.runAfter(6000, [&]{ std::printf("[%5ld ms] 6000ms timer fired (should NOT happen)\n", ms()); std::fflush(stdout); });

    loop.loop();
    return 0;
}
