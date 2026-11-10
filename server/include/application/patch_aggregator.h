// patch_aggregator.h —— ep09 步骤 4（G5·ADR-13）：patch 两阶段聚合器（用例层·主线程单写者）
// 语义：patch 广播到 N 份 VM（per-shard cmdQ）→各 VM 独立编译+不变式自测+引用原子换→
//       PatchResult 事件回流聚合——**全成功=收口成功；任一失败=二阶段回退**
//       （对全部 shard 重发 LastGoodSource=恢复点，由 IPatchChannel::LastGoodSource 提供）；
//       500ms 超时按失败处理（未回执 shard 状态未知→回退广播覆盖）。
// 幂等防护：收口中（Broadcasting/Reverting）的新 patch 命令拒收（Notice）。
// 纯逻辑类——广播/通知经 Sink 端口（orchestrator 注入），可脱离 IO 单测（test_patch_agg）。
#pragma once
#include <cstdint>
#include <string>

namespace sightline::application {

class PatchAggregator {
public:
    enum class Phase : std::uint8_t { Idle, Broadcasting, Reverting };

    /** 收口副作用端口（orchestrator 注入——广播走 cmdQ，通知走命令口 Notice） */
    struct Sink {
        virtual ~Sink() = default;
        virtual void BroadcastPatch(std::size_t shardIdx, const std::string& name,
                                    const std::string& source) = 0;
        virtual void Notice(const std::string& msg) = 0;
        /** 回退源（任一 shard 的通道取——加载初值/上次成功 patch 的源） */
        virtual std::string LastGoodSource(const std::string& name) = 0;
        virtual std::size_t ShardCount() const = 0;
    };

    static constexpr std::uint64_t kTimeoutMs = 500;   // ADR-13 收口上界

    explicit PatchAggregator(Sink& sink) : sink_(sink) {}

    /** 命令口 patch 入口：Idle 才受理（收口中拒收）；取回退源→广播 N 份→进入 Broadcasting */
    bool Begin(const std::string& name, std::uint64_t nowMs) {
        if (phase_ != Phase::Idle) {
            sink_.Notice("上一 patch 仍在收口中——请稍后重试");
            return false;
        }
        const std::size_t n = sink_.ShardCount();
        if (n == 0) {
            sink_.Notice("patch 拒收：无 patch 通道（HAS_LUA off）");
            return false;
        }
        name_ = name;
        goodSource_ = sink_.LastGoodSource(name);          // 回退恢复点（广播前取存）
        pending_ = n;
        failCount_ = 0;
        deadlineMs_ = nowMs + kTimeoutMs;
        phase_ = Phase::Broadcasting;
        for (std::size_t i = 0; i < n; ++i)
            sink_.BroadcastPatch(i, name, "");             // source 空=各 VM 自读 scriptDir（同源）
        return true;
    }

    /** PatchResult 事件回流（每 VM 一条）——pending 归零即收口判定 */
    void OnPatchResult(bool ok, std::uint64_t nowMs) {
        if (phase_ == Phase::Idle) return;                 // 野回执（超时后迟到的）——忽略
        if (pending_ > 0) --pending_;
        if (!ok) ++failCount_;
        Settle(nowMs);
    }

    /** 超时检查（orchestrator OnFrame 顺带调）——超时=未回执视同失败→回退广播覆盖 */
    void OnFrameTick(std::uint64_t nowMs) {
        if (phase_ == Phase::Idle) return;
        Settle(nowMs);
    }

    Phase phase() const { return phase_; }
    std::uint64_t FaultCount() const { return failCount_; }

private:
    /** 收口判定：pending==0 或超时——全成功收口；有失败进入 Reverting（二阶段广播回退源）；
     *  Reverting 阶段再收口（全 shard 确认旧源回写）→ 失败通知落定。 */
    void Settle(std::uint64_t nowMs) {
        const bool timeout = nowMs >= deadlineMs_;
        if (pending_ != 0 && !timeout) return;             // 未齐+未超时——继续等
        if (phase_ == Phase::Broadcasting) {
            if (failCount_ == 0 && !timeout) {
                sink_.Notice("patch 成功（" + std::to_string(sink_.ShardCount()) + "/" +
                             std::to_string(sink_.ShardCount()) + " VM 同步生效）");
                Reset_();
                return;
            }
            // 任一失败（或超时=状态未知）→ 二阶段：全部 shard 重发恢复点（成功者被拉回、失败者本就未换）
            phase_ = Phase::Reverting;
            pending_ = sink_.ShardCount();
            failCount_ = 0;
            deadlineMs_ = nowMs + kTimeoutMs;
            for (std::size_t i = 0; i < sink_.ShardCount(); ++i)
                sink_.BroadcastPatch(i, name_, goodSource_);   // 重发旧源=回退（Patch 幂等替换语义）
            sink_.Notice("patch 失败——正在回退全部 VM 至上一版（两阶段收口）");
            return;
        }
        if (phase_ == Phase::Reverting) {
            sink_.Notice("patch 失败——已回退（" + std::to_string(sink_.ShardCount() - failCount_) +
                         "/" + std::to_string(sink_.ShardCount()) + " VM 确认旧版）·审计留痕");
            Reset_();
        }
    }

    void Reset_() {
        phase_ = Phase::Idle;
        pending_ = 0;
        failCount_ = 0;
        name_.clear();
        goodSource_.clear();
    }

    Sink& sink_;
    Phase phase_ = Phase::Idle;
    std::string name_;
    std::string goodSource_;
    std::size_t pending_ = 0;
    std::size_t failCount_ = 0;
    std::uint64_t deadlineMs_ = 0;
};

} // namespace sightline::application
