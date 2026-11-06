// wire_sink.h —— ep08 步骤 3：把既有 FrameWriter 适配成用例层 IWireSink 端口
// 用例层不认识 FrameWriter；这一层只做 span → vector 的形状转换（谦卑对象）。
// BroadcastConfig（v1.6 收口⑥）：快照由编排器传入（用例定时机与内容），本层只编码下发——
// 端口入参不再被丢弃，FrameWriter 不再自持 IConfigVersion*（第二入口已灭）。
#pragma once
#include <span>
#include <vector>

#include "adapters/lobby_wiring.h"
#include "application/ports.h"

namespace adapters {

class WireSinkAdapter final : public sightline::application::IWireSink {
public:
    explicit WireSinkAdapter(FrameWriter& writer) : writer_(writer) {}

    void DeliverMatch(std::span<const sightline::application::MatchEvent> es) override {
        std::vector<sightline::application::MatchEvent> v(es.begin(), es.end());
        writer_.deliverMatch(v);
    }

    void BroadcastConfig(const sightline::application::ConfigWireSnapshot& snap) override {
        writer_.BroadcastConfigVersion(snap);   // 入参真正被用（v1.6：不再忽略快照另取视图）
    }

private:
    FrameWriter& writer_;
};

} // namespace adapters
