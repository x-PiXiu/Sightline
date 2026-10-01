// 协议消息枚举 —— 与 docs/protocol/MSG_IDS.md 保持同步（服务端 ⇆ UE 共同契约）
// 帧格式：[2B len][2B msgid][payload]，小端；len = 2 + payload.size()
#pragma once
#include <cstdint>

namespace protocol {

    enum MsgId : uint16_t {
        MSG_S2C_WELCOME        = 1,  // S→C  [8B serverNowMs]
        MSG_C2S_HEARTBEAT      = 2,  // C→S  [4B seq][8B clientTs]
        MSG_S2C_HEARTBEAT_ACK  = 3,  // S→C  [4B seq][8B clientTs][8B serverTs]
    };

} // namespace protocol
