// storage_io.h —— 存储线程池骨架：任务队列 + 专用线程 + EventLoop 回投（服务端 01 文档 D1）
//
// 铁律：epoll 网络线程禁止阻塞 IO。一切 MySQL/Redis/Mongo 操作以
//       post(job) 投递到本线程执行；结果经 postBackToMain 回投主 EventLoop。
//
// D2~D4 起StorageContext 将挂载 MysqlPool / RedisConnection / MongoClient；
//       D1 先跑通"投递→执行→回投"骨架（日志即演示）。

#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

#include "net/event_loop.h"

namespace sightline {

/** 线程安全任务队列（极简版：互斥 + 条件变量阻塞弹出） */
class ThreadSafeTaskQueue
{
public:
    void push(std::function<void()> job)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push(std::move(job));
        }
        cv_.notify_one();
    }

    /** 阻塞弹出；stop() 后返回 false 退出工作循环 */
    bool pop(std::function<void()>& out)
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !queue_.empty() || stop_; });
        if (stop_ && queue_.empty()) return false;
        out = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            stop_ = true;
        }
        cv_.notify_all();
    }

private:
    std::queue<std::function<void()>> queue_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    bool                    stop_ = false;
};

/** 存储线程池骨架：单工作线程消费任务；回投主 EventLoop */
class StorageIO
{
public:
    /** mainLoop：回投目标（服务器主 EventLoop）；启动工作线程 */
    void start(common::network::EventLoop* mainLoop)
    {
        mainLoop_ = mainLoop;
        running_ = true;
        worker_ = std::thread([this] { run(); });
    }

    void stop()
    {
        running_ = false;
        queue_.stop();
        if (worker_.joinable()) worker_.join();
    }

    /** 主线程调用：投递存储任务 */
    void post(std::function<void()> job) { queue_.push(std::move(job)); }

    /** 存储线程调用：把结果回投到主 EventLoop 执行（跨线程投递，复用 Reactor 机制） */
    void postBackToMain(std::function<void()> cb)
    {
        if (mainLoop_) mainLoop_->queueInLoop(std::move(cb));
    }

private:
    void run()
    {
        std::function<void()> job;
        while (queue_.pop(job))
        {
            if (job) job();
        }
    }

    ThreadSafeTaskQueue      queue_;
    std::thread              worker_;
    common::network::EventLoop* mainLoop_ = nullptr;
    std::atomic<bool>        running_{false};
};

} // namespace sightline
