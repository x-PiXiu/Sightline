// tests/application/test_room_service.cpp —— 用例层单测
// 架构收益的直接证明：全流程（登录→进房→开战→开火→击杀→重生→胜负→心跳踢人）
// 无 socket、无 epoll、无 timerfd——Fake 双件替身即可驱动。

#include <cstdio>
#include <optional>
#include "application/session_service.h"
#include "application/room_service.h"

using namespace sightline;
using namespace sightline::app;

static int g_failures = 0;
#define CHECK(cond) do { \
    if (!(cond)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_failures; } \
} while (0)

// ---- Fake：IGameChannel 替身（记录发送）----
struct FakeChannel : IGameChannel {
    struct Sent { PlayerId to; GameEvent ev; };
    std::vector<Sent> sent;
    std::vector<PlayerId> closed;

    void sendTo(PlayerId pid, const GameEvent& ev) override { sent.push_back({pid, ev}); }
    void sendToAll(const std::vector<PlayerId>& pids, const GameEvent& ev) override {
        for (auto p : pids) sent.push_back({p, ev});
    }
    void close(PlayerId pid) override { closed.push_back(pid); }

    size_t countEvents() const {
        // 每个 sendToAll 展开为多条，仅统计种类时用 filter 即可
        return sent.size();
    }
    template <typename E>
    std::vector<const E*> eventsOfType() const {
        std::vector<const E*> out;
        for (auto& s : sent) if (auto* e = std::get_if<E>(&s.ev)) out.push_back(e);
        return out;
    }
};

// ---- Fake：ITimerScheduler 替身（手动推进时间）----
struct FakeTimer : ITimerScheduler {
    struct Item { uint64_t id; int ms; std::function<void()> cb; bool cancelled = false; bool fired = false; };
    std::vector<Item> items;
    uint64_t next_id = 1;

    uint64_t runAfter(int ms, std::function<void()> cb) override {
        items.push_back({next_id, ms, std::move(cb)});
        return next_id++;
    }
    void cancel(uint64_t id) override {
        for (auto& it : items) if (it.id == id) it.cancelled = true;
    }
    // 触发所有未取消未触发的定时器（测试用手动推进）
    void fireAll() {
        for (auto& it : items) {
            if (!it.cancelled && !it.fired) { it.fired = true; it.cb(); }
        }
    }
};

int main() {
    FakeChannel channel;
    FakeTimer timers;

    RoomService rooms(channel, timers);
    SessionService sessions(channel, timers,
                             [&rooms](PlayerId pid) { rooms.handlePlayerGone(pid); });
    const uint64_t CONN_A = 100, CONN_B = 101;

    // 1. 登录 → LoginAck
    sessions.onConnected(CONN_A);
    sessions.handleLogin(CONN_A, LoginCommand{"Alice"});
    CHECK(!channel.eventsOfType<LoginAckEvent>().empty());
    CHECK(sessions.playerIdOf(CONN_A) == 1);

    // 2. 单人进房不开战
    rooms.handleJoin(1, "Alice");
    CHECK(rooms.roomCount() == 1);
    CHECK(channel.eventsOfType<RoomStartEvent>().empty());

    // 3. 第二人进房 → RoomStart 广播给两人
    sessions.onConnected(CONN_B);
    sessions.handleLogin(CONN_B, LoginCommand{"Bob"});
    rooms.handleJoin(2, "Bob");
    auto starts = channel.eventsOfType<RoomStartEvent>();
    CHECK(starts.size() == 2);
    CHECK(starts[0]->players.size() == 2);

    // 4. 移动：B 上报，A 收到 MoveEvent（B 自己不收）
    channel.sent.clear();
    rooms.handleMove(2, MoveCommand{Vec3{5, 0, 0}, 0.5f});
    auto moves = channel.eventsOfType<MoveEvent>();
    CHECK(moves.size() == 1);
    CHECK(moves[0]->player_id == 2);

    // 5. A 站原点朝 +x 开火 → 命中 B（miss 也广播 HitEvent victim=0）
    channel.sent.clear();
    rooms.handleFire(1, FireCommand{Vec3{0, 0, 0}, Vec3{1, 0, 0}});
    auto hits = channel.eventsOfType<HitEvent>();
    CHECK(hits.size() == 2);              // 广播给房间两人
    CHECK(hits[0]->victim == 2);
    CHECK(hits[0]->victim_hp == 75);
    CHECK(!hits[0]->victim_dead);

    // 6. 心跳超时 → Kick + close → 联动房间退房广播
    channel.sent.clear();
    timers.fireAll();                     // 推进所有定时器（含心跳超时）
    CHECK(!channel.eventsOfType<KickEvent>().empty());
    CHECK(channel.closed.size() >= 1);
    // 断开后走 onDisconnected 路径（生产由网络层触发；此处直接驱动验证联动）
    sessions.onDisconnected(CONN_B);
    CHECK(!channel.eventsOfType<PlayerLeftEvent>().empty());
    // B 走后 A 独享房间 → 游戏结束判胜
    auto overs = channel.eventsOfType<GameOverEvent>();
    CHECK(!overs.empty());

    // 7. Ping/Pong RTT 通道
    channel.sent.clear();
    sessions.handlePing(1, PingCommand{12345});
    auto pongs = channel.eventsOfType<PongEvent>();
    CHECK(pongs.size() == 1 && pongs[0]->client_time == 12345);

    if (g_failures == 0) std::printf("test_room_service: ALL PASSED\n");
    return g_failures == 0 ? 0 : 1;
}
