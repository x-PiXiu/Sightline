// sightline_config.h —— 全局配置（组装层，main 填充；对应 01 文档"10 行结构体替代 280 行 networkConfig"）
// 15 号 01 文档 D1：新增 applyLua——config.lua 为热更单一来源，命令行优先级最高

#pragma once
#include <cstdint>
#include <string>
#include "net/event_loop.h"
#include "lua/lua_vm.h"
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

    // GM 管理 API（03 文档）：0=关闭；token 为 Bearer 凭据，正式部署必须改默认值
    uint16_t admin_port = 8080;
    std::string admin_token = "sightline-dev-token";

    common::network::EventLoopConfig loop;
    app::SessionService::Config session;
    app::RoomService::Config room;

    // 命令行：./sightline [port] [debug]   —— debug 时日志级别降为 DEBUG（实时观察连接进出）
    static SightlineConfig fromArgs(int argc, char* argv[]) {
        SightlineConfig c;
        if (argc > 1) c.port = static_cast<uint16_t>(std::stoi(argv[1]));
        if (argc > 2 && std::string(argv[2]) == "debug") c.debug_log = true;
        return c;
    }
    bool debug_log = false;

    /** Lua 配置应用（键存在才覆盖——当前值作缺省，未配置键保持原值）。
     *  优先级：命令行参数 > config.lua > 内置默认 */
    void applyLua(sightline::LuaVM& vm)
    {
        port                = vm.getInt("network", "port", port);
        session.heartbeat_timeout_ms = vm.getInt("network", "heartbeat_timeout_ms",
                                                session.heartbeat_timeout_ms);
        session.scan_interval_ms     = vm.getInt("network", "scan_interval_ms",
                                                session.scan_interval_ms);
        room.room_rules.kills_to_win = vm.getInt("game", "win_kills",
                                                room.room_rules.kills_to_win);
        room.room_rules.max_players  = vm.getInt("game", "max_players",
                                                room.room_rules.max_players);
        room.respawn_delay_ms        = vm.getInt("game", "respawn_ms",
                                                room.respawn_delay_ms);
        admin_port   = static_cast<uint16_t>(vm.getInt("admin", "port", admin_port));
        admin_token  = vm.getString("admin", "token", admin_token);
    }
};

} // namespace sightline
