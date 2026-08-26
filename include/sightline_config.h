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
