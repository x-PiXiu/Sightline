// net/event_loop_thread_pool.h —— 主从 Reactor 的 IO 线程池
// 拓扑：主 loop 跑 Acceptor（accept），N 个 IO loop 各自跑一批连接的读写；
//      新连接 round-robin 分发到 IO loop。
// 线程契约（整洁架构的并发决策）：IO 多线程只做"搬运字节"；
//      游戏逻辑单线程——上层（GameServer）把消息从 IO 线程跳回主 loop 分发，
//      服务层因此零锁、零改动。

#pragma once
#include <future>
#include <memory>
#include <thread>
#include <vector>
#include "net/event_loop.h"
#include "logger/logger.h"

namespace common {
    namespace network {

// 单个 IO 线程：线程内创建并运行一个 EventLoop（栈对象，随线程生死）
class EventLoopThread {
public:
    EventLoopThread() = default;
    ~EventLoopThread() { stop(); }

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    // 启动线程并拿到该线程内 EventLoop 的指针（future 保证 happens-before）
    EventLoop* startLoop() {
        std::promise<EventLoop*> promise;
        auto future = promise.get_future();
        thread_ = std::thread([this, &promise] {
            EventLoop loop;              // 在本线程构造：threadId_ 天然正确
            loop_ = &loop;
            promise.set_value(&loop);
            LOG_INFO("IO loop thread started");   // 多 Reactor 拓扑自证：启动时可见各线程 ID
            loop.loop();                 // 阻塞直到 quit()
        });
        return future.get();
    }

    void stop() {
        if (thread_.joinable()) {
            if (loop_) {
                loop_->quit();           // 跨线程 quit：内部写 eventfd 唤醒
            }
            thread_.join();
        }
    }

private:
    std::thread thread_;
    EventLoop* loop_ = nullptr;
};

// IO 线程池：num=0 时退化为单 Reactor（getNextLoop 返回主 loop）
class EventLoopThreadPool {
public:
    EventLoopThreadPool(EventLoop* base_loop, int num_threads)
        : base_loop_(base_loop), num_threads_(num_threads > 0 ? num_threads : 0) {}

    ~EventLoopThreadPool() { stop(); }

    void start() {
        for (int i = 0; i < num_threads_; ++i) {
            auto t = std::make_unique<EventLoopThread>();
            loops_.push_back(t->startLoop());
            threads_.push_back(std::move(t));
        }
    }

    // round-robin 取一个 IO loop；无 IO 线程时返回主 loop（单 Reactor 模式）
    EventLoop* getNextLoop() {
        if (loops_.empty()) return base_loop_;
        EventLoop* loop = loops_[next_];
        next_ = (next_ + 1) % loops_.size();
        return loop;
    }

    size_t size() const { return loops_.size(); }

    void stop() {
        for (auto& t : threads_) t->stop();
        threads_.clear();
        loops_.clear();
    }

private:
    EventLoop* base_loop_;
    int num_threads_;
    size_t next_ = 0;
    std::vector<std::unique_ptr<EventLoopThread>> threads_;
    std::vector<EventLoop*> loops_;
};

    } // namespace network
} // namespace common
