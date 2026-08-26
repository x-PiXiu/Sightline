// application/ports/i_game_channel.h —— Port #1：网络出口
// 接口归用例层所有（依赖倒置）：用例只认这个接口，
// 实现在 adapters/GameServer（编码为字节 → TcpConnection 发送）。
// 这一个接口同时覆盖"发送"与"断开"，是应用层看到的网络世界的全部。

#pragma once
#include <vector>
#include "application/dto.h"
#include "domain/types.h"

namespace sightline::app {

class IGameChannel {
public:
    virtual ~IGameChannel() = default;

    // 定向发送（事件由适配器的 codec 编码为字节流）
    virtual void sendTo(PlayerId pid, const GameEvent& ev) = 0;

    // 群发给指定列表（房间成员由用例从 Room 取得——发送名单是业务知识，留在内层）
    virtual void sendToAll(const std::vector<PlayerId>& pids, const GameEvent& ev) = 0;

    // 主动断开（心跳超时踢人）
    virtual void close(PlayerId pid) = 0;
};

} // namespace sightline::app
