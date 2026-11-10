// match_wiring.h —— 第 3 期对局编排（适配器，D3-15 的落地形态）
// 职责：双用例（Lobby/Match）互不认识——fd↔PlayerId 翻译、房主/名册查询的组合在此完成
// 注册顺序契约：Register() 须先于 wireLobby() 调用——msgid 10 的对局离场钩子要先于
//               大厅离开执行（fd→PlayerId 翻译必须发生在 lobby 移除映射之前）
#pragma once
#include <chrono>
#include <cstring>
#include <functional>

#include "adapters/lobby_wiring.h"   // MessageDispatcher / FrameWriter / ConnTable
#include "lobby/lobby.h"
#include "logger/logger.h"
#include "match/match_session.h"
#include "match/match_shards.h"
#include "protocol/messages.h"   // v4 wire 结构体（variant 编解码——业务层禁 include，红线见 messages.h）
#include "protocol/msg_ids.h"
#include "adapters/bot_driver.h"

namespace adapters {

class MatchWiring {
public:
    // 时钟注入（ADR-1 单钟·2026-11-04 双钟复盘）：适配器不得自带时钟——main 以 nowMs 注入，与
    // Tick/人机驱动/RTT 完全同源。此前 19/20 号处理器内联 steady_clock，而 main/Tick 用 system_clock，
    // 两个时钟数值差 N 小时 → ①玩家击杀 respawnAtMs(steady) 对 Tick(system) 恒"已过期"=同毫秒
    // KILL+RESPAWN（受害者瞬移复活=客户端"人机拉扯"）②玩家开火 judgeAt(steady) 采样人机历史
    // (system 打戳) 恒钳最老条目=永远打在 ~1 秒前的人机身上（垂距 300~750cm=400cm/s×1s）。
    // 默认回退 system_clock（与 main.nowMs 同款）防裸构造漏注入。
    MatchWiring(MessageDispatcher& disp, sightline::match::MatchShards& shards,
                sightline::lobby::LobbyService& lobby, FrameWriter& writer,
                BotDriver* bot = nullptr,
                std::function<std::uint64_t()> now = nullptr)
        : disp_(disp), shards_(shards), lobby_(lobby), writer_(writer), bot_(bot), now_(std::move(now)) {}

    // ep08 步骤 3（B1 修复）：Fire 是第三处事件源。注入后由编排器统一扇出，
    // MatchWiring 不认识编排器类型（只有 std::function），也不直接依赖任何具体 sink；
    // 未注入时保持 ep07 原路径，238 断言与既有行为零变化。
    using MatchEventSink = std::function<void(std::vector<sightline::match::MatchEvent>&&)>;
    void SetMatchEventSink(MatchEventSink cb) { eventSink_ = std::move(cb); }

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
            const auto nowMs = Now_();                       // 注入钟（ADR-1 单钟）——历史环打戳与 Tick/回溯同源
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                            // 未登录/不在房：上报无效
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::Move;
            in.atMs = nowMs;                                 // ep06：接收时刻入历史环（单钟）
            in.x = msg->x; in.y = msg->y; in.z = msg->z;
            in.yaw = msg->yaw; in.pitch = msg->pitch;
            in.flags = msg->flags; in.seq = msg->seq;
            (void)shards_.Route(static_cast<uint32_t>(pid), std::move(in));   // ep09：投递化（drain 层消费）
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
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::Rtt;
            in.rttMs = rd16(0);
            (void)shards_.Route(static_cast<std::uint32_t>(pid), std::move(in));
            // up/down KB/s（offset 2/4）：ep12 带宽统计消费——本期收下不存
        });

        // 16：v4 R10 对局期停用——在局者收到 16 视为无效帧丢弃（19 替代）；对局外 16 本就无效（未知 pid 静默）
        disp_.on(protocol::MSG_C2S_MOVE_REPORT, "位置上报(v4停用)",
                 [this](int fd, const std::string&) {
            const int pid = lobby_.PlayerIdByFd(fd);
            std::uint32_t rid = 0, ep = 0;
            const auto* session = pid >= 0 ? shards_.Find(
                (lobLocate_(static_cast<std::uint32_t>(pid), rid, ep), rid)) : nullptr;
            if (session && session->IsActive(static_cast<std::uint32_t>(pid))) {
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
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::Fire;
            in.atMs = Now_();                                // 注入钟（ADR-1 单钟·judgeAt 采样源）
            (void)shards_.Route(static_cast<std::uint32_t>(pid), std::move(in));   // ep09：投递化
            // Fire 事件经 DrainAndApply 产出→编排器统一扇出（B1 三源统一点不变）
        }, true);

        // 29：自主换弹（2026-11-05·空 payload）——换弹钟权威在服务器；静默布尔落 INFO 日志
        disp_.on(protocol::MSG_C2S_RELOAD, "自主换弹",
                 [this](int fd, const std::string& payload) {
            if (!payload.empty()) {
                LOG_WARNING("[换弹] 非法 payload 长度=" + std::to_string(payload.size()) + " fd=" + std::to_string(fd));
                return;
            }
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;                             // 未登录：无效
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::Reload;
            in.atMs = Now_();
            if (shards_.Route(static_cast<std::uint32_t>(pid), std::move(in)))
                LOG_INFO("[换弹·请求] shooter=" + std::to_string(pid));
        });

        // 28：换枪意图（v4.8）——1B weaponType；服务器权威状态 + 配置表校验，20 号仍为空包
        disp_.on(protocol::MSG_C2S_SWITCH_WEAPON, "换枪",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            const auto msg = sightline::wire::SwitchWeaponMsg::Parse(bytes);
            if (!msg) {
                LOG_WARNING("[换枪] 非法 payload 长度=" + std::to_string(payload.size()) +
                            " fd=" + std::to_string(fd));
                return;
            }
            const int pid = lobby_.PlayerIdByFd(fd);
            if (pid < 0) return;
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::SwitchWeapon;
            in.atMs = Now_();
            in.flags = msg->weaponType;                      // weaponType 复用 flags 字节位（1B 载荷）
            (void)shards_.Route(static_cast<std::uint32_t>(pid), std::move(in));
            // 拒绝原因（why 出参）随投递化归 drain 层静默——28 号本就"被拒无回执"，日志降级可接受
        });

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
            std::vector<sightline::match::RosterEntry> entries;   // 名册（含 ep07 人机席位·排尾）；出生点序=名册序由用例 StartMatch 分配
            for (const auto& m : lobby_.RoomRoster(roomId))
                entries.push_back({m.playerId, m.name, 0});
            lobby_.SetRoomInMatch(roomId, true);            // ep07：对局态过桥（对局中拒 JOIN 码5·❗11）
            // v1.6 收口⑥：StartMatch 事件走同一订阅点（编排器在此触发 15 号后 30 号广播
            // + MatchStart 进归档流=战报开档）；未注入回调 = ep07 原路径。
            auto& session = shards_.OpenRoom(roomId);   // ep09：无则建+注入（幂等）
            {
                auto evs = session.StartMatch(roomId, entries);
                if (eventSink_) eventSink_(std::move(evs));
                else writer_.deliverMatch(evs);
            }
            // 活动区数据校验（数据问题数据入口拦——与"掩体压出生点"同族纪律）：出生点在合同地板
            // 之外 → 域已钳入界内（锚/24 号同源），但请挪回地板重烘焙，否则重生/开局仍在钳制边缘
            if (const auto& g = session.Geometry(); !g.spawnPoints.empty())
                for (std::size_t i = 0; i < g.spawnPoints.size(); ++i) {
                    const auto& sp = g.spawnPoints[i];
                    if (sp.x < session.PlayMinX() || sp.x > session.PlayMaxX() ||
                        sp.y < session.PlayMinY() || sp.y > session.PlayMaxY())
                        LOG_WARNING("[活动区] 出生点 " + std::to_string(i) + " (" +
                                    std::to_string(sp.x) + "," + std::to_string(sp.y) +
                                    ") 在地板合同之外——已钳入界内；请挪回地板并重烘焙");
                }
            // 人机首报读用例分配结果（Entity::spawnIx）——不传本地 entries：值传递副本里 spawnIndex 全 0
            if (bot_) bot_->OnMatchStart(roomId, session.Geometry(), session, Now_());
        });
        // 26/27：人机席位指令（仅房主——D2-11 同款校验线）
        disp_.on(protocol::MSG_C2S_BOT_ADD, "添加人机",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            const auto msg = sightline::wire::BotAddMsg::Parse(bytes);
            if (!msg) {
                LOG_WARNING("[人机] 添加拒绝（载荷长度非法 len=" + std::to_string(payload.size()) + "）fd=" + std::to_string(fd));
                return;
            }
            uint32_t roomId = 0;
            if (!lobby_.RoomIdByFd(fd, roomId) || !lobby_.IsHost(fd)) {
                LOG_WARNING("[人机] 添加拒绝（不在房间/非房主）fd=" + std::to_string(fd));
                return;
            }
            const int pid = lobby_.AddBot(roomId);
            if (pid < 0) {
                LOG_WARNING("[人机] 添加失败（房间满/对局中）房=" + std::to_string(roomId));
                return;
            }
            // 人机真名从名册取（Bot1/Bot2…——AddBot 带房间内序号）——driver 登记名与名册一致
            std::string botName = "Bot";
            for (const auto& m : lobby_.RoomRoster(roomId))
                if (m.playerId == static_cast<std::uint32_t>(pid)) { botName = m.name; break; }
            if (bot_) bot_->OnBotAdded(roomId, static_cast<std::uint32_t>(pid), botName, msg->difficulty);
            writer_.deliver(lobby_.RoomStateAndList(roomId));
            LOG_INFO("[人机] 已添加 房=" + std::to_string(roomId) + " pid=" + std::to_string(pid));
        });
        disp_.on(protocol::MSG_C2S_BOT_REMOVE, "移除人机",
                 [this](int fd, const std::string& payload) {
            const std::span<const std::uint8_t> bytes(
                reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());
            const auto msg = sightline::wire::BotRemoveMsg::Parse(bytes);
            if (!msg) {
                LOG_WARNING("[人机] 移除拒绝（载荷长度非法 len=" + std::to_string(payload.size()) + "）fd=" + std::to_string(fd));
                return;
            }
            uint32_t roomId = 0;
            if (!lobby_.RoomIdByFd(fd, roomId) || !lobby_.IsHost(fd)) {
                LOG_WARNING("[人机] 移除拒绝（不在房间/非房主）fd=" + std::to_string(fd));
                return;
            }
            if (!lobby_.IsBot(msg->botPid)) {
                LOG_WARNING("[人机] 移除拒绝（pid=" + std::to_string(msg->botPid) + " 不是人机）");
                return;
            }
            writer_.deliver(lobby_.RemoveBot(msg->botPid));
            if (bot_) bot_->RemoveBot(msg->botPid);
            LOG_INFO("[人机] 已移除 pid=" + std::to_string(msg->botPid));
        });

        // 10 的对局侧钩子（仅对局离场；大厅离开逻辑仍在 wireLobby 的 10 号处理器里）
        disp_.on(protocol::MSG_C2S_LEAVE_ROOM, "对局离场",
                 [this](int fd, const std::string&) { HandleMemberGone(fd); });
    }

    // 断线编排：须在 lobby.Disconnect 之前调用（fd→PlayerId 翻译依赖 lobby 的映射仍在）
    void HandleDisconnect(int fd) { HandleMemberGone(fd); }

private:
    // 注入钟（ADR-1 单钟）：main 以 nowMs（system_clock 墙钟）注入；空=回退 system_clock 同款
    std::uint64_t Now_() const {
        if (now_) return now_();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }
    /** ep09 步骤 2：快照定位（16 号日志用）——正式路由已投递化（Route） */
    bool lobLocate_(std::uint32_t pid, std::uint32_t& roomId, std::uint32_t& epoch) {
        return lobby_.LocateFromSnapshot(pid, roomId, epoch);
    }
    // fd → PlayerId 翻译 → 对局离场（不在局则空转）
    void HandleMemberGone(int fd) {
        const int pid = lobby_.PlayerIdByFd(fd);
        if (pid < 0) return;
        // v1.6：离场事件同走订阅点（EntityLeave 进归档流=战报完整性）；未注入=原路径
        {
            sightline::match::MatchInput in;
            in.kind = sightline::match::MatchInput::Kind::RemovePlayer;   // 数据面 FIFO 保序（G1 配套）
            (void)shards_.Route(static_cast<uint32_t>(pid), std::move(in));
            // 离场事件经 DrainAndApply 产出→编排器统一扇出（EntityLeave 进归档流不变）
        }
    }

    MessageDispatcher& disp_;
    sightline::match::MatchShards& shards_;   // ep09：多房分桶（路由化直调点）
    sightline::lobby::LobbyService& lobby_;
    FrameWriter& writer_;
    BotDriver* bot_ = nullptr;   // ep07：人机驱动（可空——纯真人局）；声明序=构造初始化序（-Wreorder）
    std::function<std::uint64_t()> now_;   // 注入钟（ADR-1 单钟·2026-11-04 双钟复盘）——空=回退 system
    MatchEventSink eventSink_;   // v1.6：Fire/StartMatch/RemovePlayer 三源统一点（可空=保持原路径）
};

} // namespace adapters
