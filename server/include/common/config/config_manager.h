/**
 * @file config_manager.h
 * @brief 统一配置管理器 - 支持多种配置格式和热重载
 * @author AI Assistant
 * @date 2025/7/6
 * @version 1.0
 *
 * 功能特性：
 * - 支持YAML、JSON、环境变量等多种配置源
 * - 线程安全的配置读写操作
 * - 配置变更监听和回调机制
 * - 配置热重载功能
 * - 类型安全的配置值转换
 * - 配置验证和错误处理
 */

#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

// 防止宏冲突，在包含系统头文件之前取消可能冲突的宏定义
#ifdef DEBUG
#undef DEBUG
#endif
#ifdef INFO
#undef INFO
#endif
#ifdef WARNING
#undef WARNING
#endif
#ifdef ERROR
#undef ERROR
#endif
#ifdef TRACE
#undef TRACE
#endif
#ifdef FATAL
#undef FATAL
#endif

#include <string>
#include <map>
#include <vector>
#include <shared_mutex>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <algorithm>    // 用于std::transform
#include <functional>   // 用于std::function
#include <atomic>       // 用于std::atomic
#include <thread>       // 用于std::thread
#include <chrono>       // 用于时间相关操作
#include <memory>
#include <yaml-cpp/yaml.h>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <mutex>

namespace YAML {
    class Node;
}

namespace common {
    namespace config {

        /**
         * @brief 统一配置管理器类
         *
         * 提供线程安全的配置管理功能，支持多种配置源和热重载。
         * 使用单例模式确保全局唯一性。
         */
        class ConfigManager {
        public:
            static ConfigManager& getInstance();

            /**
             * @brief 析构函数
             *
             * 安全地清理所有资源，包括：
             * - 停止热重载监控线程
             * - 清除所有配置变更监听器
             * - 清空配置数据
             * - 重置所有状态变量
             *
             * 注意：析构函数不会抛出异常，所有错误都会被记录到日志中
             */
            ~ConfigManager();

            // 禁用拷贝构造和赋值操作（单例模式）
            ConfigManager(const ConfigManager&) = delete;
            ConfigManager& operator=(const ConfigManager&) = delete;
            ConfigManager(ConfigManager&&) = delete;
            ConfigManager& operator=(ConfigManager&&) = delete;

            // 配置文件查找
            /**
             * @brief 智能查找配置文件
             * @param filename 配置文件名（如 "service_registry.yml"）
             * @param env_var 环境变量名（可选，如 "SERVICE_REGISTRY_CONFIG"）
             * @return 找到的绝对路径，未找到返回空字符串
             *
             * 查找优先级：
             * 1. 绝对路径直接检查
             * 2. 环境变量指定路径
             * 3. 从可执行文件目录向上遍历查找 config/<filename>
             * 4. 当前工作目录的相对路径候选
             * 5. 标准部署路径
             */
            static std::string findConfigFile(const std::string& filename,
                                              const std::string& env_var = "");

            // 配置文件加载
            bool loadFromFile(const std::string& file_path);
            bool loadFromYaml(const std::string& file_path);
            bool loadFromJson(const std::string& file_path);
            bool loadFromEnv();

            // 配置值操作
            template<typename T>
            T get(const std::string& key, const T& default_value = T{}) const;

            template<typename T>
            void set(const std::string& key, const T& value);

            bool has(const std::string& key) const;
            void remove(const std::string& key);
            void clear();

            // 批量操作
            std::map<std::string, std::string> getAllConfigs() const;
            std::map<std::string, std::string> getConfigsByPrefix(const std::string& prefix) const;

            // 配置监听
            using ChangeListener = std::function<void(const std::string& key, const std::string& old_value, const std::string& new_value)>;
            void addChangeListener(const std::string& key, ChangeListener listener);
            void removeChangeListeners(const std::string& key);

            // 配置验证
            bool validate() const;
            std::vector<std::string> getValidationErrors() const;

            // 配置持久化
            bool saveToFile(const std::string& file_path) const;
            bool saveToYaml(const std::string& file_path) const;
            bool saveToJson(const std::string& file_path) const;

            // 配置热重载
            void enableHotReload(const std::string& file_path, std::chrono::seconds check_interval = std::chrono::seconds(5));
            void disableHotReload();

            // 配置管理操作
            bool reload();
            void reset();

        private:
            /**
             * @brief 私有构造函数（单例模式）
             *
             * 初始化配置管理器的所有成员变量，设置默认配置值。
             * 包括：
             * - 初始化热重载相关状态
             * - 设置默认系统配置
             * - 初始化时间戳
             */
            ConfigManager();



            std::map<std::string, std::string> configs_;
            mutable std::shared_mutex config_mutex_;

            // 配置监听相关
            std::map<std::string, std::vector<ChangeListener>> listeners_;  // 修正变量名和类型
            mutable std::shared_mutex listeners_mutex_;  // 修正锁类型

            // 热重载相关
            std::atomic<bool> hot_reload_enabled_;
            std::filesystem::file_time_type last_file_write_time_;  // 修正时间类型
            std::chrono::seconds hot_reload_check_interval_;  // 修正变量名
            std::string hot_reload_file_path_;  // 修正变量名
            std::thread hot_reload_thread_;

            void notifyConfigChange(const std::string& key, const std::string& old_value, const std::string& new_value);
            void hotReloadWorker();
            std::filesystem::file_time_type getFileModificationTime(const std::string& file_path);

            // 配置验证辅助方法
            bool validateSpecificConfigFormats() const;
            void validateNumericConfigs(std::vector<std::string>& errors) const;
            void validateBooleanConfigs(std::vector<std::string>& errors) const;
            void validateFilePathConfigs(std::vector<std::string>& errors) const;
            void validateNetworkConfigs(std::vector<std::string>& errors) const;

            // 配置解析辅助方法
            long long parseSizeValue(const std::string& value) const;

            // 配置文件解析方法 - 缺少的函数声明
            void loadYamlNode(const YAML::Node& node, const std::string& prefix);
            void loadJsonNode(const nlohmann::json& node, const std::string& prefix);
            std::string envKeyToConfigKey(const std::string& env_key) const;

            // 工具方法
            bool isValidConfigKey(const std::string& key) const;
            std::string sanitizeConfigValue(const std::string& value) const;

            // 验证辅助方法
            bool isEmptyValueAllowed(const std::string& key) const;
            std::vector<std::string> getRequiredConfigKeys() const;
            bool validateConfigValues() const;
            std::vector<std::string> getConfigValueValidationErrors() const;

            // 文件格式辅助方法
            void setYamlValue(YAML::Node& node, const std::string& key, const std::string& value) const;
            void setJsonValue(nlohmann::json& node, const std::string& key, const std::string& value) const;

            // 类型转换辅助函数
            template<typename T>
            T convertFromString(const std::string& str) const;

            template<typename T>
            std::string convertToString(const T& value) const;

            // 字符数组特化声明
            template<size_t N>
            std::string convertToString(const char (&value)[N]) const;
        };

        // 模板方法实现
        template<typename T>
        T ConfigManager::get(const std::string& key, const T& default_value) const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);

            auto it = configs_.find(key);
            if (it == configs_.end()) {
                return default_value;
            }

            try {
                return convertFromString<T>(it->second);
            } catch (const std::exception& e) {
                // LOG_WARNING("Failed to convert config value for key '" + key + "': " + e.what() );
                return default_value;
            }
        }

        template<typename T>
        void ConfigManager::set(const std::string& key, const T& value) {
            std::string str_value = convertToString(value);
            std::string old_value;

            {
                std::unique_lock<std::shared_mutex> lock(config_mutex_);
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    old_value = it->second;
                }
                configs_[key] = str_value;
            }

            notifyConfigChange(key, old_value, str_value);
        }

        // 类型转换特化
        template<>
        inline std::string ConfigManager::convertFromString<std::string>(const std::string& str) const {
            return str;
        }

        template<>
        inline int ConfigManager::convertFromString<int>(const std::string& str) const {
            return std::stoi(str);
        }

        template<>
        inline long ConfigManager::convertFromString<long>(const std::string& str) const {
            return std::stol(str);
        }

        template<>
        inline long long ConfigManager::convertFromString<long long>(const std::string& str) const {
            return std::stoll(str);
        }

        template<>
        inline double ConfigManager::convertFromString<double>(const std::string& str) const {
            return std::stod(str);
        }

        template<>
        inline bool ConfigManager::convertFromString<bool>(const std::string& str) const {
            std::string lower_str = str;
            std::transform(lower_str.begin(), lower_str.end(), lower_str.begin(), ::tolower);
            return lower_str == "true" || lower_str == "1" || lower_str == "yes" || lower_str == "on";
        }

        template<>
        inline uint16_t ConfigManager::convertFromString<uint16_t>(const std::string& str) const {
            try {
                unsigned long result = std::stoul(str);
                if (result > std::numeric_limits<uint16_t>::max()) {
                    throw std::out_of_range("Value too large for uint16_t");
                }
                return static_cast<uint16_t>(result);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to uint16_t: " + e.what());
            }
        }

        template<>
        inline uint32_t ConfigManager::convertFromString<uint32_t>(const std::string& str) const {
            try {
                unsigned long result = std::stoul(str);
                if (result > std::numeric_limits<uint32_t>::max()) {
                    throw std::out_of_range("Value too large for uint32_t");
                }
                return static_cast<uint32_t>(result);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to uint32_t: " + e.what());
            }
        }

        template<>
        inline uint64_t ConfigManager::convertFromString<uint64_t>(const std::string& str) const {
            try {
                return std::stoull(str);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to uint64_t: " + e.what());
            }
        }

        template<>
        inline std::chrono::seconds ConfigManager::convertFromString<std::chrono::seconds>(const std::string& str) const {
            try {
                int seconds = std::stoi(str);
                return std::chrono::seconds(seconds);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to std::chrono::seconds: " + e.what());
            }
        }

        template<>
        inline std::chrono::minutes ConfigManager::convertFromString<std::chrono::minutes>(const std::string& str) const {
            try {
                int minutes = std::stoi(str);
                return std::chrono::minutes(minutes);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to std::chrono::minutes: " + e.what());
            }
        }

        template<>
        inline std::chrono::milliseconds ConfigManager::convertFromString<std::chrono::milliseconds>(const std::string& str) const {
            try {
                int milliseconds = std::stoi(str);
                return std::chrono::milliseconds(milliseconds);
            } catch (const std::exception& e) {
                throw std::invalid_argument("Cannot convert '" + str + "' to std::chrono::milliseconds: " + e.what());
            }
        }

        template<>
        inline std::vector<std::string> ConfigManager::convertFromString<std::vector<std::string>>(const std::string& str) const {
            std::vector<std::string> result;

            if (str.empty()) {
                return result;
            }

            std::stringstream ss(str);
            std::string item;

            while (std::getline(ss, item, ',')) {
                // 去除前后空格
                item.erase(0, item.find_first_not_of(" \t"));
                item.erase(item.find_last_not_of(" \t") + 1);

                if (!item.empty()) {
                    result.push_back(item);
                }
            }

            return result;
        }

        template<>
        inline std::vector<int> ConfigManager::convertFromString<std::vector<int>>(const std::string& str) const {
            std::vector<int> result;

            if (str.empty()) {
                return result;
            }

            std::stringstream ss(str);
            std::string item;

            while (std::getline(ss, item, ',')) {
                // 去除前后空格
                item.erase(0, item.find_first_not_of(" \t\r\n"));
                item.erase(item.find_last_not_of(" \t\r\n") + 1);

                if (!item.empty()) {
                    try {
                        // 检查是否为有效的整数格式
                        size_t pos;
                        int value = std::stoi(item, &pos);

                        // 确保整个字符串都被解析
                        if (pos != item.length()) {
                            throw std::invalid_argument("Invalid integer format: '" + item + "'");
                        }

                        result.push_back(value);
                    } catch (const std::invalid_argument& e) {
                        throw std::invalid_argument("Cannot convert '" + item + "' to int in vector: " + e.what());
                    } catch (const std::out_of_range& e) {
                        throw std::out_of_range("Integer out of range: '" + item + "' in vector: " + e.what());
                    }
                }
            }

            return result;
        }

        // ==================== convertToString模板特化 ====================

        template<>
        inline std::string ConfigManager::convertToString<std::string>(const std::string& value) const {
            return value;
        }

        template<>
        inline std::string ConfigManager::convertToString<int>(const int& value) const {
            return std::to_string(value);
        }

        template<>
        inline std::string ConfigManager::convertToString<long>(const long& value) const {
            return std::to_string(value);
        }

        template<>
        inline std::string ConfigManager::convertToString<long long>(const long long& value) const {
            return std::to_string(value);
        }

        template<>
        inline std::string ConfigManager::convertToString<double>(const double& value) const {
            return std::to_string(value);
        }

        template<>
        inline std::string ConfigManager::convertToString<bool>(const bool& value) const {
            return value ? "true" : "false";
        }

        // const char* 特化
        template<>
        inline std::string ConfigManager::convertToString<const char*>(const char* const& value) const {
            return std::string(value ? value : "");
        }

        // 字符数组特化实现
        template<size_t N>
        inline std::string ConfigManager::convertToString(const char (&value)[N]) const {
            return std::string(value);
        }

        // ==================== 配置结构体定义 ====================

        /**
         * @brief MySQL数据库配置结构体
         */
        struct DatabaseConfig {
            // 基本连接配置
            std::string mysql_host = "127.0.0.1";      ///< MySQL服务器主机地址
            int mysql_port = 3306;                     ///< MySQL服务器端口
            std::string mysql_user = "root";                    ///< MySQL用户名
            std::string mysql_password = "123456";                ///< MySQL密码
            std::string mysql_database = "game_db";                ///< MySQL数据库名 (默认值，应通过配置文件覆盖)
            std::string mysql_charset = "utf8mb4";     ///< 字符集
            std::string mysql_collation = "utf8mb4_unicode_ci"; ///< 排序规则

            bool enable_async_operations = true;
            int thread_pool_core_size = 4;                         ///< 线程池核心线程数
            int thread_pool_max_size = 16;                         ///< 线程池最大线程数
            int thread_pool_keep_alive_ms = 60000;                 ///< 线程保活时间(毫秒)
            int thread_pool_queue_capacity = 1000;                 ///< 任务队列容量
            bool thread_pool_enable_monitoring = true;             ///< 是否启用线程池监控

            // 连接池配置
            size_t mysql_pool_size = 10;               ///< MySQL连接池初始大小
            size_t mysql_max_pool_size = 50;           ///< MySQL连接池最大大小
            size_t mysql_min_pool_size = 5;            ///< MySQL连接池最小大小
            int mysql_pool_timeout_ms = 30000;         ///< 获取连接超时时间（毫秒）
            int mysql_pool_max_idle_time_ms = 600000;  ///< 连接最大空闲时间（10分钟）
            bool mysql_pool_test_on_borrow = true;     ///< 获取连接时是否测试
            bool mysql_pool_test_on_return = false;    ///< 归还连接时是否测试
            bool mysql_pool_test_while_idle = true;    ///< 空闲时是否测试连接
            long mysql_pool_validation_interval_ms = 30000; ///< 连接验证间隔（毫秒）

            // 连接参数
            int mysql_connect_timeout_ms = 10000;      ///< 连接超时时间（毫秒）
            int mysql_read_timeout_ms = 30000;         ///< 读取超时时间（毫秒）
            int mysql_write_timeout_ms = 30000;        ///< 写入超时时间（毫秒）
            bool mysql_auto_reconnect = true;          ///< 是否自动重连
            int mysql_max_reconnect_attempts = 3;      ///< 最大重连次数
            int mysql_reconnect_interval_ms = 1000;    ///< 重连间隔（毫秒）

            // SSL配置
            bool mysql_use_ssl = false;                ///< 是否使用SSL
            std::string mysql_ssl_ca;                  ///< SSL CA证书路径
            std::string mysql_ssl_cert;                ///< SSL客户端证书路径
            std::string mysql_ssl_key;                 ///< SSL私钥路径
            std::string mysql_ssl_cipher;              ///< SSL加密套件
            bool mysql_ssl_verify_server_cert = true;  ///< 是否验证服务器证书

            // 性能配置
            int mysql_max_allowed_packet = 16777216;   ///< 最大数据包大小（16MB）
            int mysql_net_buffer_length = 32768;       ///< 网络缓冲区长度（32KB）
            bool mysql_compress = false;               ///< 是否启用压缩
            std::string mysql_init_command;            ///< 初始化命令

            // 事务配置
            std::string mysql_isolation_level = "REPEATABLE-READ"; ///< 事务隔离级别
            bool mysql_autocommit = true;              ///< 是否自动提交
            int mysql_lock_wait_timeout = 50;          ///< 锁等待超时时间（秒）

            /**
             * @brief 从配置管理器加载数据库配置
             * @return 数据库配置实例
             */
            static DatabaseConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief Redis配置结构体
         */
        struct RedisConfig {
            // 基本连接配置
            std::string redis_host = "127.0.0.1";          ///< Redis服务器主机地址
            int redis_port = 6379;                     ///< Redis服务器端口
            std::string redis_password = "123456";                ///< Redis密码
            int redis_database = 0;                    ///< Redis数据库索引

            // ==================== 线程池集成配置 ====================
            bool enable_async_operations = true;                    ///< 是否启用异步操作
            int thread_pool_core_size = 4;                         ///< 线程池核心线程数
            int thread_pool_max_size = 16;                         ///< 线程池最大线程数
            int thread_pool_keep_alive_ms = 60000;                 ///< 线程保活时间(毫秒)
            int thread_pool_queue_capacity = 1000;                 ///< 任务队列容量
            bool thread_pool_enable_monitoring = true;             ///< 是否启用线程池监控

            // 连接池配置
            size_t redis_pool_size = 5;                ///< Redis连接池初始大小
            size_t redis_max_pool_size = 20;           ///< Redis连接池最大大小
            size_t redis_min_pool_size = 2;            ///< Redis连接池最小大小
            int redis_pool_timeout_ms = 5000;          ///< 获取连接超时时间（毫秒）
            int redis_pool_max_idle_time_ms = 300000;  ///< 连接最大空闲时间（5分钟）
            bool redis_pool_test_on_borrow = true;     ///< 获取连接时是否测试
            bool redis_pool_test_on_return = false;    ///< 归还连接时是否测试
            int redis_pool_validation_interval_ms = 30000;

            // 连接参数
            int redis_connect_timeout_ms = 5000;       ///< 连接超时时间（毫秒）
            int redis_socket_timeout_ms = 3000;        ///< Socket超时时间（毫秒）
            int redis_command_timeout_ms = 3000;       ///< 命令超时时间（毫秒）
            bool redis_tcp_keepalive = true;           ///< 是否启用TCP keepalive
            int redis_tcp_keepalive_idle = 600;        ///< TCP keepalive空闲时间（秒）
            int redis_tcp_keepalive_interval = 30;     ///< TCP keepalive间隔（秒）
            int redis_tcp_keepalive_count = 3;         ///< TCP keepalive探测次数

            // 重试和恢复配置
            bool redis_auto_reconnect = true;          ///< 是否自动重连
            int redis_max_reconnect_attempts = 3;      ///< 最大重连次数
            int redis_reconnect_interval_ms = 1000;    ///< 重连间隔（毫秒）
            int redis_retry_attempts = 3;              ///< 命令重试次数
            int redis_retry_delay_ms = 100;            ///< 重试延迟（毫秒）

            // 集群配置
            bool redis_cluster_enabled = false;        ///< 是否启用集群模式
            std::vector<std::string> redis_cluster_nodes; ///< 集群节点列表
            int redis_cluster_max_redirections = 5;    ///< 最大重定向次数
            bool redis_cluster_require_full_coverage = true; ///< 是否要求完整覆盖

            // 哨兵配置
            bool redis_sentinel_enabled = false;       ///< 是否启用哨兵模式
            std::vector<std::string> redis_sentinel_hosts; ///< 哨兵主机列表
            std::string redis_sentinel_master_name;    ///< 哨兵主服务器名称
            std::string redis_sentinel_password;       ///< 哨兵密码
            int redis_sentinel_timeout_ms = 5000;      ///< 哨兵超时时间（毫秒）

            // SSL/TLS配置
            bool redis_use_ssl = false;                ///< 是否使用SSL/TLS
            std::string redis_ssl_cert_file;           ///< SSL证书文件路径
            std::string redis_ssl_key_file;            ///< SSL私钥文件路径
            std::string redis_ssl_ca_file;             ///< SSL CA证书文件路径
            bool redis_ssl_verify_peer = true;         ///< 是否验证对等证书
            std::string redis_ssl_ciphers;             ///< SSL加密套件

            // 性能配置
            int redis_pipeline_size = 100;             ///< Pipeline批处理大小
            bool redis_enable_pipeline = true;         ///< 是否启用Pipeline
            int redis_max_memory_policy = 0;           ///< 内存策略（0=noeviction）
            bool redis_compress_data = false;          ///< 是否压缩数据

            /**
             * @brief 从配置管理器加载Redis配置
             * @return Redis配置实例
             */
            static RedisConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief 服务器配置结构体
         */
        struct ServerConfig {
            std::string serviceName = "default";
            std::string version;
            std::string description;
            std::string environment;
            int max_connections;


            /**
             * @brief 从配置管理器加载服务器配置
             * @return 服务器配置实例
             */
            static ServerConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief Kafka生产者配置结构体
         */
        struct kafkaProducerConfig {
            std::string brokers = "kafka:9092";        ///< Kafka代理服务器地址
            std::string client_id;                     ///< 客户端ID
            int acks = -1;                  ///< 确认模式：-1/0/1
            int retries = 3;                           ///< 重试次数
            int batch_size = 16384;                    ///< 批处理大小（字节）
            int linger_ms = 5;                         ///< 延迟发送时间（毫秒）
            int buffer_memory = 33554432;              ///< 缓冲区内存（32MB）
            std::string compression_type = "none";     ///< 压缩类型：none/gzip/snappy/lz4/zstd
            int max_request_size = 1048576;            ///< 最大请求大小（1MB）
            int request_timeout_ms = 30000;            ///< 请求超时时间（毫秒）
            int delivery_timeout_ms = 120000;          ///< 交付超时时间（毫秒）
            bool enable_idempotence = true;            ///< 是否启用幂等性
            int max_in_flight_requests = 5;            ///< 最大飞行中请求数

            // 安全配置
            std::string security_protocol = "PLAINTEXT"; ///< 安全协议：PLAINTEXT/SSL/SASL_PLAINTEXT/SASL_SSL
            std::string sasl_mechanism;                ///< SASL机制：PLAIN/SCRAM-SHA-256/SCRAM-SHA-512
            std::string sasl_username;                 ///< SASL用户名
            std::string sasl_password;                 ///< SASL密码
            std::string ssl_ca_location;               ///< SSL CA证书位置
            std::string ssl_certificate_location;      ///< SSL客户端证书位置
            std::string ssl_key_location;              ///< SSL私钥位置
            std::string ssl_key_password;              ///< SSL私钥密码

            // 业务配置
            bool enable_async_operations = true;

            /**
             * @brief 从配置管理器加载Kafka生产者配置
             * @return Kafka生产者配置实例
             */
            static kafkaProducerConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief Kafka消费者配置结构体
         */
        struct kafkaConsumerConfig {
            std::string brokers = "kafka:9092";        ///< Kafka代理服务器地址
            std::string group_id;                      ///< 消费者组ID
            std::string client_id;                     ///< 客户端ID
            std::string auto_offset_reset = "earliest"; ///< 偏移量重置策略：earliest/latest/none
            bool enable_auto_commit = false;           ///< 是否启用自动提交
            int auto_commit_interval_ms = 5000;        ///< 自动提交间隔（毫秒）
            int session_timeout_ms = 30000;            ///< 会话超时时间（毫秒）
            int heartbeat_interval_ms = 3000;          ///< 心跳间隔（毫秒）
            int max_poll_interval_ms = 300000;         ///< 最大轮询间隔（毫秒）
            int max_poll_records = 500;                ///< 单次轮询最大记录数
            int fetch_min_bytes = 1;                   ///< 最小获取字节数
            int fetch_max_wait_ms = 500;               ///< 最大等待时间（毫秒）
            int max_partition_fetch_bytes = 1048576;   ///< 单分区最大获取字节数（1MB）
            bool check_crcs = true;                    ///< 是否检查CRC
            std::string isolation_level = "read_uncommitted"; ///< 隔离级别：read_uncommitted/read_committed

            // 安全配置（与生产者相同）
            std::string security_protocol = "PLAINTEXT"; ///< 安全协议
            std::string sasl_mechanism;                ///< SASL机制
            std::string sasl_username;                 ///< SASL用户名
            std::string sasl_password;                 ///< SASL密码
            std::string ssl_ca_location;               ///< SSL CA证书位置
            std::string ssl_certificate_location;      ///< SSL客户端证书位置
            std::string ssl_key_location;              ///< SSL私钥位置
            std::string ssl_key_password;              ///< SSL私钥密码

            // 业务需求
            bool enable_async_operations = true;

            /**
             * @brief 从配置管理器加载Kafka消费者配置
             * @return Kafka消费者配置实例
             */
            static kafkaConsumerConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief Kafka通用配置结构体（兼容旧版本）
         */
        struct KafkaConfig {
            kafkaProducerConfig producer;              ///< 生产者配置
            kafkaConsumerConfig consumer;              ///< 消费者配置

            bool enable = true;
            std::string topic_prefix = "game_microservices";

            /**
             * @brief 从配置管理器加载Kafka配置
             * @return Kafka配置实例
             */
            static KafkaConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };



        /**
         * @brief 网络配置结构体
         */
        struct NetworkConfig {
            std::string bind_address = "0.0.0.0";      ///< 网络绑定地址
            int listen_port = 8081;                    ///< 监听端口
            int backlog = 128;                         ///< 监听队列长度
            int worker_threads = 4;                    ///< 工作线程数
            int io_threads = 2;                        ///< IO线程数
            bool tcp_nodelay = true;                   ///< TCP_NODELAY选项
            bool so_reuseaddr = true;                  ///< SO_REUSEADDR选项
            bool so_reuseport = true;
            bool so_keepalive = true;                  ///< SO_KEEPALIVE选项
            int send_buffer_size = 65536;              ///< 发送缓冲区大小
            int recv_buffer_size = 65536;              ///< 接收缓冲区大小
            int connection_timeout = 30;               ///< 连接超时时间（秒）
            int read_timeout = 60;                     ///< 读取超时时间（秒）
            int write_timeout = 60;                    ///< 写入超时时间（秒）
            int max_connections = 10000;               ///< 最大连接数
            bool enable_epoll = true;                  ///< 是否启用epoll（Linux）

            /**
             * @brief 从配置管理器加载网络配置
             * @return 网络配置实例
             */
            static NetworkConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief 任务调度器配置结构体
         */
        struct SchedulerConfig {
            int worker_threads = 4;                    ///< 工作线程数
            int max_pending_tasks = 10000;             ///< 最大待处理任务数
            int task_queue_size = 1000;                ///< 任务队列大小
            int tick_interval_ms = 100;                ///< 调度器时钟间隔（毫秒）
            bool enable_metrics = true;                ///< 是否启用性能指标
            bool enable_task_timeout = true;           ///< 是否启用任务超时
            int default_task_timeout_ms = 30000;       ///< 默认任务超时时间（毫秒）
            int max_retry_attempts = 3;                ///< 最大重试次数
            int retry_delay_ms = 1000;                 ///< 重试延迟时间（毫秒）
            bool enable_priority_queue = true;         ///< 是否启用优先级队列
            int high_priority_threshold = 100;         ///< 高优先级阈值

            /**
             * @brief 从配置管理器加载调度器配置
             * @return 调度器配置实例
             */
            static SchedulerConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

        /**
         * @brief 线程池配置结构体
         */
        struct ThreadPoolConfig {
            int core_pool_size = 16;                    ///< 核心线程池大小
            int maximum_pool_size = 24;                ///< 最大线程池大小
            int keep_alive_time_ms = 60000;            ///< 线程保活时间（毫秒）
            int queue_capacity = 1000;                 ///< 任务队列容量
            bool allow_core_thread_timeout = false;    ///< 是否允许核心线程超时
            std::string rejection_policy = "abort";    ///< 拒绝策略：abort/discard/caller_runs
            bool prestartAllCoreThreads = false;       ///< 是否预启动所有核心线程
            int thread_priority = 0;                   ///< 线程优先级
            std::string thread_name_prefix = "pool-";  ///< 线程名称前缀
            bool enable_monitoring = true;             ///< 是否启用监控
            int monitoring_interval_ms = 5000;         ///< 监控间隔（毫秒）
            bool enable_thread_dump = false;           ///< 线程转储


            /**
             * @brief 从配置管理器加载线程池配置
             * @return 线程池配置实例
             */
            static ThreadPoolConfig fromConfigManager();

            /**
             * @brief 验证配置的有效性
             * @throws std::invalid_argument 配置无效时抛出异常
             */
            void validate() const;
        };

    }
}


#endif //CONFIG_MANAGER_H
