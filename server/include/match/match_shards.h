// match_shards.h —— ep09 多房分桶容器（用例层）：房→session 的唯一权威
// v1 = SerialDispatcher 形态（草案 §4.11）：单线程顺序 TickAll——Shard 结构/绑定哈希先立住，
//      步骤 3 的 worker 池换 ITickDispatcher 实现，本结构不变（可插拔的推迟决策）。
// 幂等（G2·Serial 简化形）：MatchSession::StartMatch 本就是全量复位（entities_.clear()+终局
//      三件套复位，2026-11-03 联调根因修复）——「不存在建 / 存在重置」与常驻单局时代行为零变化；
//      unique_ptr 替换形留给 worker 版（所有权不跨线程时才需要）。
#pragma once
#include "match/match_session.h"
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sightline::match {

/** 开房共享依赖（构造注入·DIP）：session 创建时的只读共享件 + 初始配置闭包。
 *  geometry/damage 为 nullptr 时保持既有兼容路径（空表/内建策略——238 断言未注入行为）。 */
struct ShardDeps {
    const MapGeometry* geometry = nullptr;
    IDamagePolicy*     damage   = nullptr;
    std::function<GameConfig()> currentConfig;   // 新 session 初始规则快照（main 组装=ApplyConfig 同源）
};

class MatchShards {
public:
    static constexpr std::size_t kDefaultShardCount = 4;   // P1 待批示默认值（worker 数=分桶数）

    explicit MatchShards(ShardDeps deps, std::size_t n = kDefaultShardCount)
        : deps_(std::move(deps)), shards_(n ? n : 1) {}

    /** 开房（确保存在+注入）：无则建（seed=roomId 派生·ADR-23 + Deps 注入），有则幂等返回。
     *  **不自动 StartMatch**——开局时机/名册/事件流归调用方（wiring 显式 StartMatch，
     *  与常驻单局时代语义完全同构；G2 幂等重置由 StartMatch 全量复位承担） */
    MatchSession& OpenRoom(std::uint32_t roomId) {
        auto& sh = shards_[roomId % shards_.size()];
        auto it = sh.rooms.find(roomId);
        if (it == sh.rooms.end()) {
            auto s = std::make_unique<MatchSession>(DeriveSeed(roomId));
            if (deps_.geometry) s->SetGeometry(*deps_.geometry);
            if (deps_.damage)   s->SetDamagePolicy(deps_.damage);
            if (deps_.currentConfig) s->SetConfig(deps_.currentConfig());
            it = sh.rooms.emplace(roomId, std::move(s)).first;
        }
        return *it->second;
    }

    /** 路由查（wiring/bot 用）——房间从未开局返回 nullptr */
    MatchSession* Find(std::uint32_t roomId) {
        auto& sh = shards_[roomId % shards_.size()];
        auto it = sh.rooms.find(roomId);
        return it == sh.rooms.end() ? nullptr : it->second.get();
    }

    /** 全 shard 顺序 Tick（Serial）——事件合流（每条自带 roomId，Fanout 按房分发零改动） */
    std::vector<MatchEvent> TickAll(std::uint64_t nowMs) {
        std::vector<MatchEvent> out;
        for (auto& sh : shards_)
            for (auto& [rid, s] : sh.rooms) {
                auto evs = s->Tick(nowMs);
                out.insert(out.end(), std::make_move_iterator(evs.begin()),
                           std::make_move_iterator(evs.end()));
            }
        return out;
    }

    /** 配置热重载传播（orchestrator ApplyConfig 调）——所有已建 session 逐个 SetConfig */
    void ApplyConfigAll(const GameConfig& c) {
        for (auto& sh : shards_)
            for (auto& [rid, s] : sh.rooms) s->SetConfig(c);
    }

    /** 启动期共享件注入（main 装配序：geometry 装载段/策略分支晚于容器声明——
     *  延迟到 setter；OpenRoom 全部发生在 wiring 运行期，晚于此两调，无竞态） */
    void SetSharedGeometry(const MapGeometry* g) { deps_.geometry = g; }
    void SetSharedDamage(IDamagePolicy* p) { deps_.damage = p; }

    std::size_t ActiveRooms() const {
        std::size_t n = 0;
        for (const auto& sh : shards_) n += sh.rooms.size();
        return n;
    }

private:
    /** ADR-23：seed=roomId 派生（乘法混淆防相邻房种子线性相关） */
    static std::uint32_t DeriveSeed(std::uint32_t roomId) {
        return roomId * 2654435761u ^ 0x9E3779B9u;
    }
    struct Shard { std::unordered_map<std::uint32_t, std::unique_ptr<MatchSession>> rooms; };
    ShardDeps deps_;
    std::vector<Shard> shards_;
};

} // namespace sightline::match
