//
// Created by 29108 on 2025/7/1.
//

#ifndef EVENT_LOOP_H
#define EVENT_LOOP_H

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include "epoll.h"
#include "common/config/config_manager.h"
#include "common/thread_pool/thread_pool.h"
#include "HierarchicalTimingWheel.h"
// #include "timer_queue.h"

namespace common {
    namespace network {
        /**
 * @brief 网络模块统一配置结构体
 * @details 基于现有网络类的实际需求设计的配置结构，整合了Socket、Epoll、EventLoop等组件的配置参数
 *
 * 设计原则：
 * - 基于现有代码中的硬编码值设计默认配置
 * - 支持从ConfigManager动态加载配置
 * - 提供配置验证确保参数合法性
 * - 支持线程池和性能优化的可选集成
 */
        struct networkConfig {
            // ==================== Socket配置 ====================
            /// struct NetworkConfig {@brief 服务器绑定地址，默认绑定所有网卡
            std::string bind_address = "0.0.0.0";

            /// @brief 服务器监听端口
            int port = 8081;

            /// @brief 监听队列长度，对应Socket::listen()中的硬编码值1024
            /// @details 决定了系统可以排队等待accept()的连接数量
            int backlog = 1024;

            /// @brief 是否启用SO_REUSEADDR选项，允许重用TIME_WAIT状态的地址
            /// @details 解决服务器重启时"Address already in use"错误
            bool reuse_address = true;

            /// @brief 是否启用SO_REUSEPORT选项，允许多个socket绑定同一地址
            /// @details 支持多进程/多线程服务器架构，需要Linux 3.9+内核支持
            bool reuse_port = false;

            /// @brief 是否启用TCP_NODELAY选项，禁用Nagle算法
            /// @details 启用后立即发送数据，降低延迟但可能增加网络包数量
            bool tcp_no_delay = true;

            /// @brief 是否启用SO_KEEPALIVE选项，启用TCP层保活机制
            /// @details 定期发送保活探测包检测连接状态，自动清理死连接
            bool socket_keep_alive = true;

            // Keep-Alive详细参数（对应Socket::configureKeepAliveParameters()）
            /// @brief TCP Keep-Alive空闲时间（秒），开始探测前的空闲时间
            /// @details 连接空闲多长时间后开始发送keep-alive探测包
            int keep_alive_idle_time = 600;

            /// @brief TCP Keep-Alive探测间隔（秒），探测包发送间隔
            /// @details 每次探测包之间的时间间隔
            int keep_alive_interval = 60;

            /// @brief TCP Keep-Alive最大探测次数
            /// @details 连续探测失败多少次后认为连接已断开
            int keep_alive_probes = 3;

            // ==================== Epoll配置 ====================
            /// @brief 是否启用epoll，通常应该保持true
            /// @details 控制是否使用epoll进行IO多路复用
            bool enable_epoll = true;

            /// @brief Epoll等待超时时间（毫秒），对应EventLoop::loop()中的10秒超时
            /// @details 控制epoll_wait()的超时时间，影响事件循环的响应性
            int epoll_timeout = 100;

            /// @brief Epoll最大事件数，对应Epoll中的MAX_EVENTS常量
            /// @details 限制单次epoll_wait()返回的最大事件数量
            int max_events = 1024;

            /// @brief Epoll初始事件列表大小，对应Epoll::KInitEventListSize常量
            /// @details 初始分配的epoll_event数组大小，影响内存使用和性能
            int init_event_list_size = 48;

            // ==================== EventLoop配置 ====================
            /// @brief 是否启用事件循环监控，控制调试日志输出
            /// @details 启用后会记录事件循环的详细运行信息
            bool enable_event_loop_monitoring = true;

            /// @brief eventfd创建标志，用于线程间通信的eventfd选项
            /// @details 控制createEventfd()函数中的标志位设置
            int wakeup_fd_flags = 0;

            /// @brief 是否启用待处理回调函数的优化
            /// @details 控制pendingFunctors的处理策略优化
            bool enable_pending_functors_optimization = true;

            // ==================== 线程池集成配置 ====================
            /// @brief 是否启用线程池处理回调，控制异步处理开关
            /// @details false时保持原有同步处理方式，true时启用异步处理
            bool enable_thread_pool = true;

            /// @brief 网络模块专用线程池大小
            /// @details 当enable_thread_pool为true时，创建的线程池线程数量
            int thread_pool_size = 4;

            /// @brief 是否使用专用IO线程，控制IO处理策略
            /// @details true时为IO操作分配专门的线程池
            bool use_dedicated_io_threads = false;

            // ==================== 性能优化配置 ====================
            /// @brief 是否启用Channel指针验证，控制安全检查
            /// @details 启用后在fillActiveChannels()中进行严格的指针验证
            bool enable_channel_validation = true;

            /// @brief 是否启用Epoll事件列表动态调整优化
            /// @details 控制events_数组的智能扩容机制
            bool enable_epoll_resize_optimization = true;

            /// @brief Epoll事件列表扩容因子，控制内存增长策略
            /// @details 当事件列表满载时，按此因子扩容（如1.5倍）
            double epoll_resize_factor = 1.5;

            /**
             * @brief 从ConfigManager加载网络配置
             * @return 加载完成的网络配置对象
             * @details 从统一配置管理器中读取网络相关配置，如果配置项不存在则使用默认值
             *
             * 配置项说明：
             * - network.* 前缀的配置项对应网络模块的各项参数
             * - 所有配置项都有合理的默认值，确保在配置缺失时系统仍能正常运行
             * - 配置加载失败不会抛出异常，而是使用默认值并记录警告日志
             */
            static networkConfig fromConfigManager() {
                auto& config = common::config::ConfigManager::getInstance();
                networkConfig result;

                // Socket配置 - 基础网络连接参数
                /// 服务器绑定地址，支持IPv4地址或主机名
                result.bind_address = config.get<std::string>("network.bind_address", "0.0.0.0");
                /// 服务器监听端口，范围1-65535
                result.port = config.get<int>("network.port", 8081);
                /// 监听队列长度，影响并发连接处理能力
                result.backlog = config.get<int>("network.backlog", 1024);
                /// SO_REUSEADDR选项，解决TIME_WAIT状态地址重用问题
                result.reuse_address = config.get<bool>("network.reuse_address", true);
                /// SO_REUSEPORT选项，支持多进程绑定同一端口（需内核支持）
                result.reuse_port = config.get<bool>("network.reuse_port", false);
                /// TCP_NODELAY选项，控制Nagle算法启用状态
                result.tcp_no_delay = config.get<bool>("network.tcp_no_delay", true);
                /// SO_KEEPALIVE选项，启用TCP层面的连接保活
                result.socket_keep_alive = config.get<bool>("network.socket_keep_alive", true);

                // Keep-Alive参数 - TCP连接保活的精细控制
                /// 空闲多长时间后开始发送keep-alive探测包（秒）
                result.keep_alive_idle_time = config.get<int>("network.keep_alive_idle_time", 600);
                /// keep-alive探测包的发送间隔（秒）
                result.keep_alive_interval = config.get<int>("network.keep_alive_interval", 60);
                /// 连续探测失败多少次后认为连接断开
                result.keep_alive_probes = config.get<int>("network.keep_alive_probes", 3);

                // Epoll配置 - IO多路复用相关参数
                /// 是否启用epoll，通常应保持true以获得最佳性能
                result.enable_epoll = config.get<bool>("network.enable_epoll", true);
                /// epoll_wait()超时时间，影响事件循环响应性（毫秒）
                result.epoll_timeout = config.get<int>("network.epoll_timeout", 1000);
                /// 单次epoll_wait()最大返回事件数，影响批处理效率
                result.max_events = config.get<int>("network.max_events", 1024);
                /// epoll事件数组初始大小，影响内存使用和扩容频率
                result.init_event_list_size = config.get<int>("network.init_event_list_size", 48);

                // EventLoop配置 - 事件循环行为控制
                /// 是否启用详细的事件循环监控日志
                result.enable_event_loop_monitoring = config.get<bool>("network.enable_event_loop_monitoring", true);
                /// 是否启用待处理回调函数队列的性能优化
                result.enable_pending_functors_optimization = config.get<bool>("network.enable_pending_functors_optimization", true);

                // 线程池集成配置 - 异步处理能力控制
                /// 是否启用线程池进行异步回调处理
                result.enable_thread_pool = config.get<bool>("network.enable_thread_pool", true);
                /// 网络模块专用线程池的线程数量
                result.thread_pool_size = config.get<int>("network.thread_pool_size", 4);
                /// 是否为IO操作使用专门的线程池
                result.use_dedicated_io_threads = config.get<bool>("network.use_dedicated_io_threads", false);

                // 性能优化配置 - 运行时优化开关
                /// 是否启用严格的Channel指针验证（调试时建议启用）
                result.enable_channel_validation = config.get<bool>("network.enable_channel_validation", true);
                /// 是否启用epoll事件数组的智能扩容优化
                result.enable_epoll_resize_optimization = config.get<bool>("network.enable_epoll_resize_optimization", true);
                /// epoll事件数组扩容时的增长因子
                result.epoll_resize_factor = config.get<double>("network.epoll_resize_factor", 1.5);

                return result;
            }

            /**
             * @brief 验证网络配置参数的合法性
             * @throws std::runtime_error 当配置参数不合法时抛出异常
             * @details 对所有关键配置参数进行合法性检查，确保系统能够正常运行
             *
             * 验证规则：
             * - 网络地址和端口必须有效
             * - 数值型参数必须在合理范围内
             * - 布尔型开关组合必须逻辑一致
             * - 性能参数必须符合系统限制
             */
            void validate() const {
                // 基础网络参数验证
                if (bind_address.empty()) {
                    throw std::runtime_error("Network bind_address cannot be empty");
                }

                // 端口范围验证：1-65535为有效端口范围
                if (port <= 0 || port > 65535) {
                    throw std::runtime_error("Invalid network port: " + std::to_string(port) +
                                           " (must be 1-65535)");
                }

                // 监听队列长度验证：必须为正数，通常不超过系统限制
                if (backlog <= 0) {
                    throw std::runtime_error("Network backlog must be > 0, got: " + std::to_string(backlog));
                }

                // Epoll参数验证
                if (max_events <= 0) {
                    throw std::runtime_error("Network max_events must be > 0, got: " +
                                           std::to_string(max_events));
                }

                if (init_event_list_size <= 0) {
                    throw std::runtime_error("Network init_event_list_size must be > 0, got: " +
                                           std::to_string(init_event_list_size));
                }

                if (epoll_timeout < 0) {
                    throw std::runtime_error("Network epoll_timeout must be >= 0, got: " +
                                           std::to_string(epoll_timeout));
                }

                // 线程池参数验证
                if (thread_pool_size <= 0) {
                    throw std::runtime_error("Network thread_pool_size must be > 0, got: " +
                                           std::to_string(thread_pool_size));
                }

                // 性能优化参数验证
                if (epoll_resize_factor <= 1.0) {
                    throw std::runtime_error("Network epoll_resize_factor must be > 1.0, got: " +
                                           std::to_string(epoll_resize_factor));
                }

                // Keep-Alive参数验证：所有时间参数必须为正数
                if (keep_alive_idle_time <= 0) {
                    throw std::runtime_error("Invalid keep_alive_idle_time: " +
                                           std::to_string(keep_alive_idle_time) + " (must be > 0)");
                }

                if (keep_alive_interval <= 0) {
                    throw std::runtime_error("Invalid keep_alive_interval: " +
                                           std::to_string(keep_alive_interval) + " (must be > 0)");
                }

                if (keep_alive_probes <= 0) {
                    throw std::runtime_error("Invalid keep_alive_probes: " +
                                           std::to_string(keep_alive_probes) + " (must be > 0)");
                }

                // 逻辑一致性验证
                if (enable_thread_pool && thread_pool_size <= 0) {
                    throw std::runtime_error("Thread pool enabled but thread_pool_size <= 0");
                }

                if (!enable_epoll && enable_epoll_resize_optimization) {
                    throw std::runtime_error("Epoll resize optimization enabled but epoll is disabled");
                }
            }
        };


        class Channel;

        class EventLoop {
        public:
            typedef std::function<void()> Functor;
            typedef std::vector<Channel*> ChannelList;
            /// @brief 线程池工厂回调函数类型
            using ThreadPoolFactoryCallback = std::function<std::shared_ptr<common::thread_pool::ThreadPool>(int)>;

            /// @brief 配置变更通知回调函数类型
            using ConfigChangeCallback = std::function<void(const std::string&, const std::string&)>;

            /// @brief 配置更新事件回调函数类型
            using ConfigUpdateEventCallback = std::function<void(const networkConfig&, const networkConfig&)>;

            EventLoop();
            ~EventLoop();

            /**
            * @brief 配置构造函数 - 企业级推荐方式
            * @param config 网络配置对象
            * @details 在业务协调层接收配置，并向下层组件传递必要参数
            */
            explicit EventLoop(const networkConfig& config);

            /**
             * @brief 配置构造函数 - 支持线程池注入
             * @param config 网络配置对象
             * @param thread_pool 外部线程池（可选）
             * @details 支持外部注入线程池，实现资源共享
             */
            EventLoop(const networkConfig& config,std::shared_ptr<common::thread_pool::ThreadPool> thread_pool);

            //启动事件循环
            void loop();

            //退出事件循环
            void quit();

            // 检查事件循环是否正在运行
            bool isLooping() const { return looping_.load(); }

            //在IO线程中执行回调
            void runInLoop(const Functor& cb);

            //在下一次事件循环中执行回调
            void queueInLoop(const Functor& cb);

            // 添加定时器接口（透传给时间轮）
            uint64_t runAfter(int ms, std::function<void()> cb);
            uint64_t runEvery(int ms, std::function<void()> cb);
            void cancelTimer(uint64_t id);

            // 唤醒事件循环
            void wakeup();

            // 更新Channel
            void updateChannel(Channel* channel);
            void removeChannel(Channel* channel);

            // 检查是否有指定Channel
            bool hasChannel(Channel* channel);

            // 🔧 添加获取线程池的方法
            std::shared_ptr<common::thread_pool::ThreadPool> getThreadPool() const { return thread_pool_; }
            
            // 🔧 添加获取配置的方法
            const networkConfig& getConfig() const { return config_; }

            // 检查是否在当前线程
            bool isInLoopThread() const;

            // 断言在循环线程中
            void assertInLoopThread() const {
                if (!isInLoopThread()) {
                    abortNotInLoopThread();
                }
            }

            /**
            * @brief 设置线程池 - 支持运行时配置
             * @param pool 线程池实例
            * @details 允许运行时设置或更换线程池
            */
            void setThreadPool(std::shared_ptr<common::thread_pool::ThreadPool> pool);

            /**
            * @brief 启用网络配置热更新
            * @param config_file_path 配置文件路径
            * @param check_interval 检查间隔（默认5秒）
            * @details 监听网络相关配置的变化，自动应用新配置
            */
            void enableNetworkConfigHotReload(const std::string& config_file_path,
                                             std::chrono::seconds check_interval = std::chrono::seconds(5));

            /**
             * @brief 禁用网络配置热更新
             */
            void disableNetworkConfigHotReload();

            /**
             * @brief 手动更新网络配置
             * @param new_config 新的网络配置
             * @details 安全地更新网络配置，保持连接状态
             */
            void updateNetworkConfig(const networkConfig& new_config);

            /**
            * @brief 设置线程池工厂回调
             * @param callback 线程池工厂回调函数
             * @details 当配置要求创建线程池时，会调用此回调函数
            */
            void setThreadPoolFactoryCallback(ThreadPoolFactoryCallback callback);

            /**
             * @brief 设置配置变更通知回调
             * @param callback 配置变更通知回调函数
             * @details 当配置变更需要外部处理时，会调用此回调函数
             */
            void setConfigChangeCallback(ConfigChangeCallback callback);

            /**
             * @brief 设置配置更新事件回调
             * @param callback 配置更新事件回调函数
             * @details 当配置更新完成时，会调用此回调函数
             */
            void setConfigUpdateEventCallback(ConfigUpdateEventCallback callback);

            /**
            * @brief 异步执行回调函数
            * @param cb 回调函数
            * @details 根据配置决定是否使用线程池异步执行
            */
            void executeCallbackAsync(const Functor& cb);

        private:

            /// @brief 配置变更分析结构体
            struct ConfigChangeAnalysis {
                bool epoll_config_changed = false;
                bool socket_config_changed = false;
                bool thread_pool_config_changed = false;
                bool monitoring_config_changed = false;
                bool network_binding_changed = false;
                bool requires_restart = false;
            };

            /// @brief 线程池工厂回调
            ThreadPoolFactoryCallback thread_pool_factory_callback_;

            /// @brief 配置变更通知回调
            ConfigChangeCallback config_change_callback_;

            /// @brief 配置更新事件回调
            ConfigUpdateEventCallback config_update_event_callback_;


            // ✅ 配置管理：业务协调层持有配置
            networkConfig config_;

            // ✅ 资源管理：统一管理线程池资源
            std::shared_ptr<common::thread_pool::ThreadPool> thread_pool_;

            std::atomic<bool> looping_;  // 事件循环运行标志
            std::atomic<bool> quit_;      // 退出标志
            mutable std::atomic<std::thread::id> threadId_;  // 绑定线程ID
            std::unique_ptr<Epoll> poller_;   // IO多路复用器

            int wakeupFd_;  // 唤醒文件描述符
            std::unique_ptr<Channel> wakeupChannel_;  // 唤醒通道

            ChannelList activeChannels_;  // 活跃通道列表
            std::mutex mutex_;  // 互斥锁（保护pendingFunctors_）
            std::vector<Functor> pendingFunctors_;  // 待执行回调队列
            std::atomic<bool> callingPendingFunctors_;  // 回调执行标志

            /// @brief 配置热更新相关成员
            std::atomic<bool> config_hot_reload_enabled_{false};
            std::string config_file_path_;
            std::chrono::seconds config_check_interval_{5};

            /// @brief 配置变更监听器ID列表
            std::vector<std::string> config_listener_keys_;

            // 时间轮实例
            std::unique_ptr<timer::HierarchicalTimingWheel> timing_wheel_;

            // 用于时间轮的 timerfd
            int timerFd_; // 使用 timerfd_create 创建的文件描述符
            std::unique_ptr<Channel> timerChannel_; // 用于监听 timerfd 的 Channel


            void initTimer();



            /**
             * @brief 初始化Epoll - 使用配置参数
             * @details 根据配置创建Epoll实例，体现依赖注入原则
             */
            void initializeEpoll();

            //处理唤醒
            void handleRead();

            void handleTimerfdRead();

            void setInitialTimerfdTimeout();

            void resetTimerfd();

            //执行待处理回调
            void doPendingFunctors();

            //错误处理
            void abortNotInLoopThread() const;

            /**
             * @brief 网络配置变更回调函数
             * @param key 配置键
             * @param old_value 旧值
             * @param new_value 新值
             */
            void onNetworkConfigChanged(const std::string& key,
                                       const std::string& old_value,
                                       const std::string& new_value);

            /**
             * @brief 安全地重新配置Epoll
             * @param new_config 新配置
             * @details 在不中断服务的情况下更新Epoll配置
             */
            void safeReconfigureEpoll(const networkConfig& new_config);

            /**
             * @brief 应用Socket配置到现有连接
             * @param new_config 新配置
             */
            void applySocketConfigToExistingConnections(const networkConfig& new_config);

            /**
            * @brief 应用Socket配置到指定文件描述符
            * @param fd 文件描述符
            * @param new_config 新配置
            */
            void applySocketConfigToFd(int fd, const networkConfig& new_config);

            /**
             * @brief 应用Keep-Alive参数到指定文件描述符
             * @param fd 文件描述符
             * @param config 配置参数
             */
            void applyKeepAliveParams(int fd, const networkConfig& config);


            /**
             * @brief 验证配置变更的安全性
             * @param old_config 旧配置
             * @param new_config 新配置
             * @return 是否可以安全应用配置
             */
            bool isConfigChangeSafe(const networkConfig& old_config,
                                   const networkConfig& new_config) const;

            /**
            * @brief 分析配置变更
            * @param old_config 旧配置
            * @param new_config 新配置
            * @return 配置变更分析结果
            */
            ConfigChangeAnalysis analyzeConfigChanges(const networkConfig& old_config,
                                                     const networkConfig& new_config) const;

            /**
             * @brief 按优先级顺序应用配置变更
             * @param old_config 旧配置
             * @param new_config 新配置
             * @param changes 变更分析结果
             */
            void applyConfigChangesInOrder(const networkConfig& old_config,
                                          const networkConfig& new_config,
                                          const ConfigChangeAnalysis& changes);

            /**
             * @brief 应用监控配置变更
             */
            void applyMonitoringConfigChanges(const networkConfig& old_config,
                                             const networkConfig& new_config);

            /**
             * @brief 应用Socket配置变更
             */
            void applySocketConfigChanges(const networkConfig& old_config,
                                         const networkConfig& new_config);

            /**
             * @brief 应用线程池配置变更
             */
            void applyThreadPoolConfigChanges(const networkConfig& old_config,
                                             const networkConfig& new_config);

            /**
             * @brief 应用Epoll配置变更
             */
            void applyEpollConfigChanges(const networkConfig& old_config,
                                        const networkConfig& new_config);

            /**
             * @brief 记录配置变更详情
             */
            void logConfigChanges(const networkConfig& old_config,
                                 const networkConfig& new_config,
                                 const ConfigChangeAnalysis& changes) const;

            /**
             * @brief 通知配置更新完成
             */
            void notifyConfigUpdateComplete(const networkConfig& old_config,
                                           const networkConfig& new_config);

            std::string calculateSHA256Hash(const std::string & string) const;

            /**
             * @brief 计算配置哈希值（用于审计）
             */
            std::string calculateConfigHash(const networkConfig& config) const;
        };
    }
}

#endif //EVENT_LOOP_H
