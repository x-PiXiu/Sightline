// 底座冒烟测试：无参 EventLoop 创建 → 日志装配 → 定时器触发 → 干净退出
// 验证毕设网络库原版在 WSL 下可编译、可链接、可运行（不碰 ConfigManager 路径）
// ⚠️ 遗产行为记录：毕设 Logger 不 addSink 就静默丢日志——装配是使用者的责任
#include "common/logger/logger.h"
#include "common/network/event_loop.h"

int main() {
    // 日志装配：ConsoleSink 必须显式添加，否则 LOG_xxx 全部静默丢弃
    common::logger::Logger::getInstance().addSink(
        std::make_unique<common::logger::ConsoleSink>());

    common::network::EventLoop loop;   // 无参构造：不触发 ConfigManager
    LOG_INFO("smoke: EventLoop created");

    bool fired = false;
    loop.runAfter(100, [&fired] {
        fired = true;
        LOG_INFO("smoke: timer fired (runAfter 100ms)");
    });
    loop.runAfter(300, [&loop] { loop.quit(); });

    loop.loop();

    if (!fired) {
        LOG_ERROR("smoke: FAILED — timer did not fire");
        return 1;
    }
    LOG_INFO("smoke: PASS — base library alive");
    return 0;
}
