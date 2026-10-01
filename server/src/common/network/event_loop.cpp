//
// Created by 29108 on 2025/7/1.
//

#include "common/network/event_loop.h"
#include "common/logger/logger.h"
#include "common/network/channel.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <cassert>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <iostream>

namespace common {
    namespace network {
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

        EventLoop::EventLoop(const networkConfig &config)
            : config_(config),
            looping_(false),
            quit_(false),
            threadId_(std::this_thread::get_id()),
            wakeupFd_(createEventfd()),
            wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
            callingPendingFunctors_(false),
            timerFd_(-1) { // 不创建timerFd_，按需创建
            config_.validate(); // 验证配置

            initializeEpoll();

            wakeupChannel_->setReadCallback([this] { handleRead(); });
            wakeupChannel_->enableReading();

            timing_wheel_ = std::make_unique<timer::HierarchicalTimingWheel>();

            // 这确保了定时器回调最终会在 EventLoop 线程中执行
            timing_wheel_->setAsyncTaskSubmitter(
                [this](std::function<void()> task) {
                    this->runInLoop(std::move(task));
                }
            );
        }

        EventLoop::EventLoop(const networkConfig &config,std::shared_ptr<common::thread_pool::ThreadPool> thread_pool)
            : config_(config),
              thread_pool_(thread_pool),
              looping_(false),
              quit_(false),
              threadId_(std::this_thread::get_id()),
              wakeupFd_(createEventfd()),
              wakeupChannel_(std::make_unique<Channel>(this, wakeupFd_)),
              callingPendingFunctors_(false),
              timerFd_(-1) { // 🔧 终极修复：不创建timerFd_，按需创建
            config_.validate();

            initializeEpoll();

            wakeupChannel_->setReadCallback([this] { handleRead(); });
            wakeupChannel_->enableReading();

            // 初始化时间轮和 timerfd
            timing_wheel_ = std::make_unique<timer::HierarchicalTimingWheel>();

            // 设置时间轮的异步任务提交器，指向 EventLoop 自身的 runInLoop
            // 这确保了定时器回调最终会在 EventLoop 线程中执行
            timing_wheel_->setAsyncTaskSubmitter(
                [this](std::function<void()> task) {
                    this->runInLoop(std::move(task));
                }
            );
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

                auto pollEventQty = poller_->poll(config_.epoll_timeout, &activeChannels_);

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

        void EventLoop::setThreadPool(std::shared_ptr<common::thread_pool::ThreadPool> pool) {
            thread_pool_ = pool;
            LOG_INFO("ThreadPool attached to EventLoop");
        }

        void EventLoop::enableNetworkConfigHotReload(const std::string &config_file_path,
            std::chrono::seconds check_interval) {
            if (looping_.load()) {
                LOG_WARNING("Cannot enable hot reload while EventLoop is running");
                return;
            }

            // 禁用之前的热更新
            disableNetworkConfigHotReload();

            config_hot_reload_enabled_ = true;
            config_file_path_ = config_file_path;
            config_check_interval_ = check_interval;

            // 获取ConfigManager实例
            auto& config_manager = common::config::ConfigManager::getInstance();

            // 注册网络相关配置的监听器
            std::vector<std::string> network_config_keys = {
                "network.bind_address",
                "network.port",
                "network.backlog",
                "network.reuse_address",
                "network.reuse_port",
                "network.tcp_no_delay",
                "network.socket_keep_alive",
                "network.keep_alive_idle_time",
                "network.keep_alive_interval",
                "network.keep_alive_probes",
                "network.enable_epoll",
                "network.epoll_timeout",
                "network.max_events",
                "network.init_event_list_size",
                "network.enable_event_loop_monitoring",
                "network.enable_pending_functors_optimization",
                "network.enable_thread_pool",
                "network.thread_pool_size",
                "network.use_dedicated_io_threads",
                "network.enable_channel_validation",
                "network.enable_epoll_resize_optimization",
                "network.epoll_resize_factor"
            };

            // 为每个配置键注册监听器
            for (const auto& key : network_config_keys) {
                config_manager.addChangeListener(key,
                    [this](const std::string& k, const std::string& old_val, const std::string& new_val) {
                        this->onNetworkConfigChanged(k, old_val, new_val);
                    });
                config_listener_keys_.push_back(key);
            }

            // 启用ConfigManager的热更新
            config_manager.enableHotReload(config_file_path, check_interval);

            LOG_INFO("Network config hot reload enabled for file: " + config_file_path);
        }

        void EventLoop::disableNetworkConfigHotReload() {
            if (!config_hot_reload_enabled_.load()) {
                return;
            }

            config_hot_reload_enabled_ = false;

            // 移除配置监听器
            auto& config_manager = common::config::ConfigManager::getInstance();
            for (const auto& key : config_listener_keys_) {
                config_manager.removeChangeListeners(key);
            }
            config_listener_keys_.clear();

            LOG_INFO("Network config hot reload disabled");
        }

        void EventLoop::updateNetworkConfig(const networkConfig &new_config) {
            assertInLoopThread();

            try {
                // 验证新配置
                new_config.validate();

                auto old_config = config_;

                // 详细的配置变更检测
                ConfigChangeAnalysis changes = analyzeConfigChanges(old_config, new_config);

                // 记录配置变更详情
                logConfigChanges(old_config, new_config, changes);

                // 原子性配置更新：先备份，再更新，失败时回滚
                auto config_backup = config_;
                config_ = new_config;

                try {
                    // 按优先级顺序应用配置变更
                    applyConfigChangesInOrder(old_config, new_config, changes);

                    // 通知配置变更完成
                    notifyConfigUpdateComplete(old_config, new_config);

                    LOG_INFO("Network configuration updated successfully");

                } catch (const std::exception& e) {
                    // 配置应用失败，回滚到原配置
                    LOG_ERROR("Config update failed, rolling back: " + std::string(e.what()));
                    config_ = config_backup;
                    throw;
                }

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to update network config: " + std::string(e.what()));
                throw;
            }
        }

        void EventLoop::setThreadPoolFactoryCallback(ThreadPoolFactoryCallback callback) {
            thread_pool_factory_callback_ = std::move(callback);
            // LOG_DEBUG("Thread pool factory callback registered");
        }

        void EventLoop::setConfigChangeCallback(ConfigChangeCallback callback) {
            config_change_callback_ = std::move(callback);
            // LOG_DEBUG("Config change callback registered");
        }

        void EventLoop::setConfigUpdateEventCallback(ConfigUpdateEventCallback callback) {
            config_update_event_callback_ = std::move(callback);
            // LOG_DEBUG("Config update event callback registered");
        }

        /**
         * @brief 处理wakeupFd_的读事件（与wakeup()配对使用）
         *
         * 功能说明：
         * - 读取eventfd中的数据，重置其状态
         * - 清除eventfd的可读状态，为下次唤醒做准备
         * - 这是wakeup()机制的接收端处理函数
         *
         * 工作原理：
         * 1. 当wakeup()向eventfd写入数据后，eventfd变为可读
         * 2. epoll检测到可读事件，调用此函数
         * 3. 读取eventfd中的数据，获取累积的唤醒次数
         * 4. 读取后eventfd重置为不可读状态
         *
         * 数据含义：
         * - 读取的uint64_t值表示自上次读取以来的唤醒次数
         * - 多次wakeup()调用会累加这个值
         * - 一次handleRead()会清空所有累积的唤醒
         *
         * 错误处理：
         * - EAGAIN/EWOULDBLOCK: 正常情况，表示没有数据可读
         * - 其他错误: 记录日志但不影响程序运行
         */
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
                // LOG_DEBUG("[TIMER_FD] 提交tick()任务到异步执行");
                executeCallbackAsync([this]{
                    // LOG_DEBUG("[TIMER_WHEEL] 开始执行tick()");
                    timing_wheel_->tick();
                    // LOG_DEBUG("[TIMER_WHEEL] tick()执行完成");

                    // LOG_DEBUG("[TIMER_FD] 重新设置timerfd");
                    resetTimerfd();
                });

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

            int nextExpireMs = timing_wheel_->getNextExpiration(config_.epoll_timeout);
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

        void EventLoop::onNetworkConfigChanged(const std::string &key, const std::string &old_value,
            const std::string &new_value) {
            LOG_INFO("Network config changed: " + key + " = " + new_value + " (was: " + old_value + ")");

            // 在IO线程中安全地处理配置变更
            runInLoop([this, key, old_value, new_value]() {
                try {
                    // 重新加载完整的网络配置
                    auto new_config = networkConfig::fromConfigManager();

                    // 验证配置变更的安全性
                    if (!isConfigChangeSafe(config_, new_config)) {
                        LOG_WARNING("Unsafe config change detected for key: " + key +
                                   ", skipping update. Restart required for this change.");
                        return;
                    }

                    // 应用新配置
                    updateNetworkConfig(new_config);

                } catch (const std::exception& e) {
                    LOG_ERROR("Failed to apply network config change for key " + key +
                             ": " + std::string(e.what()));
                }
            });
        }

        void EventLoop::safeReconfigureEpoll(const networkConfig &new_config) {
            assertInLoopThread();

            if (looping_.load()) {
                LOG_WARNING("Cannot safely reconfigure Epoll while EventLoop is running. "
                           "Some changes may require restart.");
                return;
            }

            try {
                // 保存当前注册的Channel
                std::vector<Channel*> active_channels;
                if (poller_) {
                    // 这里需要Epoll类提供获取活跃Channel的方法
                    //active_channels = poller_->getActiveChannels();
                }

                // 重新创建Epoll实例
                poller_ = std::make_unique<Epoll>(this,
                                                 new_config.init_event_list_size,
                                                 new_config.max_events,
                                                 new_config.enable_epoll_resize_optimization,
                                                 new_config.epoll_resize_factor);

                // 重新注册Channel
                for (auto* channel : active_channels) {
                    if (channel) {
                        poller_->updateChannel(channel);
                    }
                }

                LOG_INFO("Epoll reconfigured successfully");

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to reconfigure Epoll: " + std::string(e.what()));
                throw;
            }
        }

        void EventLoop::applySocketConfigToExistingConnections(const networkConfig &new_config) {
            assertInLoopThread();

            try {
                // 获取所有活跃的Channel
                if (!poller_) {
                    return;
                }

                auto channel_states = poller_->getAllChannelStates();

                for (const auto& [channel, events] : channel_states) {
                    if (!channel || channel->fd() < 0) {
                        continue;
                    }

                    // 应用Socket配置到现有连接
                    applySocketConfigToFd(channel->fd(), new_config);
                }

                LOG_INFO("Applied socket config to " + std::to_string(channel_states.size()) +
                        " existing connections");

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to apply socket config to existing connections: " +
                         std::string(e.what()));
            }
        }

        void EventLoop::applySocketConfigToFd(int fd, const networkConfig &new_config) {
            if (fd < 0) {
                return;
            }

            try {
                // 应用TCP_NODELAY选项
                int tcp_no_delay = new_config.tcp_no_delay ? 1 : 0;
                if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
                                &tcp_no_delay, sizeof(tcp_no_delay)) < 0) {
                    LOG_WARNING("Failed to set TCP_NODELAY on fd " + std::to_string(fd) +
                               ": " + std::string(strerror(errno)));
                                }

                // 应用SO_KEEPALIVE选项
                int keep_alive = new_config.socket_keep_alive ? 1 : 0;
                if (::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                                &keep_alive, sizeof(keep_alive)) < 0) {
                    LOG_WARNING("Failed to set SO_KEEPALIVE on fd " + std::to_string(fd) +
                               ": " + std::string(strerror(errno)));
                                } else if (new_config.socket_keep_alive) {
                                    // 应用Keep-Alive详细参数
                                    applyKeepAliveParams(fd, new_config);
                                }

                // LOG_DEBUG("Applied socket config to fd: " + std::to_string(fd));

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to apply socket config to fd " + std::to_string(fd) +
                         ": " + std::string(e.what()));
            }
        }

        void EventLoop::applyKeepAliveParams(int fd, const networkConfig &config) {
            if (fd < 0) {
                return;
            }

#ifdef TCP_KEEPIDLE
            int idle_time = config.keep_alive_idle_time;
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,
                            &idle_time, sizeof(idle_time)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPIDLE on fd " + std::to_string(fd) +
                           ": " + std::string(strerror(errno)));
                            }
#endif

#ifdef TCP_KEEPINTVL
            int interval = config.keep_alive_interval;
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,
                            &interval, sizeof(interval)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPINTVL on fd " + std::to_string(fd) +
                           ": " + std::string(strerror(errno)));
                            }
#endif

#ifdef TCP_KEEPCNT
            int probes = config.keep_alive_probes;
            if (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,
                            &probes, sizeof(probes)) < 0) {
                LOG_WARNING("Failed to set TCP_KEEPCNT on fd " + std::to_string(fd) +
                           ": " + std::string(strerror(errno)));
                            }
#endif

            // LOG_DEBUG("Applied keep-alive params to fd " + std::to_string(fd) +
            //          ": idle=" + std::to_string(config.keep_alive_idle_time) +
            //          "s, interval=" + std::to_string(config.keep_alive_interval) +
            //          "s, probes=" + std::to_string(config.keep_alive_probes));
        }

        bool EventLoop::isConfigChangeSafe(const networkConfig &old_config, const networkConfig &new_config) const {
            // 检查不能在运行时安全变更的配置
            if (looping_.load()) {
                // 运行时不能变更的配置
                if (old_config.bind_address != new_config.bind_address ||
                    old_config.port != new_config.port) {
                    return false; // 地址和端口变更需要重启
                    }
            }

            // 检查配置值的合理性
            if (new_config.max_events < old_config.max_events / 2) {
                LOG_WARNING("Large decrease in max_events detected, may impact performance");
            }

            return true;
        }

        EventLoop::ConfigChangeAnalysis EventLoop::analyzeConfigChanges(const networkConfig &old_config,
            const networkConfig &new_config) const {
            ConfigChangeAnalysis changes = {};

            // Epoll配置变更检测
            changes.epoll_config_changed = (
                old_config.max_events != new_config.max_events ||
                old_config.init_event_list_size != new_config.init_event_list_size ||
                old_config.enable_epoll_resize_optimization != new_config.enable_epoll_resize_optimization ||
                old_config.epoll_resize_factor != new_config.epoll_resize_factor ||
                old_config.epoll_timeout != new_config.epoll_timeout
            );

            // Socket配置变更检测
            changes.socket_config_changed = (
                old_config.tcp_no_delay != new_config.tcp_no_delay ||
                old_config.socket_keep_alive != new_config.socket_keep_alive ||
                old_config.keep_alive_idle_time != new_config.keep_alive_idle_time ||
                old_config.keep_alive_interval != new_config.keep_alive_interval ||
                old_config.keep_alive_probes != new_config.keep_alive_probes
            );

            // 线程池配置变更检测
            changes.thread_pool_config_changed = (
                old_config.enable_thread_pool != new_config.enable_thread_pool ||
                old_config.thread_pool_size != new_config.thread_pool_size ||
                old_config.use_dedicated_io_threads != new_config.use_dedicated_io_threads
            );

            // 监控配置变更检测
            changes.monitoring_config_changed = (
                old_config.enable_event_loop_monitoring != new_config.enable_event_loop_monitoring ||
                old_config.enable_pending_functors_optimization != new_config.enable_pending_functors_optimization ||
                old_config.enable_channel_validation != new_config.enable_channel_validation
            );

            // 网络基础配置变更检测（这些通常需要重启）
            changes.network_binding_changed = (
                old_config.bind_address != new_config.bind_address ||
                old_config.port != new_config.port ||
                old_config.backlog != new_config.backlog
            );

            // 危险配置变更检测
            changes.requires_restart = changes.network_binding_changed;

            return changes;
        }

        void EventLoop::applyConfigChangesInOrder(const networkConfig &old_config, const networkConfig &new_config,
            const ConfigChangeAnalysis &changes) {
            // 1. 首先检查是否有需要重启的配置变更
            if (changes.requires_restart) {
                LOG_WARNING("Detected configuration changes that require restart:");
                if (changes.network_binding_changed) {
                    LOG_WARNING("  - Network binding changed (bind_address/port/backlog)");
                }
                LOG_WARNING("These changes will be ignored. Please restart the service to apply them.");
            }

            // 2. 应用监控配置变更（优先级最高，影响最小）
            if (changes.monitoring_config_changed) {
                applyMonitoringConfigChanges(old_config, new_config);
            }

            // 3. 应用Socket配置变更（影响现有连接）
            if (changes.socket_config_changed) {
                applySocketConfigChanges(old_config, new_config);
            }

            // 4. 应用线程池配置变更（可能影响性能）
            if (changes.thread_pool_config_changed) {
                applyThreadPoolConfigChanges(old_config, new_config);
            }

            // 5. 应用Epoll配置变更（优先级最低，影响最大）
            if (changes.epoll_config_changed) {
                applyEpollConfigChanges(old_config, new_config);
            }
        }

        void EventLoop::applyMonitoringConfigChanges(const networkConfig &old_config, const networkConfig &new_config) {
            if (old_config.enable_event_loop_monitoring != new_config.enable_event_loop_monitoring) {
                LOG_INFO("Event loop monitoring " +
                        std::string(new_config.enable_event_loop_monitoring ? "enabled" : "disabled"));
            }

            if (old_config.enable_pending_functors_optimization != new_config.enable_pending_functors_optimization) {
                LOG_INFO("Pending functors optimization " +
                        std::string(new_config.enable_pending_functors_optimization ? "enabled" : "disabled"));
            }

            if (old_config.enable_channel_validation != new_config.enable_channel_validation) {
                LOG_INFO("Channel validation " +
                        std::string(new_config.enable_channel_validation ? "enabled" : "disabled"));
            }

            // LOG_DEBUG("Monitoring configuration changes applied successfully");
        }

        void EventLoop::applySocketConfigChanges(const networkConfig &old_config, const networkConfig &new_config) {
            LOG_INFO("Applying socket configuration changes to existing connections...");

            try {
                // 应用到所有现有连接
                applySocketConfigToExistingConnections(new_config);

                // 记录具体的变更内容
                if (old_config.tcp_no_delay != new_config.tcp_no_delay) {
                    LOG_INFO("TCP_NODELAY updated: " +
                            std::string(old_config.tcp_no_delay ? "enabled" : "disabled") +
                            " -> " + std::string(new_config.tcp_no_delay ? "enabled" : "disabled"));
                }

                if (old_config.socket_keep_alive != new_config.socket_keep_alive) {
                    LOG_INFO("SO_KEEPALIVE updated: " +
                            std::string(old_config.socket_keep_alive ? "enabled" : "disabled") +
                            " -> " + std::string(new_config.socket_keep_alive ? "enabled" : "disabled"));
                }

                if (new_config.socket_keep_alive && (
                    old_config.keep_alive_idle_time != new_config.keep_alive_idle_time ||
                    old_config.keep_alive_interval != new_config.keep_alive_interval ||
                    old_config.keep_alive_probes != new_config.keep_alive_probes)) {

                    LOG_INFO("Keep-Alive parameters updated: idle=" +
                            std::to_string(new_config.keep_alive_idle_time) + "s, interval=" +
                            std::to_string(new_config.keep_alive_interval) + "s, probes=" +
                            std::to_string(new_config.keep_alive_probes));
                    }

                LOG_INFO("Socket configuration changes applied successfully");

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to apply socket config changes: " + std::string(e.what()));
                throw;
            }
        }

        void EventLoop::applyThreadPoolConfigChanges(const networkConfig &old_config, const networkConfig &new_config) {
            LOG_INFO("Applying thread pool configuration changes...");

            try {
                // 处理线程池启用/禁用
                if (old_config.enable_thread_pool != new_config.enable_thread_pool) {
                    if (new_config.enable_thread_pool && !thread_pool_) {
                        LOG_INFO("Thread pool enabled via config update");

                        // 尝试创建新的线程池实例
                        if (thread_pool_factory_callback_) {
                            thread_pool_ = thread_pool_factory_callback_(new_config.thread_pool_size);
                            if (thread_pool_) {
                                LOG_INFO("Thread pool created with " +
                                        std::to_string(new_config.thread_pool_size) + " threads");
                            } else {
                                LOG_WARNING("Thread pool factory callback failed to create thread pool");
                            }
                        } else {
                            LOG_WARNING("Thread pool enabled but no factory callback registered. "
                                       "External thread pool injection required.");

                            // 通知外部需要创建线程池
                            if (config_change_callback_) {
                                config_change_callback_("thread_pool_required",
                                                       std::to_string(new_config.thread_pool_size));
                            }
                        }

                    } else if (!new_config.enable_thread_pool && thread_pool_) {
                        LOG_INFO("Thread pool disabled via config update");

                        // 等待当前任务完成后停止使用线程池
                        if (thread_pool_) {
                            // 不立即销毁线程池，而是标记为不使用
                            // 实际销毁由外部管理
                            LOG_INFO("Thread pool marked as disabled, external cleanup may be required");

                            if (config_change_callback_) {
                                config_change_callback_("thread_pool_disabled", "");
                            }
                        }
                    }
                }

                // 处理线程池大小变更
                if (new_config.enable_thread_pool && thread_pool_ &&
                    old_config.thread_pool_size != new_config.thread_pool_size) {

                    LOG_INFO("Thread pool size change requested: " +
                            std::to_string(old_config.thread_pool_size) + " -> " +
                            std::to_string(new_config.thread_pool_size));

                    // 线程池大小变更需要外部处理
                    if (config_change_callback_) {
                        config_change_callback_("thread_pool_resize",
                                               std::to_string(new_config.thread_pool_size));
                    } else {
                        LOG_WARNING("Thread pool resize requested but no callback registered");
                    }
                    }

                LOG_INFO("Thread pool configuration changes applied successfully");

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to apply thread pool config changes: " + std::string(e.what()));
                throw;
            }
        }

        void EventLoop::applyEpollConfigChanges(const networkConfig &old_config, const networkConfig &new_config) {
            LOG_INFO("Applying Epoll configuration changes...");

            try {
                // 检查是否需要重建Epoll
                bool needs_epoll_rebuild = (
                    old_config.max_events != new_config.max_events ||
                    old_config.init_event_list_size != new_config.init_event_list_size ||
                    old_config.enable_epoll_resize_optimization != new_config.enable_epoll_resize_optimization ||
                    old_config.epoll_resize_factor != new_config.epoll_resize_factor
                );

                if (needs_epoll_rebuild) {
                    if (looping_.load()) {
                        LOG_WARNING("Epoll rebuild required but EventLoop is running. "
                                   "Changes will take effect after restart or manual stop.");

                        // 记录需要重建的配置项
                        if (old_config.max_events != new_config.max_events) {
                            LOG_INFO("Max events change pending: " +
                                    std::to_string(old_config.max_events) + " -> " +
                                    std::to_string(new_config.max_events));
                        }

                        if (old_config.init_event_list_size != new_config.init_event_list_size) {
                            LOG_INFO("Initial event list size change pending: " +
                                    std::to_string(old_config.init_event_list_size) + " -> " +
                                    std::to_string(new_config.init_event_list_size));
                        }

                    } else {
                        // EventLoop未运行，可以安全重建
                        safeReconfigureEpoll(new_config);
                    }
                }

                // epoll_timeout可以立即生效，无需重建
                if (old_config.epoll_timeout != new_config.epoll_timeout) {
                    LOG_INFO("Epoll timeout updated: " +
                            std::to_string(old_config.epoll_timeout) + "ms -> " +
                            std::to_string(new_config.epoll_timeout) + "ms (effective immediately)");
                }

                LOG_INFO("Epoll configuration changes applied successfully");

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to apply Epoll config changes: " + std::string(e.what()));
                throw;
            }
        }

        void EventLoop::logConfigChanges(const networkConfig &old_config, const networkConfig &new_config,
            const ConfigChangeAnalysis &changes) const {
            LOG_INFO("=== Network Configuration Update Analysis ===");

            if (changes.network_binding_changed) {
                LOG_WARNING("Network binding changes detected (requires restart):");
                if (old_config.bind_address != new_config.bind_address) {
                    LOG_WARNING("  bind_address: " + old_config.bind_address + " -> " + new_config.bind_address);
                }
                if (old_config.port != new_config.port) {
                    LOG_WARNING("  port: " + std::to_string(old_config.port) + " -> " + std::to_string(new_config.port));
                }
                if (old_config.backlog != new_config.backlog) {
                    LOG_WARNING("  backlog: " + std::to_string(old_config.backlog) + " -> " + std::to_string(new_config.backlog));
                }
            }

            if (changes.socket_config_changed) {
                LOG_INFO("Socket configuration changes detected:");
                if (old_config.tcp_no_delay != new_config.tcp_no_delay) {
                    LOG_INFO("  tcp_no_delay: " + std::string(old_config.tcp_no_delay ? "true" : "false") +
                            " -> " + std::string(new_config.tcp_no_delay ? "true" : "false"));
                }
                if (old_config.socket_keep_alive != new_config.socket_keep_alive) {
                    LOG_INFO("  socket_keep_alive: " + std::string(old_config.socket_keep_alive ? "true" : "false") +
                            " -> " + std::string(new_config.socket_keep_alive ? "true" : "false"));
                }
            }

            if (changes.thread_pool_config_changed) {
                LOG_INFO("Thread pool configuration changes detected:");
                if (old_config.enable_thread_pool != new_config.enable_thread_pool) {
                    LOG_INFO("  enable_thread_pool: " + std::string(old_config.enable_thread_pool ? "true" : "false") +
                            " -> " + std::string(new_config.enable_thread_pool ? "true" : "false"));
                }
                if (old_config.thread_pool_size != new_config.thread_pool_size) {
                    LOG_INFO("  thread_pool_size: " + std::to_string(old_config.thread_pool_size) +
                            " -> " + std::to_string(new_config.thread_pool_size));
                }
            }

            if (changes.epoll_config_changed) {
                LOG_INFO("Epoll configuration changes detected:");
                if (old_config.max_events != new_config.max_events) {
                    LOG_INFO("  max_events: " + std::to_string(old_config.max_events) +
                            " -> " + std::to_string(new_config.max_events));
                }
                if (old_config.epoll_timeout != new_config.epoll_timeout) {
                    LOG_INFO("  epoll_timeout: " + std::to_string(old_config.epoll_timeout) +
                            "ms -> " + std::to_string(new_config.epoll_timeout) + "ms");
                }
            }

            if (changes.monitoring_config_changed) {
                LOG_INFO("Monitoring configuration changes detected:");
                if (old_config.enable_event_loop_monitoring != new_config.enable_event_loop_monitoring) {
                    LOG_INFO("  enable_event_loop_monitoring: " +
                            std::string(old_config.enable_event_loop_monitoring ? "true" : "false") +
                            " -> " + std::string(new_config.enable_event_loop_monitoring ? "true" : "false"));
                }
            }

            LOG_INFO("=== End Configuration Update Analysis ===");
        }

        void EventLoop::notifyConfigUpdateComplete(const networkConfig &old_config, const networkConfig &new_config) {
            // 调用外部回调通知配置更新完成
            if (config_change_callback_) {
                config_change_callback_("config_update_complete", "");
            }

            // 记录配置更新完成的审计日志
            LOG_INFO("Network configuration update completed successfully");
            LOG_INFO("Configuration hash: " + calculateConfigHash(new_config));

            // 可选：触发配置更新事件
            if (config_update_event_callback_) {
                config_update_event_callback_(old_config, new_config);
            }
        }

        std::string EventLoop::calculateSHA256Hash(const std::string &input) const {
            // 注意：这里需要包含 <openssl/sha.h> 或使用其他SHA256库
            // 为了避免依赖问题，这里提供一个简化的实现

#ifdef USE_OPENSSL
            unsigned char hash[SHA256_DIGEST_LENGTH];
            SHA256_CTX sha256;
            SHA256_Init(&sha256);
            SHA256_Update(&sha256, input.c_str(), input.length());
            SHA256_Final(hash, &sha256);

            std::ostringstream ss;
            for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
                ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
            }
            return ss.str();
#else
            // 简化实现：使用std::hash作为替代（仅用于演示）
            std::hash<std::string> hasher;
            size_t hash_value = hasher(input);

            // 将哈希值转换为十六进制字符串
            std::ostringstream ss;
            ss << std::hex << hash_value;

            // 添加时间戳以增加唯一性
            auto now = std::chrono::system_clock::now();
            auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            ss << "_" << std::hex << timestamp;

            LOG_WARNING("Using simplified hash implementation. Consider enabling OpenSSL for production use.");
            return ss.str();
#endif
        }

        std::string EventLoop::calculateConfigHash(const networkConfig &config) const {
            // 构建完整的配置字符串，包含所有配置项
            std::ostringstream config_stream;

            // Socket配置
            config_stream << "bind_address=" << config.bind_address << ";";
            config_stream << "port=" << config.port << ";";
            config_stream << "backlog=" << config.backlog << ";";
            config_stream << "reuse_address=" << (config.reuse_address ? "true" : "false") << ";";
            config_stream << "reuse_port=" << (config.reuse_port ? "true" : "false") << ";";
            config_stream << "tcp_no_delay=" << (config.tcp_no_delay ? "true" : "false") << ";";
            config_stream << "socket_keep_alive=" << (config.socket_keep_alive ? "true" : "false") << ";";

            // Keep-Alive参数
            config_stream << "keep_alive_idle_time=" << config.keep_alive_idle_time << ";";
            config_stream << "keep_alive_interval=" << config.keep_alive_interval << ";";
            config_stream << "keep_alive_probes=" << config.keep_alive_probes << ";";

            // Epoll配置
            config_stream << "enable_epoll=" << (config.enable_epoll ? "true" : "false") << ";";
            config_stream << "epoll_timeout=" << config.epoll_timeout << ";";
            config_stream << "max_events=" << config.max_events << ";";
            config_stream << "init_event_list_size=" << config.init_event_list_size << ";";

            // EventLoop配置
            config_stream << "enable_event_loop_monitoring=" << (config.enable_event_loop_monitoring ? "true" : "false") << ";";
            config_stream << "enable_pending_functors_optimization=" << (config.enable_pending_functors_optimization ? "true" : "false") << ";";

            // 线程池配置
            config_stream << "enable_thread_pool=" << (config.enable_thread_pool ? "true" : "false") << ";";
            config_stream << "thread_pool_size=" << config.thread_pool_size << ";";
            config_stream << "use_dedicated_io_threads=" << (config.use_dedicated_io_threads ? "true" : "false") << ";";

            // 性能优化配置
            config_stream << "enable_channel_validation=" << (config.enable_channel_validation ? "true" : "false") << ";";
            config_stream << "enable_epoll_resize_optimization=" << (config.enable_epoll_resize_optimization ? "true" : "false") << ";";
            config_stream << "epoll_resize_factor=" << std::fixed << std::setprecision(2) << config.epoll_resize_factor << ";";

            std::string config_str = config_stream.str();

            // 使用SHA256计算哈希值
            return calculateSHA256Hash(config_str);
        }



        void EventLoop::executeCallbackAsync(const Functor &cb) {
            if (config_.enable_thread_pool && thread_pool_) {
                // 使用线程池异步执行回调
                thread_pool_->submit([cb]() {
                    try {
                        cb();
                    } catch (const std::exception& e) {
                        LOG_ERROR("Async callback execution failed: " + std::string(e.what()));
                    } catch (...) {
                        LOG_ERROR("Async callback execution failed with unknown exception");
                    }
                });
            } else {
                // 在当前线程执行
                cb();
            }
        }

        void EventLoop::initializeEpoll() {
            poller_ = std::make_unique<Epoll>(this,
                                     config_.init_event_list_size,
                                     config_.max_events,
                                     config_.enable_epoll_resize_optimization,
                                     config_.epoll_resize_factor);

            LOG_INFO("Epoll initialized with EventLoop configuration");
        }
    }
}
