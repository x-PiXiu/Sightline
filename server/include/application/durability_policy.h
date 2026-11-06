// durability_policy.h —— ep08 步骤 2：纯策略（零 IO / 零时钟 / 零全局状态）
// §2.1 谦卑对象约束：sink 只按本策略给出的决策值搬运字节，不含业务判定分支。
// §5.3 定案：Must 在 sink 不可用或队列将满时 SpillToWal，落 AuditWal 即视为「不丢」，
// 落库允许延迟 —— 与 §5.5「三库全挂 = 对局照打」不再互斥。
#pragma once
#include <cstddef>

#include "application/ports.h"

namespace sightline::application {

// QueueState 已上移 ports.h（v1.6：IMatchArchive::State() 按值返回，完整定义须在端口头）。

struct DurabilityPolicy {
    static QueueAction Classify(const QueueState& s) {
        const bool full = (s.capacity != 0 && s.depth >= s.capacity);
        if (s.cls == Durability::Must) {
            if (!s.health.mysql || full) return QueueAction::SpillToWal;
            return QueueAction::Enqueue;
        }
        if (s.cls == Durability::BestEffort && full) return QueueAction::DropAndCount;
        return QueueAction::Enqueue;
    }

    static DegradeDecision Degrade(SinkHealth h) {
        const bool anyDown = !(h.mysql && h.mongo && h.redis);
        // 对局永远继续：三库全挂自动退化为 lite 语义（§5.7.2）。
        return DegradeDecision{/*continueMatch=*/true, /*warn=*/anyDown};
    }
};

} // namespace sightline::application
