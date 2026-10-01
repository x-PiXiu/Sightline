// 最小复现：EventLoop::runEvery 周期路径是否触发（排查心跳 ACK 未回问题）
#include "net/event_loop.h"
#include "logger/logger.h"
int main() {
    common::logger::Logger::getInstance().addSink(std::make_unique<common::logger::ConsoleSink>());
    common::network::EventLoop loop;
    int count = 0;
    loop.runEvery(100, [&]{ count++; LOG_INFO("tick " + std::to_string(count)); });
    loop.runAfter(1000, [&]{ LOG_INFO("1秒到了 count=" + std::to_string(count)); loop.quit(); });
    loop.loop();
    return count == 0 ? 1 : 0;
}
