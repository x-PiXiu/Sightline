// match_wiring.h —— 第 3 期对局编排（适配器，D3-15 的落地形态）
// 职责：双用例（Lobby/Match）互不认识——fd↔PlayerId 翻译、房主/名册查询的组合在此完成
// 注册顺序契约：Register() 须先于 wireLobby() 调用——msgid 10 的对局离场钩子要先于
//               大厅离开执行（fd→PlayerId 翻译必须发生在 lobby 移除映射之前）
#pragma once
#include <cstring>

#include "adapters/lobby_wiring.h"   // MessageDispatcher / FrameWriter / ConnTable
#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/match_service.h"
#include "protocol/msg_ids.h"

namespace adapters {

class MatchWiring {
public:
    MatchWiring(MessageDispatcher& disp, sightline::match::MatchService& match,
                sightline::lobby::LobbyService& lobby, FrameWriter& writer)
        : disp_(disp), match_(match), lobby_(lobby), writer_(writer) {}

    void Register() {
        // 16：位置上报——直发版只转发不校验（v3 契约规则 9）
        disp_.on(protocol::MSG_C2S_MOVE_REPORT, "位置上报",
                 [this](int fd, const std::string& payload) {
            if (payload.size() != 21) {                     // [12B pos][4B yaw][4B pitch][1B flags] = 21B
                LOG_WARNING("[位置上报] 非法长度=" + std::to_string(payload.size()) +
                            " fd=" + std::to_string(fd));
                return;
            }
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                            // 未登录/不在房：上报无效
            const auto rdF = [&payload](int O) {
                float F;
                std::memcpy(&F, payload.data() + O, sizeof(float));
                return F;
            };
            match_.UpdateEntity(static_cast<uint32_t>(pid),
                                rdF(0), rdF(4), rdF(8), rdF(12), rdF(16),
                                static_cast<uint8_t>(payload[20]));
        });

        // 14：开始对局——房主校验（D2-11 预留兑现）→ 名册纯数据过桥 → 用例 → 事件翻译
        disp_.on(protocol::MSG_C2S_START_MATCH, "开始对局",
                 [this](int fd, const std::string& payload) {
            if (!payload.empty()) {
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
