//
// Created by 29108 on 2025/7/5.
//

#ifndef THREAD_POOL_H
#define THREAD_POOL_H


#include <vector>
#include <queue>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <future>
#include <functional>
#include <stdexcept>
#include <atomic>
#include <chrono>
#include "common/config/config_manager.h"

namespace common {
    namespace thread_pool {
        class ThreadPool {
        public:
            explicit ThreadPool();
            explicit ThreadPool(const common::config::ThreadPoolConfig& config);
            ~ThreadPool();

            // 提交任务并返回future
            template<class F, class... Args>
            auto submit(F&& f, Args&&... args)
                -> std::future<typename std::result_of<F(Args...)>::type>;

            // 获取统计信息
            size_t getQueueSize() const;
            size_t getActiveThreadCount() const;
            size_t getTotalThreadCount() const;

            // 优雅关闭
            void shutdown();
            void forceShutdown();  // 强制立即关闭
            bool isShutdown() const;

            /**
            * @brief 启用配置热更新监听
            */
            void enableConfigHotReload();

            /**
             * @brief 动态调整核心线程池大小
             * @param new_core_size 新的核心线程数
             * @details 实现线程池核心线程数的动态调整
             */
            void adjustCorePoolSize(int new_core_size);

            /**
             * @brief 动态调整最大线程池大小
             * @param new_max_size 新的最大线程数
             */
            void adjustMaximumPoolSize(int new_max_size);

            /**
             * @brief 停止监控功能
             */
            void stopMonitoring();

        private:
            common::config::ThreadPoolConfig manager_config_;
            std::vector<std::thread> workers_;
            std::queue<std::function<void()>> tasks_;

            mutable std::mutex queue_mutex_;
            std::condition_variable condition_;
            std::atomic<bool> stop_;
            std::atomic<size_t> active_threads_;

            std::atomic<int> core_threads_to_reduce_{0};           ///< 需要减少的核心线程数
            std::atomic<int> threads_to_terminate_{0};             ///< 需要终止的线程数
            std::atomic<bool> monitoring_enabled_{false};          ///< 监控是否启用
            std::thread monitoring_thread_;                        ///< 监控线程

            /**
            * @brief 工作线程函数
            */
            void workerThread();

            /**
             * @brief 启动监控功能
             * @details 启动线程池监控线程，定期报告状态
             */
            void startMonitoring();

            /**
             * @brief 报告线程池状态
             */
            void reportPoolStatus();


        };

        template<class F, class ... Args>
        auto ThreadPool::submit(F &&f, Args &&...args) -> std::future<typename std::result_of<F(Args...)>::type> {

            using return_type = typename std::result_of<F(Args...)>::type;

            // 检查线程池是否已关闭
            if (stop_) {
                throw std::runtime_error("submit on stopped ThreadPool");
            }

            // 创建packaged_task来包装任务
            auto task = std::make_shared<std::packaged_task<return_type()>>(
                std::bind(std::forward<F>(f), std::forward<Args>(args)...)
            );

            std::future<return_type> res = task->get_future();

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                
                
                // 检查队列是否已满
                if (tasks_.size() >= static_cast<size_t>(manager_config_.queue_capacity)) {
                    if (manager_config_.rejection_policy == "abort") {
                        throw std::runtime_error("ThreadPool queue is full");
                    } else if (manager_config_.rejection_policy == "discard") {
                        // 直接丢弃任务
                        return res;
                    } else if (manager_config_.rejection_policy == "caller_runs") {
                        // 在调用者线程中执行任务
                        lock.unlock();
                        (*task)();
                        return res;
                    }
                }
                
                // 将任务添加到队列
                tasks_.emplace([task](){ (*task)(); });
            }

            // 通知一个等待的线程
            condition_.notify_one();
            return res;
        }

    }
}



#endif //THREAD_POOL_H
