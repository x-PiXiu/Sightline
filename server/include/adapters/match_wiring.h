// match_wiring.h —— 第 3 期对局编排（适配器，D3-15 的落地形态）
// 职责：双用例（Lobby/Match）互不认识——fd↔PlayerId 翻译、房主/名册查询的组合在此完成
// 注册顺序契约：Register() 须先于 wireLobby() 调用——msgid 10 的对局离场钩子要先于
//               大厅离开执行（fd→PlayerId 翻译必须发生在 lobby 移除映射之前）
#pragma once
#include <chrono>
#include <cstring>

#include "adapters/lobby_wiring.h"   // MessageDispatcher / FrameWriter / ConnTable
#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/match_service.h"
#include "protocol/messages.h"   // v4 wire 结构体（variant 编解码——业务层禁 include，红线见 messages.h）
#include "protocol/msg_ids.h"

namespace adapters {

class MatchWiring {
public:
    MatchWiring(MessageDispatcher& disp, sightline::match::MatchService& match,
                sightline::lobby::LobbyService& lobby, FrameWriter& writer)
        : disp_(disp), match_(match), lobby_(lobby), writer_(writer) {}

    void Register() {
        // 19：位置上报 v2（v4 R10——对局期内替代 16）——Parse 集中校验（messages.h 唯一长度守门）
        // trace=true：30Hz×客户端 高频帧，收帧行降 TRACE（默认 DEBUG 档不再刷屏；判定不受影响）
        disp_.on(protocol::MSG_C2S_PLAYER_INPUT, "位置上报v2",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            auto msg = sightline::wire::PlayerInputMsg::Parse(bytes);
            if (!msg) {
                LOG_WARNING("[位置上报v2] 非法 payload 长度=" + std::to_string(payload.size()) +
                            " fd=" + std::to_string(fd));
                return;
            }
            const auto nowMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                            // 未登录/不在房：上报无效
            match_.UpdateEntity(static_cast<uint32_t>(pid),
                                msg->x, msg->y, msg->z, msg->yaw, msg->pitch,
                                msg->flags, msg->seq, nowMs);   // ep06：接收时刻入历史环（单钟）
        }, true);

        // 22 RTT_REPORT（ep06 回溯量源·1Hz·6B）：客户端心跳自测 RTT 上报——域内钳 1000ms（D6-04）
        disp_.on(protocol::MSG_C2S_RTT_REPORT, "RTT上报",
                 [this](int fd, const std::string& payload) {
            if (payload.size() != 6) {
                LOG_WARNING("[RTT上报] 非法长度=" + std::to_string(payload.size()) + " fd=" +
                            std::to_string(fd));
                return;
            }
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                            // 未登录/不在房：上报无效
            const auto rd16 = [&payload](int o) {
                return static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[o])) |
                       static_cast<std::uint16_t>(static_cast<std::uint8_t>(payload[o + 1])) << 8;
            };
            match_.SetShooterRtt(static_cast<std::uint32_t>(pid), rd16(0));
            // up/down KB/s（offset 2/4）：ep12 带宽统计消费——本期收下不存
        });

        // 16：v4 R10 对局期停用——在局者收到 16 视为无效帧丢弃（19 替代）；对局外 16 本就无效（未知 pid 静默）
        disp_.on(protocol::MSG_C2S_MOVE_REPORT, "位置上报(v4停用)",
                 [this](int fd, const std::string&) {
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid >= 0 && match_.IsActive(static_cast<std::uint32_t>(pid))) {
                LOG_WARNING("[位置上报] v4 R10：对局期 16 已停用 fd=" + std::to_string(fd));
                return;
            }
        });

        // 20：开火请求（v4.2 · 空 payload）——判定全用服务器权威状态（D5-01）；
        // 时间由适配层注入（steady 时钟，域不持钟）；FireResult 经 FrameWriter 翻译成 21 号全员广播。
        // trace=true：通用收帧行降 TRACE——本消息的链路日志由下方 [开火·请求]（INFO）+ FrameWriter
        // [射击判定]/[射击拒绝]（INFO）承担，INFO 档即整链可见
        disp_.on(protocol::MSG_C2S_FIRE, "开火",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            if (!sightline::wire::FireMsg::Parse(bytes)) {   // 非空 payload 拒收（带方向=宣判苗头，D5-01）
                LOG_WARNING("[开火] 非法 payload 长度=" + std::to_string(payload.size()) +
                            " fd=" + std::to_string(fd));
                return;
            }
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                             // 未登录：无效
            LOG_INFO("[开火·请求] shooter=" + std::to_string(pid) + " fd=" + std::to_string(fd));
            const auto nowMs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            writer_.deliverMatch(match_.Fire(static_cast<std::uint32_t>(pid), nowMs));
        }, true);

        // 14：开始对局——房主校验（D2-11 预留兑现）→ 名册纯数据过桥 → 用例 → 事件翻译
        disp_.on(protocol::MSG_C2S_START_MATCH, "开始对局",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            if (!sightline::wire::StartMatchMsg::Parse(bytes)) {   // v4：长度校验进 Parse（空 payload）
                LOG_WARNING("[开始对局] 非法 payload 长度=" + std::to_string(payload.size()) +
                            " fd=" + std::to_string(fd));
                return;
            }
            uint32_t roomId = 0;
            if (!lobby_.RoomIdByFd(fd, roomId)) {
                LOG_INFO("[开始对局] 拒绝：fd=" + std::to_string(fd) + " 不在房中");
                return;
            }
            if (!lobby_.IsHost(fd)) {
                LOG_INFO("[开始对局] 拒绝：fd=" + std::to_string(fd) + " 非房主");
                return;
            }
            std::vector<sightline::match::RosterEntry> entries;   // 名册序 = 出生点序
            for (const auto& m : lobby_.RoomRoster(roomId))
                entries.push_back({m.playerId, m.name, 0});
            writer_.deliverMatch(match_.StartMatch(roomId, std::move(entries)));
        });

        // 10 的对局侧钩子（仅对局离场；大厅离开逻辑仍在 wireLobby 的 10 号处理器里）
        disp_.on(protocol::MSG_C2S_LEAVE_ROOM, "对局离场",
                 [this](int fd, const std::string&) { HandleMemberGone(fd); });
    }

    // 断线编排：须在 lobby.Disconnect 之前调用（fd→PlayerId 翻译依赖 lobby 的映射仍在）
    void HandleDisconnect(int fd) { HandleMemberGone(fd); }

private:
    // fd → PlayerId 翻译 → 对局离场（不在局则空转）
    void HandleMemberGone(int fd) {
        const int pid = lobby_.PlayerIdByFd(fd);
        if (pid < 0) return;
        writer_.deliverMatch(match_.RemovePlayer(static_cast<uint32_t>(pid)));
    }

    MessageDispatcher& disp_;
    sightline::match::MatchService& match_;
    sightline::lobby::LobbyService& lobby_;
    FrameWriter& writer_;
};

} // namespace adapters
