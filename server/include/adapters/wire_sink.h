// wire_sink.h —— ep08 步骤 3：把既有 FrameWriter 适配成用例层 IWireSink 端口
// 用例层不认识 FrameWriter；这一层只做 span → vector 的形状转换（谦卑对象）。
// BroadcastConfig 在步骤 6 契约 v4.8 落地时由本类实现 30 号广播。
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

    // 30 号 S2C_CONFIG_VERSION 广播：编排器只发意图，载荷组装留在 FrameWriter（G5）
    void BroadcastConfig(const sightline::application::ConfigWireSnapshot&) override {
        writer_.BroadcastConfigVersion();
    }

private:
    FrameWriter& writer_;
};

} // namespace adapters
