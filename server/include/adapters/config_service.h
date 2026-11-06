// config_service.h —— ep08 步骤 3：配置适配器（实现 5 个用例层端口）
// 整洁架构：atomic/shared_ptr 等并发机制只住适配器；GameConfig（实体层）是纯值对象 + Validate()。
// 同一个对象被组合根注入 IConfigSource / ICombatParams / IBotTuning / IConfigVersion / IMatchRules
// 五个接口位（ISP·G9——v1.6 补 IMatchRules：deathmatch/rewind 此前零消费者）。
// 实现偏差记录：数据表用零依赖的轻量 Lua 表解析器（见 config_service.cpp），Lua VM 留给步骤 4 的
// 公式 / 决策热修——数据表本就不需要 VM，也让 lite 模式零外部依赖可编译。
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "application/config_views.h"
#include "match/game_config.h"

namespace adapters {

class ConfigService final : public sightline::application::IConfigSource,
                            public sightline::application::ICombatParams,
                            public sightline::application::IBotTuning,
                            public sightline::application::IConfigVersion,
                            public sightline::application::IMatchRules {
public:
    explicit ConfigService(std::string path);

    // ---- IConfigSource ----
    sightline::application::ReloadResult Reload(std::string_view note) override;
    void Rollback() override;

    // ---- 只读视图（一律按值返回，避免原子替换后引用悬垂）----
    std::uint16_t MaxHp() const override;
    sightline::match::WeaponParams Weapon(sightline::match::EWeaponType t) const override;
    sightline::match::FormulaParams Formula() const override;
    sightline::match::BotTuning Get() const override;
    std::string Version() const override;                    // v1.6 收口④：按值返回
    sightline::application::ConfigWireSnapshot Snapshot() const override;
    sightline::match::DeathmatchRules Deathmatch() const override;   // v1.6 收口③
    sightline::match::RewindRules Rewind() const override;

    // G4：ConfigService 不直调 persistence，只发域事件，由编排器订阅后写审计。
    using ChangeCallback = std::function<void(const sightline::application::ConfigChanged&)>;
    void Subscribe(ChangeCallback cb) { onChanged_ = std::move(cb); }

    std::string StatusLine() const;   // version 命令输出
    std::size_t HistorySize() const { return history_.size(); }

private:
    struct SnapshotEntry {
        std::shared_ptr<const sightline::match::GameConfig> cfg;
        std::string version;
        std::string note;
        std::string digest;
        std::uint64_t atUtcMs = 0;
    };

    static bool ParseFile(const std::string& path, sightline::match::GameConfig& out,
                          std::string& digest, std::string& err);
    // publish=false 用于构造期启动加载：此时组合根还没订阅，发域事件只会丢。
    sightline::application::ReloadResult LoadSnapshot(std::string_view note, bool publish);
    void Publish(const SnapshotEntry& e, std::string_view note);
    static std::uint64_t UtcNowMs();

    std::string path_;
    std::atomic<std::shared_ptr<const sightline::match::GameConfig>> cfg_;
    std::string version_;                 // 主线程读写；Version() 按值返回（v1.6：不再返回指向它的 view）
    std::string digest_;
    std::deque<SnapshotEntry> history_;   // 最近 4 版内存快照环（§3.4）
    ChangeCallback onChanged_;
    std::uint32_t session_ = 0;
};

} // namespace adapters
