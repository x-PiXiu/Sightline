// FrameCodec —— adapters 层：把 TCP 字节流切成 [2B len][2B msgid][payload] 完整帧
// 职责边界：net 层只管字节（Buffer），本层才知道什么是"消息"（换传输层时 net 不动）
// 半包处理：缓冲内不足一个完整帧时返回 false，留在缓冲等下次 read
#pragma once
#include <cstdint>
#include <string>
#include "net/buffer.h"

namespace adapters {

    struct Frame {
        uint16_t    msgid;
        std::string payload;
    };

    class FrameCodec {
    public:
        // 帧头：2B len + 2B msgid；len 为小端，= 2 + payload.size()
        static constexpr std::size_t kHeaderSize = 4;

        // 尝试从 buffer 头部切出一个完整帧
        // 成功：返回 true，frame 填充，buffer 消费对应字节
        // 半包：返回 false，buffer 不动
        // 非法帧（len < 2，无法定位 msgid）：丢弃全部已缓冲数据并返回 false——防坏流永久卡死切帧
        static bool tryDecode(net::Buffer& buffer, Frame& frame) {
            if (buffer.readable() < kHeaderSize) return false;

            const unsigned char* p = reinterpret_cast<const unsigned char*>(buffer.peek());
            uint16_t len = static_cast<uint16_t>(p[0] | (p[1] << 8));          // 小端
            if (len < 2) {
                buffer.clear();                                                // 坏流丢弃（防卡死）
                return false;
            }
            if (buffer.readable() < static_cast<std::size_t>(len) + 2) return false;

            buffer.consume(2);                                                 // 吃掉 len
            uint16_t msgid = static_cast<uint16_t>(buffer.peek()[0]
                                | (static_cast<uint16_t>(buffer.peek()[1]) << 8));
            buffer.consume(2);                                                 // 吃掉 msgid
            frame.msgid = msgid;
            frame.payload.assign(buffer.peek(), len - 2);
            buffer.consume(len - 2);
            return true;
        }

        // 打帧：msgid + payload → 附加 len 头 → 追加到 out
        static void encode(uint16_t msgid, const std::string& payload, std::string& out) {
            uint16_t len = static_cast<uint16_t>(2 + payload.size());
            out.push_back(static_cast<char>(len & 0xFF));
            out.push_back(static_cast<char>((len >> 8) & 0xFF));
            out.push_back(static_cast<char>(msgid & 0xFF));
            out.push_back(static_cast<char>((msgid >> 8) & 0xFF));
            out.append(payload);
        }
    };

} // namespace adapters
