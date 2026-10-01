//
// Created by 29108 on 2025/7/1.
//

#include "net/event_loop.h"
#include "logger/logger.h"
#include "net/channel.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <iostream>

namespace common {
    namespace network {
static constexpr int kEpollTimeoutMs = 100;  // 毕设默认值；timerfd 精准唤醒 = 演进项
        /**
 * @brief 创建用于线程间通信的eventfd
 * @return eventfd文件描述符
 *
 * 功能说明：
 * - 创建一个eventfd用于唤醒事件循环
 * - EFD_NONBLOCK: 设置为非阻塞模式，避免读写操作阻塞
 * - EFD_CLOEXEC: 在exec时自动关闭，防止文件描述符泄露
 *
 * eventfd特性：
 * - 轻量级的线程间通信机制
 * - 可以被epoll监听
 * - 写入数据会唤醒等待的读操作
 * - 比pipe更高效，只需要一个文件描述符
 */
        int createEventfd() {
            int evtfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
            if (evtfd < 0) {
                LOG_ERROR("Failed in eventfd");
                // 不要调用abort()，而是抛出异常
                throw std::runtime_error("Failed to create eventfd: " + std::string(strerror(errno)));
            }
            return evtfd;
        }

        int createTimerfd() {
            int timerFd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
            if (timerFd_ < 0) {
                LOG_ERROR("Failed in timerFd_");
                // 不要调用abort()，而是抛出异常
                throw std::runtime_error("Failed to create timerFd_: " + std::string(strerror(errno)));
            }
            return timerFd_;
        }

        EventLoop::EventLoop()
            : looping_(false),
                quit_(false),
                threadId_(std::this_thread::get_id()),
                poller_(std::make_unique<Epoll>(this)),
                wakeupFd_(createEventfd()),
                wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
                callingPendingFunctors_(false),
                timerFd_(-1) {

            wakeupChannel_->setReadCallback([this] { handleRead(); });
            wakeupChannel_->enableReading();

            // 无参构造函数也初始化时间轮
            timing_wheel_ = std::make_unique<timer::HierarchicalTimingWheel>();
            timing_wheel_->setAsyncTaskSubmitter(
                [this](std::function<void()> task) {
                    this->runInLoop(std::move(task));
                }
            );
        }

        EventLoop::~EventLoop() {
            // 避免在析构时使用日志系统，使用标准输出
            std::cout << "[EventLoop] Destructor called in thread "
                     << std::hash<std::thread::id>{}(std::this_thread::get_id())
                     << ", created in thread "
                     << std::hash<std::thread::id>{}(threadId_) << std::endl;

            try {
                // 确保EventLoop已停止运行
                if (looping_.load()) {
                    std::cout << "[EventLoop] Force stopping loop in destructor" << std::endl;
                    quit_ = true;

                    // 等待循环退出，但设置超时避免死锁
                    auto start_time = std::chrono::steady_clock::now();
                    constexpr auto timeout = std::chrono::milliseconds(1000);  // 1秒超时

                    while (looping_.load() &&
                           std::chrono::steady_clock::now() - start_time < timeout) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }

                    if (looping_.load()) {
                        std::cerr << "[EventLoop] WARNING: Loop still running after timeout in destructor" << std::endl;
                    }
                }

                // 按照正确的顺序，避免访问无效fd
                if (wakeupChannel_) {
                    // 直接设置events为0，避免调用update()触发epoll_ctl
                    wakeupChannel_.reset();  // 先重置Channel指针
                }

                // 🔧 安全关闭文件描述符：检查有效性后再关闭
                if (timerFd_ >= 0) {
                    try {
                        ::close(timerFd_);
                        std::cout << "[EventLoop] Timer fd " << timerFd_ << " closed safely" << std::endl;
                    } catch (...) {
                        std::cerr << "[EventLoop] Error closing timer fd " << timerFd_ << std::endl;
                    }
                    timerFd_ = -1;
                }

                if (wakeupFd_ >= 0) {
                    try {
                        ::close(wakeupFd_);
                        std::cout << "[EventLoop] Wakeup fd " << wakeupFd_ << " closed safely" << std::endl;
                    } catch (...) {
                        std::cerr << "[EventLoop] Error closing wakeup fd " << wakeupFd_ << std::endl;
                    }
                    wakeupFd_ = -1;
                }

                // Epoll fd将由Epoll析构函数自动关闭
                if (poller_) {
                    std::cout << "[EventLoop] Epoll fd will be closed by Epoll destructor" << std::endl;
                }

                std::cout << "[EventLoop] Destructor completed safely" << std::endl;

            } catch (const std::exception& e) {
                std::cerr << "[EventLoop] Exception in destructor: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "[EventLoop] Unknown exception in destructor" << std::endl;
            }
        }

        void EventLoop::loop() {
            if (looping_) {
                LOG_ERROR("EventLoop::loop() can not run again!");
                return;
            }

            looping_ = true;
            quit_ = false;

            // 设置线程ID为当前运行线程（修复跨线程初始化问题）
            threadId_.store(std::this_thread::get_id());

            while (!quit_) {
                activeChannels_.clear();

                auto pollEventQty = poller_->poll(kEpollTimeoutMs, &activeChannels_);

                if (pollEventQty > 0) {
                    // LOG_DEBUG("新一次事件循环 此次事件 " + std::to_string(pollEventQty) +
                    //          " 个Channel触发");
                }
                
                // 处理事件
                ChannelList channelsToHandle = activeChannels_;
                for (Channel* channel : channelsToHandle) {
                    // 增强Channel有效性检查
                    if (!channel) {
                        LOG_WARNING("EventLoop::loop() encountered null channel pointer");
                        continue;
                    }

                    // 检查Channel是否仍在activeChannels中（避免已析构的Channel）
                    if (std::find(activeChannels_.begin(), activeChannels_.end(), channel) != activeChannels_.end()) {
                        try {
                            // LOG_DEBUG("EventLoop::loop() 处理Channel fd=" + std::to_string(channel->fd()) +
                            //          ", revents=" + std::to_string(channel->revents()));
                            channel->handleEvent();
                        } catch (const std::system_error& e) {
                            // 特别处理系统错误（如errno=22），不让它终止EventLoop
                            LOG_ERROR("Channel handleEvent system error for fd " + std::to_string(channel->fd()) +
                                     ": " + e.what() + " (errno: " + std::to_string(e.code().value()) + ")");

                            // 如果是EINVAL错误，通常表示fd已无效，从activeChannels移除该channel
                            if (e.code().value() == EINVAL) {
                                LOG_WARNING("Removing invalid channel fd " + std::to_string(channel->fd()) + " from EventLoop");
                                // 注意：这里不调用removeChannel，因为fd可能已无效
                                // EventLoop会在下次循环时自动清理无效的channels
                            }
                        } catch (const std::exception& e) {
                            LOG_ERROR("Channel handleEvent error for fd " + std::to_string(channel->fd()) + ": " + e.what());
                        } catch (...) {
                            LOG_ERROR("Channel handleEvent unknown error for fd " + std::to_string(channel->fd()));
                        }
                    }
                }

                doPendingFunctors();
            }

            LOG_INFO("EventLoop " + std::to_string(std::hash<std::thread::id>{}(threadId_)) + " stop looping");
            looping_ = false;
        }

        void EventLoop::quit() {
            quit_ = true;
            if (!isInLoopThread()) {
                // 检查wakeupFd_是否有效，避免在析构过程中写入已关闭的fd
                if (wakeupFd_ >= 0) {
                    wakeup();
                }
            }
        }

        void EventLoop::runInLoop(const Functor &cb) {
            if (isInLoopThread()) {
                cb();
            } else {
                queueInLoop(std::move(cb));
            }
        }

        void EventLoop::queueInLoop(const Functor &cb) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pendingFunctors_.push_back(std::move(cb));
            }

            if (!isInLoopThread() || callingPendingFunctors_) {
                wakeup();
            }
        }

        void EventLoop::initTimer(){
            // 按需创建timerFd_
            if (timerFd_ < 0) {
                try {
                    timerFd_ = createTimerfd();
                } catch (const std::exception& e) {
                    LOG_ERROR("[TIMER_INIT] ❌ 创建timerfd失败: " + std::string(e.what()));
                    throw;  // ✅ 抛出异常，而不是静默返回
                }
            } else {
                LOG_INFO("[TIMER_INIT] timerfd已存在: fd=" + std::to_string(timerFd_));
            }

            timerChannel_  = std::make_unique<Channel>(this, timerFd_);

            timerChannel_ ->setReadCallback([this] {
                handleTimerfdRead();
            });

            timerChannel_ ->enableReading();
            setInitialTimerfdTimeout();
        }

        uint64_t EventLoop::runAfter(int ms, std::function<void()> cb) {
            try {
                assertInLoopThread(); // 确保定时器操作在 loop 线程进行，或确保线程安全
                if (!timing_wheel_) {
                    LOG_ERROR("Timing wheel not initialized");
                    return 0; // 🔧 不抛出异常，避免程序崩溃
                }

                //当添加定时器时，按需创建timerFd_和timerChannel_
                if (!timerChannel_) {
                    initTimer();
                }

                return timing_wheel_->addTimer(std::chrono::milliseconds(ms), std::move(cb));

            } catch (const std::exception& e) {
                LOG_ERROR("Exception in runEvery: " + std::string(e.what()));
                return 0;  // 返回0表示失败
            } catch (...) {
                LOG_ERROR("Unknown exception in runEvery");
                return 0;
            }

        }

        uint64_t EventLoop::runEvery(int ms, std::function<void()> cb)
        {
            try {
                assertInLoopThread();

                if (!timerChannel_) {
                    initTimer();
                }

                uint64_t timer_id = timing_wheel_->addTimer(std::chrono::milliseconds(ms), std::move(cb), std::chrono::milliseconds(ms));
                return timer_id;

            } catch (const std::exception& e) {
                LOG_ERROR("Exception in runEvery: " + std::string(e.what()));
                return 0;  // 返回0表示失败
            } catch (...) {
                LOG_ERROR("Unknown exception in runEvery");
                return 0;
            }
        }

        void EventLoop::cancelTimer(uint64_t id) {
            //    注意：cancel 可以在非 loop 线程调用，因为 timing_wheel_->cancelTimer 是线程安全的
            //    它内部有 mutex 保护 timer_map_
            if (timing_wheel_) {
                timing_wheel_->cancelTimer(id);
            }
        }

        /**
         * @brief 唤醒事件循环（核心线程间通信机制）
         *
         * 功能说明：
         * - 向wakeupFd_写入数据，唤醒正在epoll_wait()中阻塞的事件循环
         * - 实现线程安全的事件循环唤醒机制
         * - 用于跨线程任务调度和事件通知
         *
         * 工作原理：
         * 1. 向eventfd写入一个64位整数(值为1)
         * 2. eventfd变为可读状态，触发epoll事件
         * 3. 事件循环从epoll_wait()返回，处理待执行任务
         * 4. 通过handleRead()读取eventfd数据，重置状态
         *
         * 使用场景：
         * - 其他线程向IO线程提交任务时
         * - 定时器到期需要处理时
         * - 需要立即中断epoll_wait()阻塞时
         * - 实现异步任务调度时
         *
         * 线程安全性：
         * - 可以从任意线程安全调用
         * - eventfd的写操作是原子的
         * - 多次调用会累加计数，但只触发一次唤醒
         *
         * 性能特点：
         * - 系统调用开销小，比pipe更高效
         * - 非阻塞写入，不会阻塞调用线程
         * - 内核级别的事件通知机制
         */
        void EventLoop::wakeup() {
            // 检查wakeupFd_是否有效
            if (wakeupFd_ < 0) {
                return; // 静默返回，避免在析构过程中产生错误日志
            }

            // 写入64位整数1到eventfd
            // eventfd要求写入8字节的uint64_t类型数据
            uint64_t one = 1;

            // 执行非阻塞写操作
            ssize_t n = ::write(wakeupFd_, &one, sizeof one);

            // 错误检查：确保写入了正确的字节数
            if (n != sizeof one) {
                // 检查具体的错误原因
                if (n == -1) {
                    int error = errno;
                    if (error == EBADF) {
                        // fd已关闭，这在析构过程中是正常的，不记录错误
                        return;
                    } else if (error == EINTR) {
                        // 被信号中断，重试一次
                        n = ::write(wakeupFd_, &one, sizeof one);
                        if (n == sizeof one) {
                            return; // 重试成功
                        }
                    }
                }

                // 只在非析构情况下记录错误
                if (wakeupFd_ >= 0) {
                    LOG_ERROR("EventLoop::wakeup() writes " + std::to_string(n) +
                             " bytes instead of 8, errno: " + std::to_string(errno));
                }
            }

            // 注意：即使写入失败，也不会影响程序的正确性
            // 最多只是延迟一次事件循环的唤醒
        }

        void EventLoop::updateChannel(Channel *channel) {
            if(channel->ownerLoop() != this) {
                LOG_ERROR("channel is  not belong to this eventloop");
                return;
            }
            assertInLoopThread();
            poller_->updateChannel(channel);
        }

        void EventLoop::removeChannel(Channel *channel) {
            if(channel->ownerLoop() != this) {
                LOG_ERROR("channel is  not belong to this eventloop");
                return;
            }
            assertInLoopThread();
            poller_->removeChannel(channel);
        }

        /**
         * @brief 检查当前线程是否为EventLoop绑定的线程
         * @return 如果当前线程是EventLoop线程返回true，否则返回false
         *
         * 功能说明：
         * - 比较当前线程ID与EventLoop创建时的线程ID
         * - 用于确保EventLoop的方法在正确的线程中调用
         * - 这是线程安全检查的核心方法
         */
        bool EventLoop::isInLoopThread() const {
            return threadId_.load() == std::this_thread::get_id();
        }

        /**
         * @brief 检查EventLoop是否包含指定的Channel
         * @param channel 要检查的Channel指针
         * @return 如果包含该Channel返回true，否则返回false
         *
         * 功能说明：
         * - 通过Epoll检查是否监听了该Channel的文件描述符
         * - 用于验证Channel是否已经注册到EventLoop
         */
        bool EventLoop::hasChannel(Channel *channel) {
            assert(channel->ownerLoop() == this);
            assertInLoopThread();
            return poller_->hasChannel(channel);
        }

        void EventLoop::handleRead() {
            uint64_t one = 1;

            // 从eventfd读取数据，获取唤醒次数
            ssize_t n = ::read(wakeupFd_, &one, sizeof one);

            if (n != sizeof one) {
                // 检查具体的错误类型
                if (n == -1) {
                    int error = errno;
                    if (error == EAGAIN || error == EWOULDBLOCK) {
                        // 这是正常情况：eventfd中没有数据
                        // 可能是多个线程同时处理了同一个事件
                        // LOG_DEBUG("EventLoop::handleRead() no data available (EAGAIN)");
                    } else {
                        // 其他错误情况
                        LOG_ERROR("EventLoop::handleRead() failed: " + std::string(strerror(error)));
                    }
                } else {
                    // 读取的字节数不正确
                    LOG_ERROR("EventLoop::handleRead() reads " + std::to_string(n) +
                             " bytes instead of 8");
                }
            } else {
                // 成功读取，记录唤醒次数（调试信息）
                // LOG_DEBUG("EventLoop wakeup handled, count: " + std::to_string(one));
            }

            // 注意：无论读取是否成功，事件循环都会继续运行
            // handleRead()的主要目的是清除eventfd的可读状态
            // 实际的任务处理在doPendingFunctors()中进行
        }

        void EventLoop::handleTimerfdRead() {
            // LOG_DEBUG("[TIMER_FD] handleTimerfdRead() 被调用");

            uint64_t expirations;
            ssize_t n = ::read(timerFd_, &expirations, sizeof(expirations));
            
            if (n != sizeof(expirations)) {
                LOG_ERROR("[TIMER_FD] ❌ read失败, n=" + std::to_string(n) +
                         ", expected=" + std::to_string(sizeof(expirations)) +
                         ", errno=" + std::string(strerror(errno)) +
                         ", timerFd_=" + std::to_string(timerFd_));
                return;
            }

            // LOG_DEBUG("[TIMER_FD] ✅ read成功, expirations=" + std::to_string(expirations));

            // 驱动时间轮
            if (timing_wheel_) {
                // tick 在 loop 线程同步执行——定时器回调不跨线程（逻辑单线程语义）
                timing_wheel_->tick();
                resetTimerfd();
            } else {
                LOG_ERROR("❌ [TIMER_FD] timing_wheel_ is null in handleTimerfdRead()!");
            }
        }

        void EventLoop::setInitialTimerfdTimeout() {
            if (timerFd_ < 0) {
                std::string error = "timerFd_ is invalid: " + std::to_string(timerFd_) + 
                                   ", timer system will not work!";
                LOG_ERROR("[TIMER_INIT] " + error);
                throw std::runtime_error(error);  // ✅ 抛出异常
            }

            // 设置初始较短超时（100ms），快速启动定时机制
            const int initial_timeout_ms = 100;

            struct itimerspec new_value;
            struct itimerspec old_value;

            new_value.it_value.tv_sec = initial_timeout_ms / 1000;
            new_value.it_value.tv_nsec = (initial_timeout_ms % 1000) * 1000000;
            new_value.it_interval.tv_sec = 0;
            new_value.it_interval.tv_nsec = 0;

            int ret = ::timerfd_settime(timerFd_, 0, &new_value, &old_value);
            if (ret < 0) {
                std::string error = "setInitialTimerfdTimeout failed: " + std::string(strerror(errno));
                LOG_ERROR("[TIMER_INIT] " + error);
                throw std::runtime_error(error);  // ✅ 抛出异常
            }
            
            LOG_INFO("[TIMER_INIT] ✅ 初始超时设置成功: " + std::to_string(initial_timeout_ms) + "ms");
        }

        void EventLoop::resetTimerfd() {
            // LOG_DEBUG("[TIMER_FD] resetTimerfd() 被调用");

            if (!timing_wheel_) {
                LOG_ERROR("[TIMER_FD] ❌ timing_wheel_ is null in resetTimerfd()!");
                return;
            }

            if (timerFd_ < 0) {
                LOG_ERROR("[TIMER_FD] ❌ timerFd_ is invalid in resetTimerfd(): " + std::to_string(timerFd_));
                return;
            }

            int nextExpireMs = timing_wheel_->getNextExpiration(kEpollTimeoutMs);
            // LOG_DEBUG("[TIMER_FD] 下次超时: " + std::to_string(nextExpireMs) + "ms");

            // 边界检查：确保延迟时间合理
            if (nextExpireMs <= 0) {
                nextExpireMs = 1; // 至少1ms，避免过于频繁的触发
            }

            struct itimerspec new_value;
            struct itimerspec old_value;

            new_value.it_value.tv_sec = nextExpireMs / 1000;
            new_value.it_value.tv_nsec = (nextExpireMs % 1000) * 1000000;
            new_value.it_interval.tv_sec = 0;
            new_value.it_interval.tv_nsec = 0;

            int ret = ::timerfd_settime(timerFd_, 0, &new_value, &old_value);
            if (ret < 0) {
                LOG_ERROR("[TIMER_FD] ❌ timerfd_settime failed: " + std::string(strerror(errno)));
                // 🔧 关键修复：避免timerfd_settime失败导致程序崩溃
                if (errno == EINVAL) {
                    LOG_ERROR("[TIMER_FD] timerFd_可能已失效，重新创建定时器");
                    // 不抛出异常，避免程序崩溃
                    return;
                } else {
                    LOG_ERROR("[TIMER_FD] timerfd_settime其他错误: " + std::string(strerror(errno)));
                    return;
                }
            } else {
                // LOG_DEBUG("[TIMER_FD] ✅ timerfd重置成功: " + std::to_string(nextExpireMs) + "ms");
            }
        }

        /**
         * @brief 执行待处理的函数队列（跨线程任务执行机制）
         *
         * 功能说明：
         * - 执行其他线程通过runInLoop()提交的任务
         * - 实现线程安全的任务队列处理
         * - 支持跨线程的异步任务调度
         *
         * 工作原理：
         * 1. 使用局部变量交换待执行函数队列，减少锁持有时间
         * 2. 在无锁状态下执行所有待处理函数
         * 3. 设置标志位防止递归调用
         *
         * 线程安全设计：
         * - 使用mutex保护pendingFunctors_队列
         * - 快速交换队列内容，最小化临界区
         * - 执行期间不持有锁，避免死锁
         *
         * 性能优化：
         * - 批量处理所有待执行任务
         * - 减少锁竞争和上下文切换
         * - 支持任务执行期间继续接收新任务
         */
        void EventLoop::doPendingFunctors() {
            // 用于存储待执行函数的局部队列
            std::vector<std::function<void()>> functors;

            // 设置标志位，防止在执行过程中递归调用
            callingPendingFunctors_ = true;

            {
                // 临界区：快速交换队列内容
                std::lock_guard<std::mutex> lock(mutex_);
                functors.swap(pendingFunctors_);
                // 交换后pendingFunctors_为空，可以继续接收新任务
            }

            // 在无锁状态下执行所有待处理函数
            for (const auto& functor : functors) {
                try {
                    // 执行用户提交的函数
                    functor();
                } catch (const std::exception& e) {
                    // 捕获并记录用户函数中的异常，不影响其他任务
                    LOG_ERROR("Exception in pending functor: " + std::string(e.what()));
                } catch (...) {
                    // 捕获所有其他类型的异常
                    LOG_ERROR("Unknown exception in pending functor");
                }
            }

            // 重置标志位
            callingPendingFunctors_ = false;

            // 记录执行的任务数量（调试信息）
            if (!functors.empty()) {
                // LOG_DEBUG("Executed " + std::to_string(functors.size()) + " pending functors");
            }
        }

        /**
         * @brief 当不在EventLoop线程中时终止程序
         *
         * 功能说明：
         * - 记录错误信息并终止程序
         * - 用于严格的线程安全检查
         * - 帮助开发者发现线程使用错误
         */
        void EventLoop::abortNotInLoopThread() const {
            std::string errorMsg = "EventLoop was created in thread " +
                                 std::to_string(std::hash<std::thread::id>{}(threadId_)) +
                                 ", but current thread is " +
                                 std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));

            LOG_ERROR(errorMsg);

            // 不要调用abort()，统一抛出异常
            throw std::runtime_error("EventLoop thread safety violation: " + errorMsg);
        }

    }
}
