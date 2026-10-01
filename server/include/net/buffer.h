// 最小字节缓冲——第 1 期最简语义版：append / peek / readable / consume
// 设计取舍：双索引（read/write）+ writev 集中写是演进项（回包堆积出现时再启用），
// 当前规模（心跳包 <100B）erase 前缀的开销可忽略——语义直白零坑优先
#pragma once
#include <cstddef>
#include <vector>

namespace net {

    class Buffer {
    public:
        void append(const char* data, std::size_t n) {
            buf_.insert(buf_.end(), data, data + n);
        }

        const char* peek() const { return buf_.data(); }
        std::size_t readable() const { return buf_.size(); }

        void consume(std::size_t n) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(n));
        }

        void clear() { buf_.clear(); }

    private:
        std::vector<char> buf_;
    };

} // namespace net
