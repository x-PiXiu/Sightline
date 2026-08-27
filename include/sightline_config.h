// sightline_config.h —— 全局配置（组装层，main 填充；对应 01 文档"10 行结构体替代 280 行 networkConfig"）

#pragma once
#include <cstdint>
#include <string>
#include "net/event_loop.h"
#include "application/session_service.h"
#include "application/room_service.h"

namespace sightline {

struct SightlineConfig {
    uint16_t port = 8888;
    int backlog = 1024;

    // 网络层（本轮优化新增：主从 Reactor / 水位 / 限速 / 统计周期）
    int io_threads = 2;                                  // IO 线程数（0=单Reactor）
    size_t high_water_mark = 4 * 1024 * 1024;            // 输出缓冲水位（慢客户端判定）
    size_t send_rate_bytes_per_sec = 0;                  // 每连接发送限速（0=不限）
    int stats_interval_s = 10;                           // 统计上报周期

    // 日志（异步/限频已在 main 默认启用；此处控制生产文件输出）
    bool file_log = false;                               // true: 追加 FileSink（滚动）
    std::string log_dir = "logs";

    common::network::EventLoopConfig loop;
    app::SessionService::Config session;
    app::RoomService::Config room;

    // 命令行覆盖：./sightline [port]
    static SightlineConfig fromArgs(int argc, char* argv[]) {
        SightlineConfig c;
        if (argc > 1) c.port = static_cast<uint16_t>(std::stoi(argv[1]));
        return c;
    }
};

} // namespace sightline
