//
// Created by 29108 on 2025/7/5.
//

#include "common/thread_pool/thread_pool.h"
#include "common/logger/logger.h"

namespace common {
    namespace thread_pool {

        ThreadPool::ThreadPool(): ThreadPool(common::config::ThreadPoolConfig::fromConfigManager()) {
            LOG_INFO("[ThreadPool] Initialized with ConfigManager settings");
        }

        ThreadPool::ThreadPool(const common::config::ThreadPoolConfig& config)
                        :manager_config_(config),stop_(false), active_threads_(0) {
            manager_config_.validate();

            // 使用配置初始化线程池
            size_t thread_count = static_cast<size_t>(manager_config_.core_pool_size);

            // 创建工作线程
            workers_.reserve(thread_count);
            for (size_t i = 0; i < thread_count; ++i) {
                workers_.emplace_back([this] { workerThread(); });
            }

            // 如果启用了监控，启动监控线程
            if (manager_config_.enable_monitoring) {
                startMonitoring();
            }
        }

        ThreadPool::~ThreadPool() {
            try {
                if (!isShutdown()) {
                    shutdown();
                }
            } catch (const std::exception& e) {
                LOG_ERROR("[ThreadPool] Exception during destruction: " + std::string(e.what()));
                // 如果优雅关闭失败，强制关闭
                try {
                    forceShutdown();
                } catch (...) {
                    // 析构函数中不能抛出异常
                }
            } catch (...) {
                LOG_ERROR("[ThreadPool] Unknown exception during destruction");
                try {
                    forceShutdown();
                } catch (...) {
                    // 析构函数中不能抛出异常
                }
            }
        }

        size_t ThreadPool::getQueueSize() const {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            return tasks_.size();
        }

        size_t ThreadPool::getActiveThreadCount() const {
            return active_threads_.load();
        }

        size_t ThreadPool::getTotalThreadCount() const {
            return workers_.size();
        }

        void ThreadPool::shutdown() {
            // 1. 检查是否已经关闭，避免重复关闭
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                if (stop_) {
                    return;
                }
                stop_ = true;
            }

            // 2. 停止监控线程（如果启用）
            try {
                if (monitoring_enabled_.load()) {
                    stopMonitoring();
                }
            } catch (const std::exception& e) {
                LOG_ERROR("[ThreadPool] Error stopping monitoring: " + std::string(e.what()));
            }

            // 3. 通知所有工作线程停止
            condition_.notify_all();

            // 4. 等待工作线程完成

            // 将线程移到临时容器，避免在遍历时修改
            std::vector<std::thread> workers_to_join;
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);
                workers_to_join = std::move(workers_);
                workers_.clear();
            }

            int joined_count = 0;
            int failed_count = 0;

            for (auto& worker : workers_to_join) {
                if (worker.joinable()) {
                    try {
                        // 设置合理的等待时间
                        // 注意：C++ 标准线程没有 timed_join，所以直接 join
                        // 在生产环境中，应确保工作线程能快速响应停止信号
                        worker.join();
                        joined_count++;
                    } catch (const std::exception& e) {
                        failed_count++;
                        LOG_ERROR("[ThreadPool] Worker thread join failed: " + std::string(e.what()));
                        // 不再使用 detach，记录失败但让线程自然结束
                        // 如果线程仍在运行，它会在进程退出时被强制终止
                        // 这是比 detach 更安全的选择，因为 detach 会导致资源泄漏
                    } catch (...) {
                        failed_count++;
                        LOG_ERROR("[ThreadPool] Worker thread join failed with unknown exception");
                    }
                }
            }

            LOG_INFO("[ThreadPool] Thread shutdown summary - Joined: " + std::to_string(joined_count) +
                     ", Failed: " + std::to_string(failed_count));

            // 5. 清理队列中剩余的任务
            size_t remaining_tasks = 0;
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                remaining_tasks = tasks_.size();
                while (!tasks_.empty()) {
                    tasks_.pop();
                }
            }

            if (remaining_tasks > 0) {
                LOG_ERROR("[ThreadPool] Discarded " + std::to_string(remaining_tasks) +
                            " pending tasks during shutdown");
            }

            // 6. 重置统计信息
            active_threads_ = 0;
            core_threads_to_reduce_ = 0;
            threads_to_terminate_ = 0;

            LOG_INFO("[ThreadPool] Shutdown completed successfully");
        }

        void ThreadPool::forceShutdown() {
            LOG_ERROR("[ThreadPool] Force shutdown initiated");

            // 1. 立即设置停止标志
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                stop_ = true;
            }

            // 2. 强制停止监控
            try {
                if (monitoring_enabled_.load()) {
                    stopMonitoring();
                }
            } catch (...) {
                // 忽略强制关闭时的异常
            }

            // 3. 通知所有线程
            condition_.notify_all();

            // 4. 立即分离所有线程（不等待）
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.detach();
                }
            }

            // 5. 清理队列
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                size_t discarded = tasks_.size();
                while (!tasks_.empty()) {
                    tasks_.pop();
                }
                if (discarded > 0) {
                    LOG_ERROR("[ThreadPool] Force shutdown discarded " + std::to_string(discarded) + " pending tasks");
                }
            }

            // 6. 重置统计
            active_threads_ = 0;
            core_threads_to_reduce_ = 0;
            threads_to_terminate_ = 0;

            LOG_INFO("[ThreadPool] Force shutdown completed");
        }

        bool ThreadPool::isShutdown() const {
            return stop_.load();
        }

        void ThreadPool::enableConfigHotReload() {
            auto& config_manager = common::config::ConfigManager::getInstance();

            // 监听核心线程数变化
            config_manager.addChangeListener("thread_pool.core_pool_size",
                [this](const std::string& /*key*/, const std::string& old_val, const std::string& new_val) {
                    LOG_INFO("ThreadPool core_pool_size changed from " + old_val + " to " + new_val);
                    try {
                        int new_size = std::stoi(new_val);
                        this->adjustCorePoolSize(new_size);
                    } catch (const std::exception& e) {
                        LOG_ERROR("Failed to adjust core pool size: " + std::string(e.what()));
                    }
                });

            // 监听最大线程数变化
            config_manager.addChangeListener("thread_pool.maximum_pool_size",
                [this](const std::string& key, const std::string& old_val, const std::string& new_val) {
                    LOG_INFO("ThreadPool maximum_pool_size changed from " + old_val + " to " + new_val);
                    try {
                        int new_max_size = std::stoi(new_val);
                        this->adjustMaximumPoolSize(new_max_size);
                    } catch (const std::exception& e) {
                        LOG_ERROR("Failed to adjust maximum pool size: " + std::string(e.what()));
                    }
                });
        }

        void ThreadPool::workerThread() {
            while (true) {
                std::function<void()> task;

                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);

                    // 检查是否需要终止此线程
                    if (threads_to_terminate_ > 0) {
                        threads_to_terminate_--;
                        LOG_INFO("[ThreadPool] Worker thread terminating (scale down)");
                        return;
                    }

                    // 检查是否需要将核心线程转为非核心线程
                    if (core_threads_to_reduce_ > 0) {
                        core_threads_to_reduce_--;
                        // 这里可以实现核心线程转非核心线程的逻辑
                    }

                    // 等待任务或超时
                    condition_.wait_for(lock, std::chrono::milliseconds(manager_config_.keep_alive_time_ms), [this] {
                        return stop_ || !tasks_.empty();
                    });

                    if (stop_ && tasks_.empty()) {
                        return;
                    }

                    if (!tasks_.empty()) {
                        task = std::move(tasks_.front());
                        tasks_.pop();
                    }
                }

                if (task) {
                    active_threads_++;
                    try {
                        task();
                    } catch (const std::exception& e) {
                        LOG_ERROR("[ThreadPool] Task execution failed: " + std::string(e.what()));
                    } catch (...) {
                        LOG_ERROR("[ThreadPool] Task execution failed with unknown exception");
                    }
                    active_threads_--;
                }
            }
        }

        void ThreadPool::adjustCorePoolSize(int new_core_size) {
            if (new_core_size <= 0) {
                LOG_WARNING("Invalid core pool size: " + std::to_string(new_core_size));
                return;
            }

            if (new_core_size > manager_config_.maximum_pool_size) {
                LOG_WARNING("Core pool size (" + std::to_string(new_core_size) +
                           ") cannot exceed maximum pool size (" + std::to_string(manager_config_.maximum_pool_size) + ")");
                return;
            }

            std::lock_guard<std::mutex> lock(queue_mutex_);

            int old_core_size = manager_config_.core_pool_size;
            if (new_core_size == old_core_size) {
                LOG_DEBUG("Core pool size unchanged: " + std::to_string(new_core_size));
                return;
            }

            try {
                manager_config_.core_pool_size = new_core_size;

                if (new_core_size > old_core_size) {
                    // 扩容：创建新的核心线程
                    int threads_to_add = new_core_size - old_core_size;
                    for (int i = 0; i < threads_to_add; ++i) {
                        workers_.emplace_back([this] { workerThread(); });
                    }
                    LOG_INFO("Expanded core pool size from " + std::to_string(old_core_size) +
                            " to " + std::to_string(new_core_size));
                } else {
                    // 缩容：标记多余的核心线程为非核心线程
                    core_threads_to_reduce_ = old_core_size - new_core_size;
                    condition_.notify_all(); // 通知线程检查状态

                    LOG_INFO("Reduced core pool size from " + std::to_string(old_core_size) +
                            " to " + std::to_string(new_core_size));
                }

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to adjust core pool size: " + std::string(e.what()));
                manager_config_.core_pool_size = old_core_size; // 回滚
                throw;
            }
        }

        void ThreadPool::adjustMaximumPoolSize(int new_max_size) {
            if (new_max_size <= 0) {
                LOG_WARNING("Invalid maximum pool size: " + std::to_string(new_max_size));
                return;
            }

            if (new_max_size < manager_config_.core_pool_size) {
                LOG_WARNING("Maximum pool size (" + std::to_string(new_max_size) +
                           ") cannot be less than core pool size (" + std::to_string(manager_config_.core_pool_size) + ")");
                return;
            }

            std::lock_guard<std::mutex> lock(queue_mutex_);

            int old_max_size = manager_config_.maximum_pool_size;
            if (new_max_size == old_max_size) {
                LOG_DEBUG("Maximum pool size unchanged: " + std::to_string(new_max_size));
                return;
            }

            manager_config_.maximum_pool_size = new_max_size;

            if (new_max_size < old_max_size) {
                // 缩容：标记多余线程退出
                int current_thread_count = static_cast<int>(workers_.size());
                if (current_thread_count > new_max_size) {
                    threads_to_terminate_ = current_thread_count - new_max_size;
                    condition_.notify_all();
                }
                LOG_INFO("[ThreadPool] Reduced maximum pool size from " + std::to_string(old_max_size) +
                            " to " + std::to_string(new_max_size));
            } else {
                LOG_INFO("[ThreadPool] Increased maximum pool size from " + std::to_string(old_max_size) +
                            " to " + std::to_string(new_max_size));
            }
        }

        void ThreadPool::startMonitoring() {
            if (!manager_config_.enable_monitoring) {
                return;
            }

            monitoring_enabled_ = true;
            monitoring_thread_ = std::thread([this]() {
                while (monitoring_enabled_.load()) {
                    try {
                        reportPoolStatus();

                        // 使用更短的睡眠间隔，以便更快响应停止信号
                        auto interval = std::chrono::milliseconds(manager_config_.monitoring_interval_ms);
                        auto sleep_chunk = std::chrono::milliseconds(100); // 100ms chunks

                        while (interval > std::chrono::milliseconds(0) && monitoring_enabled_.load()) {
                            auto sleep_time = std::min(interval, sleep_chunk);
                            std::this_thread::sleep_for(sleep_time);
                            interval -= sleep_time;
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("[ThreadPool] Error in monitoring thread: " + std::string(e.what()));
                        // 发生异常时也要检查是否应该退出
                        if (!monitoring_enabled_.load()) {
                            break;
                        }
                    }
                }
            });
        }

        void ThreadPool::stopMonitoring() {
            monitoring_enabled_ = false;

            if (monitoring_thread_.joinable()) {
                try {
                    monitoring_thread_.join();
                } catch (const std::exception& e) {
                    LOG_ERROR("[ThreadPool] Error joining monitoring thread: " + std::string(e.what()));
                } catch (...) {
                    LOG_ERROR("[ThreadPool] Unknown error joining monitoring thread");
                }
            }
        }

        void ThreadPool::reportPoolStatus() {
            std::lock_guard<std::mutex> lock(queue_mutex_);

            int active_threads = active_threads_.load();
            int total_threads = static_cast<int>(workers_.size());
            int queue_size = static_cast<int>(tasks_.size());

            // 只在队列有任务或有活跃线程时报告状态
            if (queue_size > 0 || active_threads > 0) {
                LOG_INFO("[ThreadPool] Status - Active: " + std::to_string(active_threads)
                         + ", Total: " + std::to_string(total_threads)
                         + ", Queue: " + std::to_string(queue_size));
            }

            // 如果启用了线程转储
            if (manager_config_.enable_thread_dump && queue_size > manager_config_.queue_capacity * 0.8) {
                LOG_WARNING("[ThreadPool] Queue is " + std::to_string((queue_size * 100) / manager_config_.queue_capacity)
                         + "% full, consider increasing pool size");
            }
        }
    }
}