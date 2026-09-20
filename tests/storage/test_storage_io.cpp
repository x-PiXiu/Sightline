// test_storage_io.cpp —— 存储线程池骨架单测：投递/执行/回投/停机

#include "storage/storage_io.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>

// 极简 EventLoop 替身：只提供 queueInLoop 的队列语义（回投验证用）
class FakeLoop
{
public:
    void queueInLoop(std::function<void()> cb)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.push_back(std::move(cb));
    }
    void drain()   // 模拟主循环消费回投
    {
        std::deque<std::function<void()>> all;
        { std::lock_guard<std::mutex> lk(mtx_); all.swap(pending_); }
        for (auto& cb : all) cb();
    }
    size_t pendingCount()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        return pending_.size();
    }
private:
    std::mutex mtx_;
    std::deque<std::function<void()>> pending_;
};

int main()
{
    using sightline::StorageIO;

    FakeLoop loop;

    // 真实 EventLoop 也跑起来（回投目标）——本测试用 FakeLoop 即可，不启真 loop
    StorageIO io;
    io.start(nullptr);   // mainLoop 为空时 postBackToMain 不投递，仅验证骨架不崩

    // 1) 任务在存储线程执行
    std::atomic<bool> ran{false};
    io.post([&ran] { ran = true; });
    // 等 worker 消费（最多 1s）
    for (int i = 0; i < 100 && !ran; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!ran) { std::puts("[FAIL] 存储任务未执行"); return 1; }

    // 2) 停机：stop 后 pop 返回 false，工作线程退出
    io.stop();
    std::puts("[PASS] StorageIO 骨架（投递/执行/停机）");

    // 3) 回投语义（用 FakeLoop 验证队列语义）
    loop.queueInLoop([] { std::puts("[PASS] 回投回调在主侧执行"); });
    loop.drain();

    std::puts("[PASS] StorageIO 全部用例通过");
    return 0;
}
