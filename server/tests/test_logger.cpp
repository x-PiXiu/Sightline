// tests/logger/test_logger.cpp —— 限频语义单测（同步模式，CountingSink 计数）
// 1) ERROR 不受限频；2) 同调用点 INFO 超限丢弃；3) 窗口滚动后补聚合摘要

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include "logger/logger.h"

using common::logger::LogEntry;
using common::logger::LogLevel;
using common::logger::LogSink;
using common::logger::Logger;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

class CountingSink : public LogSink {
public:
    void write(const LogEntry&) override { count_.fetch_add(1); }
    void flush() override {}
    int count() const { return count_.load(); }
private:
    std::atomic<int> count_{0};
};

int main() {
    Logger& logger = Logger::getInstance();
    logger.setLogLevel(LogLevel::TRACE);

    auto* sink = new CountingSink();
    logger.addSink(std::unique_ptr<LogSink>(sink));
    logger.setRateLimitPerSecond(5);

    // 1. ERROR/FATAL 永不限频
    for (int i = 0; i < 50; ++i) logger.error("boom", "t.c", 1);
    CHECK(sink->count() == 50);

    // 2. 同一调用点 INFO：限 5 条/秒，100 次只过 5
    for (int i = 0; i < 100; ++i) logger.info("storm", "t.c", 2);
    CHECK(sink->count() == 55);

    // 3. 窗口滚动（>1s）：下一条 INFO 触发"上窗口 N 条被抑制"摘要 + 本条放行
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    logger.info("storm", "t.c", 2);
    CHECK(sink->count() == 57);   // 50 + 5 + 1摘要 + 1本条

    if (g_failures == 0) std::printf("test_logger: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
