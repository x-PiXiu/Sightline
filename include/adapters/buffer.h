// adapters/buffer.h —— 应用层读写缓冲（muduo Buffer 的精简实现）
// 职责：吸收 TCP 流式语义（粘包/半包），让上层按"消息"思考。
// 纯字节容器，不含任何业务知识（谦卑对象）。

#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/uio.h>
#include <cerrno>

namespace sightline::adapters {

class Buffer {
public:
    static constexpr size_t kPrepend = 8;     // 前置预留（写长度前缀时可免搬移）
    static constexpr size_t kInitSize = 1024;

    Buffer() : buf_(kPrepend + kInitSize), read_idx_(kPrepend), write_idx_(kPrepend) {}

    size_t readableBytes() const { return write_idx_ - read_idx_; }
    size_t writableBytes() const { return buf_.size() - write_idx_; }
    const uint8_t* peek() const { return begin() + read_idx_; }

    void retrieve(size_t n) {
        if (n < readableBytes()) read_idx_ += n;
        else retrieveAll();
    }
    void retrieveAll() { read_idx_ = write_idx_ = kPrepend; }

    void append(const void* data, size_t len) {
        ensureWritable(len);
        std::memcpy(beginWrite(), data, len);
        write_idx_ += len;
    }
    void append(const std::string& s) { append(s.data(), s.size()); }

    // 从 fd 读取（配合 ET：调用方循环调到返回 0 / EAGAIN）
    // 返回值：>0 本次读到的字节数；0 无数据(EAGAIN)；-1 错误（errno 已存 saved_errno）
    ssize_t readFd(int fd, int& saved_errno) {
        uint8_t extrabuf[65536];
        struct iovec vec[2];
        const size_t writable = writableBytes();
        vec[0].iov_base = beginWrite();
        vec[0].iov_len = writable;
        vec[1].iov_base = extrabuf;
        vec[1].iov_len = sizeof extrabuf;
        const int iovcnt = (writable < sizeof extrabuf) ? 2 : 1;
        ssize_t n = ::readv(fd, vec, iovcnt);
        if (n > 0) {
            if (static_cast<size_t>(n) <= writable) {
                write_idx_ += n;
            } else {
                write_idx_ = buf_.size();
                append(extrabuf, n - writable);
            }
        } else {
            saved_errno = errno;
        }
        return n;
    }

private:
    uint8_t* begin() { return buf_.data(); }
    const uint8_t* begin() const { return buf_.data(); }
    uint8_t* beginWrite() { return begin() + write_idx_; }

    void ensureWritable(size_t len) {
        if (writableBytes() < len) makeSpace(len);
    }

    void makeSpace(size_t len) {
        if (writableBytes() + read_idx_ - kPrepend < len) {
            buf_.resize(write_idx_ + len);
        } else {
            // 前部有已读空间：把未读数据搬到前面
            size_t readable = readableBytes();
            std::memmove(begin() + kPrepend, begin() + read_idx_, readable);
            read_idx_ = kPrepend;
            write_idx_ = read_idx_ + readable;
        }
    }

    std::vector<uint8_t> buf_;
    size_t read_idx_;
    size_t write_idx_;
};

} // namespace sightline::adapters
