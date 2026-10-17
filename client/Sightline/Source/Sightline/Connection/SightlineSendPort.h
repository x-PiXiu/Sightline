// SightlineSendPort —— 会话层的发送端口（D3-18 端口倒置）
// 手册 §4 DIP：边界 = 接口 + 实现，**接口归高层（会话）所有**，传输层实现——
// 会话只认端口，不认具体连接类；换传输形态（如 UDP 通道）= 换实现，会话零改动
#pragma once
#include "CoreMinimal.h"

class ISendPort {
public:
    virtual ~ISendPort() = default;
    virtual bool Send(uint16 MsgId, const TArray<uint8>& Payload) = 0;
};
