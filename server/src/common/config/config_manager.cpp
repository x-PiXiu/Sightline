//
// Created by 29108 on 2025/7/6.
//

#include "common/config/config_manager.h"
#include "common/logger/logger.h"
#include <fstream>
#include <algorithm>
#include <cstdlib>
#include <set>
#include <cctype>
#include <unistd.h>
#include <linux/limits.h>

namespace common {
    namespace config {
        ConfigManager & ConfigManager::getInstance() {
            static ConfigManager instance;
            return instance;
        }

        std::string ConfigManager::findConfigFile(const std::string& filename,
                                                   const std::string& env_var) {
            // 策略1: 绝对路径直接检查
            if (!filename.empty()) {
                std::filesystem::path p(filename);
                if (p.is_absolute() && std::filesystem::exists(p)) {
                    return std::filesystem::canonical(p).string();
                }
            }

            // 策略2: 环境变量
            if (!env_var.empty()) {
                const char* env_path = std::getenv(env_var.c_str());
                if (env_path && env_path[0] != '\0') {
                    std::filesystem::path p(env_path);
                    if (std::filesystem::exists(p)) {
                        return std::filesystem::canonical(p).string();
                    }
                }
            }

            // 策略3: 从可执行文件目录向上遍历查找 config/<filename>
            {
                char exe_path[PATH_MAX] = {};
                ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
                if (len > 0) {
                    exe_path[len] = '\0';
                    std::filesystem::path exe_dir = std::filesystem::path(exe_path).parent_path();

                    // 向上最多遍历 10 层
                    for (int i = 0; i < 10; ++i) {
                        std::filesystem::path candidate = exe_dir / "config" / filename;
                        if (std::filesystem::exists(candidate)) {
                            return std::filesystem::canonical(candidate).string();
                        }

                        std::filesystem::path parent = exe_dir.parent_path();
                        if (parent == exe_dir) {
                            break;  // 已到达根目录
                        }
                        exe_dir = parent;
                    }
                }
            }

            // 策略4: 从当前工作目录尝试相对路径候选
            {
                std::vector<std::string> relative_candidates = {
                    std::string("./config/") + filename,
                    std::string("../config/") + filename,
                    std::string("../../config/") + filename,
                    std::string("../../../config/") + filename,
                    std::string("../../../../config/") + filename,
                };

                for (const auto& candidate : relative_candidates) {
                    if (std::filesystem::exists(candidate)) {
                        return std::filesystem::canonical(candidate).string();
                    }
                }
            }

            // 策略5: 标准部署路径
            {
                std::vector<std::string> deploy_paths = {
                    std::string("/etc/microservices/") + filename,
                    std::string("/opt/microservices/config/") + filename,
                    std::string("/app/config/") + filename,
                };

                for (const auto& candidate : deploy_paths) {
                    if (std::filesystem::exists(candidate)) {
                        return candidate;
                    }
                }
            }

            return "";
        }

        bool ConfigManager::loadFromFile(const std::string &file_path) {
            std::string extension = std::filesystem::path(file_path).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            if (extension == ".yaml" || extension == ".yml") {
                return loadFromYaml(file_path);
            } else if (extension == ".json") {
                return loadFromJson(file_path);
            } else {
                std::cout << __FILE__ << ":" << __LINE__ << " Unsupported config file format: "  << std::endl;
                return false;
            }
        }

        bool ConfigManager::loadFromYaml(const std::string &file_path) {
            try {
                if (!std::filesystem::exists(file_path)) {
                    std::cout<< __FILE__ << ":" << __LINE__ <<" Config file not found: " + file_path << std::endl;
                    return false;
                }

                YAML::Node config = YAML::LoadFile(file_path);

                std::unique_lock<std::shared_mutex> lock(config_mutex_);

                // 清除旧配置，避免混合不同配置文件的内容
                configs_.clear();

                loadYamlNode(config, "");

                return true;

            } catch (const YAML::Exception& e) {
                std::cerr << __FILE__ << ":" << __LINE__ << " Failed to load YAML config: " + std::string(e.what()) << std::endl;
                return false;
            } catch (const std::exception& e) {
                std::cerr << __FILE__ << ":" << __LINE__ << " Failed to load config file: " + std::string(e.what()) << std::endl;
                return false;
            }
        }

        bool ConfigManager::loadFromJson(const std::string &file_path) {
            try {
                if (!std::filesystem::exists(file_path)) {
                    std::cout<< __FILE__ << ":" << __LINE__ <<" Config file not found: " + file_path << std::endl;
                    return false;
                }

                std::ifstream file(file_path);
                if (!file.is_open()) {
                    std::cout<< __FILE__ << ":" << __LINE__ <<" Cannot open config file: " + file_path << std::endl;
                    return false;
                }

                nlohmann::json config;
                file >> config;

                std::unique_lock<std::shared_mutex> lock(config_mutex_);
                loadJsonNode(config, "");


                return true;

            } catch (const nlohmann::json::exception& e) {
                std::cerr << __FILE__ << ":" << __LINE__ << " Failed to load Json config file: " + std::string(e.what()) << std::endl;
                return false;
            } catch (const std::exception& e) {
                std::cerr << __FILE__ << ":" << __LINE__ << " Failed to load Json config file: " + std::string(e.what()) << std::endl;
                return false;
            }
        }

        bool ConfigManager::loadFromEnv() {
            try {
                std::unique_lock<std::shared_mutex> lock(config_mutex_);

                // 加载所有环境变量，转换为配置格式
                // 使用 getenv 和已知的环境变量列表，而不是直接访问 environ
                std::vector<std::string> env_keys = {
                    "PATH", "HOME", "USER", "SHELL", "LANG", "LC_ALL",
                    "SYSTEM_VERSION", "SYSTEM_ENVIRONMENT", "SYSTEM_DEBUG_MODE",
                    "MYSQL_HOST", "MYSQL_PORT", "MYSQL_USER", "MYSQL_PASSWORD", "MYSQL_DATABASE",
                    "REDIS_HOST", "REDIS_PORT", "REDIS_PASSWORD", "REDIS_DATABASE",
                    "KAFKA_BROKERS", "KAFKA_PRODUCER_BROKERS", "KAFKA_CONSUMER_BROKERS",
                    "KAFKA_GROUP_ID", "KAFKA_CLIENT_ID",
                    "LOG_LEVEL", "LOG_FILE_PATH", "LOG_CONSOLE_ENABLED", "LOG_FILE_ENABLED",
                    "SERVER_HOST", "SERVER_PORT", "THREAD_POOL_SIZE",
                    "NETWORK_BIND_ADDRESS", "NETWORK_LISTEN_PORT"
                };

                for (const auto& env_key : env_keys) {
                    const char* env_value = std::getenv(env_key.c_str());
                    if (env_value != nullptr) {
                        std::string value(env_value);

                        // 将环境变量名转换为配置键格式
                        // 例如: MYSQL_HOST -> mysql_host
                        std::string config_key = envKeyToConfigKey(env_key);
                        if (!config_key.empty()) {
                            configs_[config_key] = value;
                        }
                    }
                }


                return true;

            } catch (const std::exception& e) {
                std::cerr << __FILE__ << ":" << __LINE__ << "Failed to load config from environment " + std::string(e.what()) << std::endl;
                return false;
            }
        }

        bool ConfigManager::has(const std::string &key) const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);
            return configs_.find(key) != configs_.end();
        }

        void ConfigManager::remove(const std::string &key) {
            std::string old_value;

            {
                std::unique_lock<std::shared_mutex> lock(config_mutex_);
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    old_value = it->second;
                    configs_.erase(it);
                } else {
                    return; // 键不存在，无需通知
                }
            }

            notifyConfigChange(key, old_value, "");
        }

        void ConfigManager::clear() {
            std::map<std::string, std::string> old_configs;

            {
                std::unique_lock<std::shared_mutex> lock(config_mutex_);
                old_configs = configs_;
                configs_.clear();
            }

            // 通知所有配置被清除
            for (const auto& [key, old_value] : old_configs) {
                notifyConfigChange(key, old_value, "");
            }
        }

        std::map<std::string, std::string> ConfigManager::getAllConfigs() const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);
            return configs_;
        }

        std::map<std::string, std::string> ConfigManager::getConfigsByPrefix(const std::string &prefix) const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);
            std::map<std::string, std::string> result;

            for (const auto& [key, value] : configs_) {
                if (key.substr(0, prefix.length()) == prefix) {
                    result[key] = value;
                }
            }

            return result;
        }

        void ConfigManager::addChangeListener(const std::string& key, ChangeListener listener) {
            // 参数验证
            if (!listener) {
                throw std::invalid_argument("ChangeListener cannot be null");
            }

            if (key.empty()) {
                throw std::invalid_argument("Configuration key cannot be empty");
            }

            // 优化：先尝试共享锁查找，减少独占锁的使用时间
            {
                std::shared_lock<std::shared_mutex> shared_lock(listeners_mutex_);
                auto it = listeners_.find(key);
                if (it != listeners_.end()) {
                    // key已存在，升级为独占锁进行修改
                    shared_lock.unlock();
                    std::unique_lock<std::shared_mutex> unique_lock(listeners_mutex_);

                    // 重新查找，因为在锁切换期间可能有变化
                    auto it_unique = listeners_.find(key);
                    if (it_unique != listeners_.end()) {
                        try {
                            it_unique->second.push_back(std::move(listener));
                            LOG_DEBUG("Added change listener for key: " + key);
                            return;
                        } catch (const std::exception& e) {
                            LOG_ERROR("Failed to add change listener for key '" + key + "': " + e.what());
                            throw;
                        }
                    }
                    // 如果key在锁切换期间被删除，继续执行下面的插入逻辑
                }
            }

            // key不存在，需要创建新的vector
            std::unique_lock<std::shared_mutex> unique_lock(listeners_mutex_);
            try {
                // 使用emplace避免不必要的拷贝，并确保异常安全
                auto& listener_vector = listeners_[key];
                listener_vector.push_back(std::move(listener));

                LOG_DEBUG("Added change listener for new key: " + key);
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to add change listener for key '" + key + "': " + e.what());
                throw;
            }
        }

        void ConfigManager::removeChangeListeners(const std::string &key) {
            if (key.empty()) {
                LOG_WARNING("Attempted to remove listeners for empty key");
                return;
            }

            std::unique_lock<std::shared_mutex> lock(listeners_mutex_);

            auto it = listeners_.find(key);
            if (it != listeners_.end()) {
                size_t listener_count = it->second.size();
                listeners_.erase(it);

                LOG_DEBUG("Removed " + std::to_string(listener_count) +
                         " change listeners for key: " + key);
            } else {
                LOG_DEBUG("No listeners found for key: " + key);
            }
        }

        bool ConfigManager::validate() const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);

            try {
                // 验证所有配置键的格式
                for (const auto& [key, value] : configs_) {
                    if (!isValidConfigKey(key)) {
                        LOG_ERROR("Invalid configuration key format: '" + key + "'");
                        return false;
                    }

                    // 验证配置值不为空（除非明确允许）
                    if (value.empty() && !isEmptyValueAllowed(key)) {
                        LOG_ERROR("Configuration value cannot be empty for key: '" + key + "'");
                        return false;
                    }
                }

                // 验证必需的配置项是否存在
                std::vector<std::string> required_keys = getRequiredConfigKeys();
                for (const auto& required_key : required_keys) {
                    if (configs_.find(required_key) == configs_.end()) {
                        LOG_ERROR("Required configuration key missing: '" + required_key + "'");
                        return false;
                    }
                }

                // 验证配置值的类型和范围
                if (!validateConfigValues()) {
                    return false;
                }

                LOG_DEBUG("Configuration validation passed");
                return true;

            } catch (const std::exception& e) {
                LOG_ERROR("Configuration validation failed with exception: " + std::string(e.what()));
                return false;
            }
        }

        std::vector<std::string> ConfigManager::getValidationErrors() const {
            std::vector<std::string> errors;
            std::shared_lock<std::shared_mutex> lock(config_mutex_);

            try {
                // 检查配置键格式
                for (const auto& [key, value] : configs_) {
                    if (!isValidConfigKey(key)) {
                        errors.push_back("Invalid configuration key format: '" + key + "'");
                    }

                    // 检查空值
                    if (value.empty() && !isEmptyValueAllowed(key)) {
                        errors.push_back("Configuration value cannot be empty for key: '" + key + "'");
                    }
                }

                // 检查必需配置项
                std::vector<std::string> required_keys = getRequiredConfigKeys();
                for (const auto& required_key : required_keys) {
                    if (configs_.find(required_key) == configs_.end()) {
                        errors.push_back("Required configuration key missing: '" + required_key + "'");
                    }
                }

                // 检查配置值的类型和范围
                auto value_errors = getConfigValueValidationErrors();
                errors.insert(errors.end(), value_errors.begin(), value_errors.end());

            } catch (const std::exception& e) {
                errors.push_back("Validation error: " + std::string(e.what()));
            }

            return errors;
        }

        bool ConfigManager::saveToFile(const std::string &file_path) const {
            if (file_path.empty()) {
                LOG_ERROR("File path cannot be empty");
                return false;
            }

            // 根据文件扩展名选择保存格式
            std::string extension = std::filesystem::path(file_path).extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

            if (extension == ".yaml" || extension == ".yml") {
                return saveToYaml(file_path);
            } else if (extension == ".json") {
                return saveToJson(file_path);
            } else {
                LOG_ERROR("Unsupported file format: " + extension + ". Supported formats: .yaml, .yml, .json");
                return false;
            }
        }

        bool ConfigManager::saveToYaml(const std::string &file_path) const {
            try {
                // 创建目录（如果不存在）
                std::filesystem::path path(file_path);
                if (path.has_parent_path()) {
                    std::filesystem::create_directories(path.parent_path());
                }

                // 构建YAML结构
                YAML::Node root;

                {
                    std::shared_lock<std::shared_mutex> lock(config_mutex_);

                    for (const auto& [key, value] : configs_) {
                        setYamlValue(root, key, value);
                    }
                }

                // 写入文件
                std::ofstream file(file_path);
                if (!file.is_open()) {
                    LOG_ERROR("Cannot open file for writing: " + file_path);
                    return false;
                }

                file << root;
                file.close();


                return true;

            } catch (const YAML::Exception& e) {
                LOG_ERROR("Failed to save YAML config: " + std::string(e.what()));
                return false;
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to save config file: " + std::string(e.what()));
                return false;
            }
        }

        bool ConfigManager::saveToJson(const std::string &file_path) const {
            try {
                // 创建目录（如果不存在）
                std::filesystem::path path(file_path);
                if (path.has_parent_path()) {
                    std::filesystem::create_directories(path.parent_path());
                }

                // 构建JSON结构
                nlohmann::json root;

                {
                    std::shared_lock<std::shared_mutex> lock(config_mutex_);

                    for (const auto& [key, value] : configs_) {
                        setJsonValue(root, key, value);
                    }
                }

                // 写入文件
                std::ofstream file(file_path);
                if (!file.is_open()) {
                    LOG_ERROR("Cannot open file for writing: " + file_path);
                    return false;
                }

                file << root.dump(4);  // 4空格缩进，美化输出
                file.close();


                return true;

            } catch (const nlohmann::json::exception& e) {
                LOG_ERROR("Failed to save JSON config: " + std::string(e.what()));
                return false;
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to save config file: " + std::string(e.what()));
                return false;
            }
        }

        void ConfigManager::enableHotReload(const std::string &file_path, std::chrono::seconds check_interval) {
            disableHotReload(); // 先停止之前的热重载

            hot_reload_enabled_ = true;
            hot_reload_file_path_ = file_path;
            hot_reload_check_interval_ = check_interval;

            // 记录文件的最后修改时间
            if (std::filesystem::exists(file_path)) {
                last_file_write_time_ = std::filesystem::last_write_time(file_path);
            }

            // 启动热重载线程
            hot_reload_thread_ = std::thread(&ConfigManager::hotReloadWorker, this);


        }

        void ConfigManager::disableHotReload() {
            hot_reload_enabled_ = false;

            if (hot_reload_thread_.joinable()) {
                hot_reload_thread_.join();
            }


        }

        ConfigManager::ConfigManager()
            : hot_reload_enabled_(false),
              last_file_write_time_(std::filesystem::file_time_type{}),
              hot_reload_check_interval_(std::chrono::seconds(5)) {

            // 避免在构造函数中使用LOG宏，防止循环依赖
            // LOG_INFO("ConfigManager initialized" );

            // 初始化默认配置值
            try {
                // 可以在这里设置一些默认的系统配置
                configs_["system.version"] = "1.0.0";
                configs_["system.startup_time"] = std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );

                // 使用标准输出代替LOG宏，避免循环依赖
                // LOG_DEBUG("Default system configurations initialized" );
            } catch (const std::exception& e) {
                // 使用标准错误输出代替LOG宏，避免循环依赖
                std::cerr << __FILE__ << ":" << __LINE__ << " ConfigManager: Failed to initialize default configurations: " << e.what() << std::endl;
            }
        }

        ConfigManager::~ConfigManager() {
            // 避免在析构函数中使用LOG宏，防止循环依赖
            // LOG_INFO("ConfigManager shutting down..." );

            try {
                // 1. 首先禁用热重载，停止监控线程
                if (hot_reload_enabled_.load()) {
                    // LOG_DEBUG("Disabling hot reload during shutdown" );
                    disableHotReload();
                }

                // 2. 清除所有配置变更监听器
                {
                    std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
                    if (!listeners_.empty()) {
                        size_t total_listeners = 0;
                        for (const auto& pair : listeners_) {
                            total_listeners += pair.second.size();
                        }
                        // LOG_DEBUG("Clearing " + std::to_string(total_listeners) +
                        //          " change listeners for " + std::to_string(listeners_.size()) + " keys",
                        //          __FILE__, __LINE__);
                        listeners_.clear();
                    }
                }

                // 3. 清空所有配置数据
                {
                    std::unique_lock<std::shared_mutex> lock(config_mutex_);
                    if (!configs_.empty()) {
                        // LOG_DEBUG("Clearing " + std::to_string(configs_.size()) + " configuration entries" );
                        configs_.clear();
                    }
                }

                // 4. 重置热重载相关状态
                hot_reload_file_path_.clear();
                last_file_write_time_ = std::filesystem::file_time_type{};
                hot_reload_check_interval_ = std::chrono::seconds(5);

                // LOG_INFO("ConfigManager shutdown completed successfully" );

            } catch (const std::exception& e) {
                // 析构函数中不应该抛出异常，只记录错误到标准错误输出
                std::cerr << "ConfigManager: Error during shutdown: " << e.what() << std::endl;
            } catch (...) {
                // 捕获所有其他异常
                std::cerr << "ConfigManager: Unknown error during shutdown" << std::endl;
            }
        }

        void ConfigManager::notifyConfigChange(const std::string &key, const std::string &old_value,
            const std::string &new_value) {
            // 复制监听器列表，避免在回调执行期间持有锁
            std::vector<ChangeListener> listeners_to_notify;

            {
                std::shared_lock<std::shared_mutex> lock(listeners_mutex_);
                auto it = listeners_.find(key);
                if (it != listeners_.end() && !it->second.empty()) {
                    // 复制监听器列表，避免在回调期间持有锁
                    listeners_to_notify = it->second;
                }
            }

            // 在锁外执行回调，避免死锁和提高并发性能
            if (!listeners_to_notify.empty()) {
                LOG_INFO("Notifying " + std::to_string(listeners_to_notify.size()) +
                         " listeners for config change: " + key);

                for (const auto& listener : listeners_to_notify) {
                    try {
                        // 检查listener是否仍然有效
                        if (listener) {
                            listener(key, old_value, new_value);
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Config change listener failed for key '" + key + "': " +
                                 e.what());
                        // 继续执行其他监听器，不因一个失败而中断
                    } catch (...) {
                        LOG_ERROR("Config change listener failed for key '" + key +
                                 "' with unknown exception");
                    }
                }
            }
        }

        void ConfigManager::hotReloadWorker() {


            while (hot_reload_enabled_.load()) {
                try {
                    // 检查文件是否存在，使用error_code避免异常
                    std::error_code ec;
                    bool file_exists = std::filesystem::exists(hot_reload_file_path_, ec);

                    if (ec) {
                        LOG_WARNING("Error checking file existence: " + ec.message());
                        std::this_thread::sleep_for(hot_reload_check_interval_);
                        continue;
                    }

                    if (!file_exists) {
                        LOG_WARNING("Config file no longer exists: " + hot_reload_file_path_);
                        std::this_thread::sleep_for(hot_reload_check_interval_);
                        continue;
                    }

                    // 获取文件修改时间
                    auto current_write_time = std::filesystem::last_write_time(hot_reload_file_path_, ec);
                    if (ec) {
                        LOG_WARNING("Error getting file modification time: " + ec.message());
                        std::this_thread::sleep_for(hot_reload_check_interval_);
                        continue;
                    }

                    // 检查文件是否有变化
                    if (current_write_time != last_file_write_time_) {


                        // 保存旧配置用于比较
                        std::map<std::string, std::string> old_configs;
                        {
                            std::shared_lock<std::shared_mutex> lock(config_mutex_);
                            old_configs = configs_;
                        }

                        // 重新加载配置
                        bool reload_success = false;
                        try {
                            reload_success = loadFromFile(hot_reload_file_path_);
                        } catch (const std::exception& e) {
                            LOG_ERROR("Failed to reload config file: " + std::string(e.what()));
                            // 继续使用旧配置，不更新时间戳
                            std::this_thread::sleep_for(hot_reload_check_interval_);
                            continue;
                        }

                        if (reload_success) {
                            last_file_write_time_ = current_write_time;


                            // 比较配置变化并通知监听器（在锁外执行通知）
                            std::map<std::string, std::string> current_configs;
                            {
                                std::shared_lock<std::shared_mutex> lock(config_mutex_);
                                current_configs = configs_;
                            }

                            // 检查新增和修改的配置
                            for (const auto& [key, new_value] : current_configs) {
                                auto old_it = old_configs.find(key);
                                if (old_it == old_configs.end()) {
                                    // 新增的配置
                                    notifyConfigChange(key, "", new_value);
                                } else if (old_it->second != new_value) {
                                    // 修改的配置
                                    notifyConfigChange(key, old_it->second, new_value);
                                }
                            }

                            // 检查删除的配置
                            for (const auto& [key, old_value] : old_configs) {
                                if (current_configs.find(key) == current_configs.end()) {
                                    notifyConfigChange(key, old_value, "");
                                }
                            }
                        } else {
                            LOG_ERROR("Failed to reload config file, keeping old configuration");
                        }
                    }

                } catch (const std::filesystem::filesystem_error& e) {
                    LOG_ERROR("Filesystem error in hot reload worker: " + std::string(e.what()));
                } catch (const std::exception& e) {
                    LOG_ERROR("Hot reload worker error: " + std::string(e.what()));
                } catch (...) {
                    LOG_ERROR("Unknown error in hot reload worker");
                }

                // 等待下次检查
                std::this_thread::sleep_for(hot_reload_check_interval_);
            }


        }

        std::filesystem::file_time_type ConfigManager::getFileModificationTime(const std::string &file_path) {
            std::error_code ec;
            auto ftime = std::filesystem::last_write_time(file_path, ec);
            if (ec) {
                LOG_WARNING("Error getting file modification time for " + file_path + ": " + ec.message());
                return std::filesystem::file_time_type{};
            }
            return ftime;
        }

        void ConfigManager::loadYamlNode(const YAML::Node &node, const std::string &prefix) {
            if (node.IsScalar()) {
                configs_[prefix] = node.as<std::string>();
            } else if (node.IsMap()) {
                for (const auto& item : node) {
                    std::string key = item.first.as<std::string>();
                    std::string full_key = prefix.empty() ? key : prefix + "." + key;
                    loadYamlNode(item.second, full_key);
                }
            } else if (node.IsSequence()) {
                // 处理数组类型
                std::ostringstream oss;
                for (size_t i = 0; i < node.size(); ++i) {
                    if (i > 0) oss << ",";
                    oss << node[i].as<std::string>();
                }
                configs_[prefix] = oss.str();
            }
        }

        void ConfigManager::loadJsonNode(const nlohmann::json &node, const std::string &prefix) {
            if (node.is_string()) {
                configs_[prefix] = node.get<std::string>();
            } else if (node.is_number()) {
                configs_[prefix] = std::to_string(node.get<double>());
            } else if (node.is_boolean()) {
                configs_[prefix] = node.get<bool>() ? "true" : "false";
            } else if (node.is_object()) {
                for (const auto& [key, value] : node.items()) {
                    std::string full_key = prefix.empty() ? key : prefix + "." + key;
                    loadJsonNode(value, full_key);
                }
            } else if (node.is_array()) {
                std::ostringstream oss;
                for (size_t i = 0; i < node.size(); ++i) {
                    if (i > 0) oss << ",";
                    if (node[i].is_string()) {
                        oss << node[i].get<std::string>();
                    } else {
                        oss << node[i].dump();
                    }
                }
                configs_[prefix] = oss.str();
            }
        }

        std::string ConfigManager::envKeyToConfigKey(const std::string &env_key) const {
            // 将环境变量名转换为配置键
            // 例如: MYSQL_HOST -> database.mysql.host
            //      REDIS_PORT -> database.redis.port
            //      HTTP_SERVER_PORT -> server.http.port

            std::string key = env_key;
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);

            // 定义环境变量到配置键的映射规则
            static const std::map<std::string, std::string> env_mappings = {
                // 系统相关
                {"system_version", "system_version"},
                {"system_environment", "system_environment"},
                {"system_debug_mode", "system_debug_mode"},

                // 数据库相关
                {"mysql_host", "mysql_host"},
                {"mysql_port", "mysql_port"},
                {"mysql_user", "mysql_user"},
                {"mysql_password", "mysql_password"},
                {"mysql_database", "mysql_database"},
                {"redis_host", "redis_host"},
                {"redis_port", "redis_port"},
                {"redis_password", "redis_password"},
                {"redis_database", "redis_database"},

                // 服务器相关
                {"server_host", "server_host"},
                {"server_port", "server_port"},
                {"http_server_port", "server.http.port"},
                {"http_server_host", "server.http.bind_address"},
                {"tcp_server_port", "server.tcp.port"},
                {"websocket_port", "server.websocket.port"},

                // 认证相关
                {"jwt_secret_key", "multi_tenant.jwt.secret_key"},
                {"jwt_expire_hours", "multi_tenant.jwt.expire_hours"},
                {"jwt_issuer", "multi_tenant.jwt.issuer"},
                {"jwt_algorithm", "multi_tenant.jwt.algorithm"},
                
                // 多商户配置
                {"default_commission_rate", "multi_tenant.merchant.default_commission_rate"},
                {"min_commission_rate", "multi_tenant.merchant.min_commission_rate"},
                {"max_commission_rate", "multi_tenant.merchant.max_commission_rate"},
                {"default_settlement_cycle", "multi_tenant.merchant.default_settlement_cycle"},
                {"min_settlement_cycle", "multi_tenant.merchant.min_settlement_cycle"},
                {"max_settlement_cycle", "multi_tenant.merchant.max_settlement_cycle"},
                {"enable_whitelist_check", "multi_tenant.merchant.enable_whitelist_check"},
                
                // 密码安全
                {"password_min_length", "multi_tenant.password.min_length"},
                {"password_max_length", "multi_tenant.password.max_length"},
                {"password_salt_length", "multi_tenant.password.salt_length"},
                
                // 游戏分区配置
                {"default_api_timeout_ms", "multi_tenant.game_server.default_api_timeout_ms"},
                {"default_api_retry_times", "multi_tenant.game_server.default_api_retry_times"},
                {"default_db_port", "multi_tenant.game_server.default_db_port"},
                {"default_db_charset", "multi_tenant.game_server.default_db_charset"},
                
                // 发货配置
                {"delivery_max_retry_times", "multi_tenant.delivery.max_retry_times"},
                {"delivery_retry_interval_ms", "multi_tenant.delivery.retry_interval_ms"},
                {"delivery_timeout_ms", "multi_tenant.delivery.timeout_ms"},

                // 日志相关
                {"log_level", "log_level"},
                {"log_file_path", "log_file_path"},
                {"log_console_enabled", "log_console_enabled"},
                {"log_file_enabled", "log_file_enabled"},

                // Kafka相关
                {"kafka_brokers", "kafka_producer_brokers"},
                {"kafka_producer_brokers", "kafka_producer_brokers"},
                {"kafka_consumer_brokers", "kafka_consumer_brokers"},
                {"kafka_group_id", "kafka_consumer_group_id"},
                {"kafka_client_id", "kafka_producer_client_id"},

                // 网络相关
                {"network_bind_address", "network_bind_address"},
                {"network_listen_port", "network_listen_port"},

                // 线程池相关
                {"thread_pool_size", "thread_pool_size"},
                {"thread_pool_core_size", "thread_pool_core_size"},
                {"thread_pool_max_size", "thread_pool_max_size"}
            };

            auto it = env_mappings.find(key);
            return it != env_mappings.end() ? it->second : "";
        }

        // 配置重新加载实现
        bool ConfigManager::reload() {
            if (hot_reload_file_path_.empty()) {
                LOG_WARNING("No file path set for reload operation");
                return false;
            }



            // 保存旧配置用于比较
            std::map<std::string, std::string> old_configs;
            {
                std::shared_lock<std::shared_mutex> lock(config_mutex_);
                old_configs = configs_;
            }

            // 重新加载配置
            bool reload_success = false;
            try {
                reload_success = loadFromFile(hot_reload_file_path_);
            } catch (const std::exception& e) {
                LOG_ERROR("Failed to reload config file: " + std::string(e.what()));
                return false;
            }

            if (!reload_success) {
                LOG_ERROR("Failed to reload configuration file");
                return false;
            }

            // 更新文件修改时间
            try {
                std::error_code ec;
                last_file_write_time_ = std::filesystem::last_write_time(hot_reload_file_path_, ec);
                if (ec) {
                    LOG_WARNING("Could not update file modification time: " + ec.message());
                }
            } catch (const std::exception& e) {
                LOG_WARNING("Exception updating file modification time: " + std::string(e.what()));
            }

            // 比较配置变化并通知监听器
            std::map<std::string, std::string> current_configs;
            {
                std::shared_lock<std::shared_mutex> lock(config_mutex_);
                current_configs = configs_;
            }

            // 检查新增和修改的配置
            for (const auto& [key, new_value] : current_configs) {
                auto old_it = old_configs.find(key);
                if (old_it == old_configs.end()) {
                    // 新增的配置
                    notifyConfigChange(key, "", new_value);
                } else if (old_it->second != new_value) {
                    // 修改的配置
                    notifyConfigChange(key, old_it->second, new_value);
                }
            }

            // 检查删除的配置
            for (const auto& [key, old_value] : old_configs) {
                if (current_configs.find(key) == current_configs.end()) {
                    notifyConfigChange(key, old_value, "");
                }
            }


            return true;
        }

        // 配置重置实现
        void ConfigManager::reset() {


            // 保存旧配置用于通知
            std::map<std::string, std::string> old_configs;
            {
                std::unique_lock<std::shared_mutex> lock(config_mutex_);
                old_configs = configs_;
                configs_.clear();

                // 恢复默认配置
                configs_["system.version"] = "1.0.0";
                configs_["system.startup_time"] = std::to_string(
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
            }

            // 停止热重载
            if (hot_reload_enabled_.load()) {
                disableHotReload();
            }

            // 清除所有监听器
            {
                std::unique_lock<std::shared_mutex> lock(listeners_mutex_);
                listeners_.clear();
            }

            // 重置热重载相关状态
            hot_reload_file_path_.clear();
            last_file_write_time_ = std::filesystem::file_time_type{};
            hot_reload_check_interval_ = std::chrono::seconds{5};

            // 通知所有配置被清除
            for (const auto& [key, old_value] : old_configs) {
                // 注意：这里不能调用notifyConfigChange，因为监听器已被清除
                LOG_DEBUG("Configuration key removed during reset: " + key);
            }


        }

        bool ConfigManager::isValidConfigKey(const std::string &key) const {
            if (key.empty()) {
                return false;
            }

            // 检查是否包含非法字符
            for (char c : key) {
                if (!std::isalnum(c) && c != '.' && c != '_' && c != '-') {
                    return false;
                }
            }

            // 检查是否以点开头或结尾
            if (key.front() == '.' || key.back() == '.') {
                return false;
            }

            // 检查是否有连续的点
            if (key.find("..") != std::string::npos) {
                return false;
            }

            return true;
        }

        std::string ConfigManager::sanitizeConfigValue(const std::string &value) const {
            std::string result = value;

            // 去除前后空白字符
            result.erase(0, result.find_first_not_of(" \t\r\n"));
            result.erase(result.find_last_not_of(" \t\r\n") + 1);

            // 处理引号包围的值
            if (result.length() >= 2) {
                if ((result.front() == '"' && result.back() == '"') ||
                    (result.front() == '\'' && result.back() == '\'')) {
                    result = result.substr(1, result.length() - 2);
                    }
            }

            return result;
        }

        bool ConfigManager::isEmptyValueAllowed(const std::string &key) const {
            // 定义允许为空值的配置键列表
            static const std::set<std::string> empty_allowed_keys = {
                // 数据库密码（可选）
                "database.mysql.password",
                "database.redis.password",

                // SSL配置（可选）
                "service.ssl_cert_file",
                "service.ssl_key_file",

                // 邮件配置（可选功能）
                "email.smtp_user",
                "email.smtp_password",

                // OAuth配置（可选功能）
                "oauth.google_client_id",
                "oauth.google_client_secret",
                "oauth.github_client_id",
                "oauth.github_client_secret",
                "oauth.wechat_app_id",
                "oauth.wechat_app_secret",
                "oauth.steam_app_id",
                "oauth.steam_api_key",
                "oauth.qq_app_id",
                "oauth.qq_app_key",

                // 系统描述（可选）
                "system.description",
                "system.environment",

                // 自定义格式（可选）
                "logging.custom_format"
            };

            // 检查键是否在允许为空的列表中
            bool allowed = empty_allowed_keys.find(key) != empty_allowed_keys.end();

            // 检查是否是可选配置的模式匹配
            if (!allowed) {
                // 以 "optional." 开头的配置都允许为空
                if (key.find("optional.") == 0) {
                    allowed = true;
                }
                // 以 ".description" 结尾的配置允许为空
                else if (key.length() > 12 && key.substr(key.length() - 12) == ".description") {
                    allowed = true;
                }
                // 以 ".comment" 结尾的配置允许为空
                else if (key.length() > 8 && key.substr(key.length() - 8) == ".comment") {
                    allowed = true;
                }
                // OAuth相关配置都允许为空（可选功能）
                else if (key.find("oauth.") == 0) {
                    allowed = true;
                }
                // 邮件相关配置都允许为空（可选功能）
                else if (key.find("email.") == 0) {
                    allowed = true;
                }
                // SSL相关配置都允许为空（可选功能）
                else if (key.find("ssl_") != std::string::npos || key.find(".ssl_") != std::string::npos) {
                    allowed = true;
                }
            }

            LOG_DEBUG("Empty value check for key '" + key + "': " + (allowed ? "allowed" : "not allowed"));
            return allowed;
        }

        std::vector<std::string> ConfigManager::getRequiredConfigKeys() const {
            // 定义必需的配置键列表（只包含核心必需配置）
            static const std::vector<std::string> required_keys = {
                // 服务器相关必需配置
                "service.host",
                "service.port",

                // 数据库相关必需配置
                "database.mysql.host",
                "database.mysql.port",
                "database.mysql.username",
                "database.mysql.database",

                // Redis相关必需配置
                "database.redis.host",
                "database.redis.port",

                // 日志相关必需配置
                "logging.level"
            };

            LOG_DEBUG("Retrieved " + std::to_string(required_keys.size()) + " required configuration keys");
            return required_keys;
        }

        bool ConfigManager::validateConfigValues() const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);

            try {
                auto required_keys = getRequiredConfigKeys();
                bool all_valid = true;

                // 1. 检查所有必需的配置键是否存在
                for (const auto& key : required_keys) {
                    auto it = configs_.find(key);
                    if (it == configs_.end()) {
                        LOG_ERROR("Required configuration key missing: " + key);
                        all_valid = false;
                        continue;
                    }

                    // 2. 检查值是否为空（对于不允许为空的键）
                    if (it->second.empty() && !isEmptyValueAllowed(key)) {
                        LOG_ERROR("Required configuration key has empty value: " + key);
                        all_valid = false;
                        continue;
                    }
                }

                // 3. 验证特定配置的值格式
                all_valid &= validateSpecificConfigFormats();

                if (all_valid) {
                    LOG_DEBUG("All configuration values validation passed");
                } else {
                    LOG_WARNING("Configuration validation failed - see error logs for details");
                }

                return all_valid;

            } catch (const std::exception& e) {
                LOG_ERROR("Exception during configuration validation: " + std::string(e.what()));
                return false;
            }
        }

        std::vector<std::string> ConfigManager::getConfigValueValidationErrors() const {
            std::shared_lock<std::shared_mutex> lock(config_mutex_);
            std::vector<std::string> errors;

            try {
                auto required_keys = getRequiredConfigKeys();

                // 1. 检查缺失的必需配置
                for (const auto& key : required_keys) {
                    auto it = configs_.find(key);
                    if (it == configs_.end()) {
                        errors.push_back("Missing required configuration: " + key);
                        continue;
                    }

                    // 2. 检查空值
                    if (it->second.empty() && !isEmptyValueAllowed(key)) {
                        errors.push_back("Empty value not allowed for: " + key);
                        continue;
                    }
                }

                // 3. 验证数值类型配置
                validateNumericConfigs(errors);

                // 4. 验证布尔类型配置
                validateBooleanConfigs(errors);

                // 5. 验证文件路径配置
                validateFilePathConfigs(errors);

                // 6. 验证网络配置
                validateNetworkConfigs(errors);

                LOG_DEBUG("Configuration validation found " + std::to_string(errors.size()) + " errors");

            } catch (const std::exception& e) {
                errors.push_back("Exception during validation: " + std::string(e.what()));
                LOG_ERROR("Exception in getConfigValueValidationErrors: " + std::string(e.what()));
            }

            return errors;
        }

        void ConfigManager::setYamlValue(YAML::Node &node, const std::string &key, const std::string &value) const {
            try {
                // 为了保持与loadYamlNode的一致性，我们使用扁平结构
                // 直接使用完整的键名，不创建嵌套结构

                // 尝试智能类型转换
                const std::string& final_key = key;

                // 尝试转换为适当的类型
                if (value == "true" || value == "false") {
                    // 布尔值
                    node[final_key] = (value == "true");
                } else if (value.find_first_not_of("0123456789") == std::string::npos && !value.empty()) {
                    // 整数
                    try {
                        long long int_val = std::stoll(value);
                        node[final_key] = int_val;
                    } catch (...) {
                        node[final_key] = value; // 回退到字符串
                    }
                } else if (value.find_first_not_of("0123456789.") == std::string::npos &&
                          std::count(value.begin(), value.end(), '.') == 1 && !value.empty()) {
                    // 浮点数
                    try {
                        double double_val = std::stod(value);
                        node[final_key] = double_val;
                    } catch (...) {
                        node[final_key] = value; // 回退到字符串
                    }
                } else {
                    // 字符串
                    node[final_key] = value;
                }

                LOG_DEBUG("Set YAML value: " + key + " = " + value);

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to set YAML value for key '" + key + "': " + std::string(e.what()));
                throw;
            }
        }

        void ConfigManager::setJsonValue(nlohmann::json &node, const std::string &key, const std::string &value) const {
            try {
                // 解析键路径（支持嵌套键，如 "database.mysql.host"）
                std::vector<std::string> key_parts;
                std::stringstream ss(key);
                std::string part;

                while (std::getline(ss, part, '.')) {
                    if (!part.empty()) {
                        key_parts.push_back(part);
                    }
                }

                if (key_parts.empty()) {
                    LOG_WARNING("Empty key provided for JSON value setting");
                    return;
                }

                // 导航到正确的节点位置
                nlohmann::json* current_node = &node;
                for (size_t i = 0; i < key_parts.size() - 1; ++i) {
                    if (!current_node->contains(key_parts[i]) || !(*current_node)[key_parts[i]].is_object()) {
                        (*current_node)[key_parts[i]] = nlohmann::json::object();
                    }
                    current_node = &((*current_node)[key_parts[i]]);
                }

                // 设置最终值，尝试智能类型转换
                const std::string& final_key = key_parts.back();

                // 尝试转换为适当的类型
                if (value == "true") {
                    (*current_node)[final_key] = true;
                } else if (value == "false") {
                    (*current_node)[final_key] = false;
                } else if (value == "null") {
                    (*current_node)[final_key] = nullptr;
                } else if (value.find_first_not_of("0123456789") == std::string::npos && !value.empty()) {
                    // 整数
                    try {
                        long long int_val = std::stoll(value);
                        (*current_node)[final_key] = int_val;
                    } catch (...) {
                        (*current_node)[final_key] = value; // 回退到字符串
                    }
                } else if (value.find_first_not_of("0123456789.") == std::string::npos &&
                          std::count(value.begin(), value.end(), '.') == 1 && !value.empty()) {
                    // 浮点数
                    try {
                        double double_val = std::stod(value);
                        (*current_node)[final_key] = double_val;
                    } catch (...) {
                        (*current_node)[final_key] = value; // 回退到字符串
                    }
                } else {
                    // 字符串
                    (*current_node)[final_key] = value;
                }

                LOG_DEBUG("Set JSON value: " + key + " = " + value);

            } catch (const std::exception& e) {
                LOG_ERROR("Failed to set JSON value for key '" + key + "': " + std::string(e.what()));
                throw;
            }
        }

        // ==================== 私有辅助方法实现 ====================

        bool ConfigManager::validateSpecificConfigFormats() const {
            bool all_valid = true;

            // 验证端口号范围
            auto validate_port = [&](const std::string& key) {
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    try {
                        int port = std::stoi(it->second);
                        if (port < 1 || port > 65535) {
                            LOG_ERROR("Invalid port number for " + key + ": " + it->second + " (must be 1-65535)");
                            return false;
                        }
                    } catch (...) {
                        LOG_ERROR("Invalid port format for " + key + ": " + it->second);
                        return false;
                    }
                }
                return true;
            };

            all_valid &= validate_port("database.mysql.port");
            all_valid &= validate_port("database.redis.port");
            all_valid &= validate_port("service.port");

            // 验证日志级别
            auto it = configs_.find("logging.level");
            if (it != configs_.end()) {
                std::set<std::string> valid_levels = {"TRACE", "DEBUG", "INFO", "WARN", "WARNING", "ERROR", "FATAL"};
                if (valid_levels.find(it->second) == valid_levels.end()) {
                    LOG_ERROR("Invalid log level: " + it->second);
                    all_valid = false;
                }
            }

            return all_valid;
        }

        /**
         * @brief 解析带单位的大小值（如 "100MB", "1GB"）
         * @param value 带单位的值字符串
         * @return 解析后的字节数，解析失败返回-1
         */
        long long ConfigManager::parseSizeValue(const std::string& value) const {
            if (value.empty()) {
                return -1;
            }

            std::string trimmed = value;
            // 去除前后空白
            trimmed.erase(0, trimmed.find_first_not_of(" \t"));
            trimmed.erase(trimmed.find_last_not_of(" \t") + 1);

            if (trimmed.empty()) {
                return -1;
            }

            // 检查是否是纯数字
            bool is_pure_number = true;
            for (char c : trimmed) {
                if (!std::isdigit(c)) {
                    is_pure_number = false;
                    break;
                }
            }

            if (is_pure_number) {
                try {
                    return std::stoll(trimmed);
                } catch (...) {
                    return -1;
                }
            }

            // 解析带单位的值
            std::string number_part;
            std::string unit_part;

            size_t i = 0;
            // 提取数字部分
            while (i < trimmed.length() && (std::isdigit(trimmed[i]) || trimmed[i] == '.')) {
                number_part += trimmed[i];
                i++;
            }

            // 提取单位部分
            while (i < trimmed.length()) {
                if (trimmed[i] != ' ' && trimmed[i] != '\t') {
                    unit_part += std::tolower(trimmed[i]);
                }
                i++;
            }

            if (number_part.empty()) {
                return -1;
            }

            double number;
            try {
                number = std::stod(number_part);
            } catch (...) {
                return -1;
            }

            // 转换单位
            long long multiplier = 1;
            if (unit_part == "kb" || unit_part == "k") {
                multiplier = 1024;
            } else if (unit_part == "mb" || unit_part == "m") {
                multiplier = 1024 * 1024;
            } else if (unit_part == "gb" || unit_part == "g") {
                multiplier = 1024 * 1024 * 1024;
            } else if (unit_part == "tb" || unit_part == "t") {
                multiplier = 1024LL * 1024 * 1024 * 1024;
            } else if (!unit_part.empty()) {
                // 未知单位
                return -1;
            }

            return static_cast<long long>(number * multiplier);
        }

        void ConfigManager::validateNumericConfigs(std::vector<std::string>& errors) const {
            // 需要验证的数值配置（使用嵌套键名）
            std::map<std::string, std::pair<long long, long long>> numeric_ranges = {
                {"database.mysql.port", {1, 65535}},
                {"database.redis.port", {1, 65535}},
                {"service.port", {1, 65535}},
                {"database.mysql.pool_size", {1, 1000}},
                {"database.mysql.max_pool_size", {1, 1000}},
                {"database.redis.pool_size", {1, 1000}},
                {"database.redis.max_pool_size", {1, 1000}},
                {"service.worker_threads", {1, 1000}},
                {"performance.max_concurrent_requests", {1, 100000}},
                {"performance.request_timeout", {1, 3600}},
                {"kafka.consumer.session_timeout_ms", {1000, 300000}},
                {"kafka.consumer.max_poll_interval_ms", {1000, 600000}},
                {"logging.max_files", {1, 1000}}
            };

            // 需要特殊处理的大小配置（支持单位）
            std::map<std::string, std::pair<long long, long long>> size_ranges = {
                {"logging.max_file_size", {1024, 1073741824}} // 1KB to 1GB
            };

            // 验证普通数值配置
            for (const auto& [key, range] : numeric_ranges) {
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    try {
                        long long value = std::stoll(it->second);
                        if (value < range.first || value > range.second) {
                            errors.push_back("Value out of range for " + key + ": " + it->second +
                                           " (valid range: " + std::to_string(range.first) + "-" + std::to_string(range.second) + ")");
                        }
                    } catch (...) {
                        errors.push_back("Invalid numeric format for " + key + ": " + it->second);
                    }
                }
            }

            // 验证大小配置（支持单位）
            for (const auto& [key, range] : size_ranges) {
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    long long value = parseSizeValue(it->second);
                    if (value == -1) {
                        errors.push_back("Invalid size format for " + key + ": " + it->second +
                                       " (expected format: number with optional unit like 100MB, 1GB)");
                    } else if (value < range.first || value > range.second) {
                        errors.push_back("Size out of range for " + key + ": " + it->second +
                                       " (valid range: " + std::to_string(range.first) + "-" + std::to_string(range.second) + " bytes)");
                    }
                }
            }
        }

        void ConfigManager::validateBooleanConfigs(std::vector<std::string>& errors) const {
            // 需要验证的布尔配置（使用嵌套键名）
            std::vector<std::string> boolean_keys = {
                "service.enable_https", "logging.console", "logging.enable_async_logging", "kafka.consumer.enable_auto_commit"
            };

            for (const auto& key : boolean_keys) {
                auto it = configs_.find(key);
                if (it != configs_.end()) {
                    std::string lower_value = it->second;
                    std::transform(lower_value.begin(), lower_value.end(), lower_value.begin(), ::tolower);

                    if (lower_value != "true" && lower_value != "false" &&
                        lower_value != "1" && lower_value != "0" &&
                        lower_value != "yes" && lower_value != "no" &&
                        lower_value != "on" && lower_value != "off") {
                        errors.push_back("Invalid boolean value for " + key + ": " + it->second);
                    }
                }
            }
        }

        void ConfigManager::validateFilePathConfigs(std::vector<std::string>& errors) const {
            // 需要验证的文件路径配置（使用嵌套键名）
            std::vector<std::string> file_path_keys = {
                "service.ssl_cert_file", "service.ssl_key_file", "logging.file"
            };

            for (const auto& key : file_path_keys) {
                auto it = configs_.find(key);
                if (it != configs_.end() && !it->second.empty()) {
                    // 检查路径是否包含非法字符
                    const std::string& path = it->second;
                    if (path.find('\0') != std::string::npos) {
                        errors.push_back("File path contains null character for " + key + ": " + path);
                    }

                    // 检查路径长度
                    if (path.length() > 4096) {
                        errors.push_back("File path too long for " + key + " (max 4096 characters)");
                    }
                }
            }
        }

        void ConfigManager::validateNetworkConfigs(std::vector<std::string>& errors) const {
            // 验证主机地址格式（使用嵌套键名）
            std::vector<std::string> host_keys = {"database.mysql.host", "database.redis.host", "service.host"};

            for (const auto& key : host_keys) {
                auto it = configs_.find(key);
                if (it != configs_.end() && !it->second.empty()) {
                    const std::string& host = it->second;

                    // 基本格式检查
                    if (host.length() > 253) {
                        errors.push_back("Host name too long for " + key + " (max 253 characters)");
                    }

                    // 检查是否包含非法字符
                    if (host.find(' ') != std::string::npos) {
                        errors.push_back("Host name contains spaces for " + key + ": " + host);
                    }
                }
            }

            // 验证Kafka brokers格式
            auto kafka_it = configs_.find("kafka.brokers");
            if (kafka_it != configs_.end() && !kafka_it->second.empty()) {
                const std::string& brokers = kafka_it->second;
                if (brokers.find(':') == std::string::npos) {
                    errors.push_back("Kafka brokers must include port (host:port format): " + brokers);
                }
            }
        }

        // ==================== 配置结构体实现 ====================

        DatabaseConfig DatabaseConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            DatabaseConfig config;

            config_manager.get<bool>("mysql.thread_pool_core_size", 4);

            // 基本连接配置
            config.mysql_host = config_manager.get<std::string>("mysql.host", config.mysql_host);
            config.mysql_port = config_manager.get<int>("mysql.port", config.mysql_port);
            config.mysql_user = config_manager.get<std::string>("mysql.username", config.mysql_user);
            config.mysql_password = config_manager.get<std::string>("mysql.password", config.mysql_password);
            config.mysql_database = config_manager.get<std::string>("mysql.database", config.mysql_database);
            config.mysql_charset = config_manager.get<std::string>("mysql.charset", config.mysql_charset);
            config.mysql_collation = config_manager.get<std::string>("mysql.mysql_collation", config.mysql_collation);

            config.enable_async_operations = config_manager.get<bool>("mysql.enable_async_operations", config.enable_async_operations);
            config.thread_pool_core_size = config_manager.get<int>("mysql.thread_pool_core_size", 4);
            config.thread_pool_max_size = config_manager.get<int>("mysql.thread_pool_max_size", 16);
            config.thread_pool_keep_alive_ms = config_manager.get<int>("mysql.thread_pool_keep_alive_ms", 60000);
            config.thread_pool_queue_capacity = config_manager.get<int>("mysql.thread_pool_queue_capacity", 1000);
            config.thread_pool_enable_monitoring = config_manager.get<bool>("mysql.thread_pool_enable_monitoring", true);
            // 连接池配置
            config.mysql_pool_size = config_manager.get<size_t>("mysql.pool.initial_size", config.mysql_pool_size);
            config.mysql_max_pool_size = config_manager.get<size_t>("mysql.pool.max_size", config.mysql_max_pool_size);
            config.mysql_min_pool_size = config_manager.get<size_t>("mysql.pool.min_size", config.mysql_min_pool_size);
            config.mysql_pool_timeout_ms = config_manager.get<int>("mysql.pool.max_wait_time", config.mysql_pool_timeout_ms);
            config.mysql_pool_max_idle_time_ms = config_manager.get<int>("mysql.pool.max_idle_time_ms", config.mysql_pool_max_idle_time_ms);
            config.mysql_pool_test_on_borrow = config_manager.get<bool>("mysql.pool.test_on_borrow", config.mysql_pool_test_on_borrow);
            config.mysql_pool_test_on_return = config_manager.get<bool>("mysql.pool.test_on_return", config.mysql_pool_test_on_return);
            config.mysql_pool_test_while_idle = config_manager.get<bool>("mysql.pool.test_while_idle", config.mysql_pool_test_while_idle);
            config.mysql_pool_validation_interval_ms = config_manager.get<int>("mysql.pool.validation_interval_ms", config.mysql_pool_validation_interval_ms);

            // 连接参数
            config.mysql_connect_timeout_ms = config_manager.get<int>("mysql.mysql_connect_timeout_ms", config.mysql_connect_timeout_ms);
            config.mysql_read_timeout_ms = config_manager.get<int>("mysql.mysql_read_timeout_ms", config.mysql_read_timeout_ms);
            config.mysql_write_timeout_ms = config_manager.get<int>("mysql.mysql_write_timeout_ms", config.mysql_write_timeout_ms);
            config.mysql_auto_reconnect = config_manager.get<bool>("mysql.mysql_auto_reconnect", config.mysql_auto_reconnect);
            config.mysql_max_reconnect_attempts = config_manager.get<int>("mysql.mysql_max_reconnect_attempts", config.mysql_max_reconnect_attempts);
            config.mysql_reconnect_interval_ms = config_manager.get<int>("mysql.mysql_reconnect_interval_ms", config.mysql_reconnect_interval_ms);

            // SSL配置
            config.mysql_use_ssl = config_manager.get<bool>("mysql.mysql_use_ssl", config.mysql_use_ssl);
            config.mysql_ssl_ca = config_manager.get<std::string>("mysql.mysql_ssl_ca", config.mysql_ssl_ca);
            config.mysql_ssl_cert = config_manager.get<std::string>("mysql.mysql_ssl_cert", config.mysql_ssl_cert);
            config.mysql_ssl_key = config_manager.get<std::string>("mysql.mysql_ssl_key", config.mysql_ssl_key);
            config.mysql_ssl_cipher = config_manager.get<std::string>("mysql.mysql_ssl_cipher", config.mysql_ssl_cipher);
            config.mysql_ssl_verify_server_cert = config_manager.get<bool>("mysql.mysql_ssl_verify_server_cert", config.mysql_ssl_verify_server_cert);

            // 性能配置
            config.mysql_max_allowed_packet = config_manager.get<int>("mysql.mysql_max_allowed_packet", config.mysql_max_allowed_packet);
            config.mysql_net_buffer_length = config_manager.get<int>("mysql.mysql_net_buffer_length", config.mysql_net_buffer_length);
            config.mysql_compress = config_manager.get<bool>("mysql.mysql_compress", config.mysql_compress);
            config.mysql_init_command = config_manager.get<std::string>("mysql.mysql_init_command", config.mysql_init_command);

            // 事务配置
            config.mysql_isolation_level = config_manager.get<std::string>("mysql.mysql_isolation_level", config.mysql_isolation_level);
            config.mysql_autocommit = config_manager.get<bool>("mysql.mysql_autocommit", config.mysql_autocommit);
            config.mysql_lock_wait_timeout = config_manager.get<int>("mysql.mysql_lock_wait_timeout", config.mysql_lock_wait_timeout);

            // LOG_ERROR("thread_pool_core_size " + std::to_string(config.thread_pool_core_size));

            return config;
        }

        void DatabaseConfig::validate() const {
            if (mysql_host.empty()) {
                LOG_ERROR("MySQL host cannot be empty");
                throw std::invalid_argument("MySQL host cannot be empty");
            }
            if (mysql_port < 1 || mysql_port > 65535) {
                LOG_ERROR("MySQL port must be between 1 and 65535");
                throw std::invalid_argument("MySQL port must be between 1 and 65535");
            }
            if (mysql_user.empty()) {
                LOG_ERROR("MySQL user cannot be empty");
                throw std::invalid_argument("MySQL user cannot be empty");
            }
            if (mysql_database.empty()) {
                LOG_ERROR("MySQL database cannot be empty");
                throw std::invalid_argument("MySQL database cannot be empty");
            }
            if (mysql_pool_size == 0) {
                LOG_ERROR("MySQL pool size must be greater than 0");
                throw std::invalid_argument("MySQL pool size must be greater than 0");
            }
            if (mysql_max_pool_size < mysql_pool_size) {
                LOG_ERROR("MySQL max pool size must be >= pool size");
                throw std::invalid_argument("MySQL max pool size must be >= pool size");
            }

            // 线程池配置验证
            if (enable_async_operations) {
                // std::cout<<thread_pool_core_size<<std::endl;
                if (thread_pool_core_size <= 0) {
                    LOG_ERROR("MySQL thread_pool_core_size must be greater than 0");
                    throw std::runtime_error("Redis thread_pool_core_size must be greater than 0");
                }
                if (thread_pool_max_size < thread_pool_core_size) {
                    LOG_ERROR("MySQL thread_pool_max_size must be >= thread_pool_core_size");
                    throw std::runtime_error("Redis thread_pool_max_size must be >= thread_pool_core_size");
                }
                if (thread_pool_queue_capacity <= 0) {
                    LOG_ERROR("MySQL thread_pool_queue_capacity must be greater than 0");
                    throw std::runtime_error("Redis thread_pool_queue_capacity must be greater than 0");
                }
                if (thread_pool_keep_alive_ms < 0) {
                    LOG_ERROR("MySQL thread_pool_keep_alive_ms must be >= 0");
                    throw std::runtime_error("Redis thread_pool_keep_alive_ms must be >= 0");
                }
            }
        }

        RedisConfig RedisConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            RedisConfig config;

            // 基本连接配置
            config.redis_host = config_manager.get<std::string>("redis.host", config.redis_host);
            config.redis_port = config_manager.get<int>("redis.port", config.redis_port);
            config.redis_password = config_manager.get<std::string>("redis.password", config.redis_password);
            config.redis_database = config_manager.get<int>("redis.database", config.redis_database);

            config.enable_async_operations = config_manager.get<bool>("redis.enable_async_operations", config.enable_async_operations);
            config.thread_pool_core_size = config_manager.get<int>("redis.thread_pool_core_size", 4);
            config.thread_pool_max_size = config_manager.get<int>("redis.thread_pool_max_size", 16);
            config.thread_pool_keep_alive_ms = config_manager.get<int>("redis.thread_pool_keep_alive_ms", 60000);
            config.thread_pool_queue_capacity = config_manager.get<int>("redis.thread_pool_queue_capacity", 1024);
            config.thread_pool_enable_monitoring = config_manager.get<bool>("redis.thread_pool_enable_monitoring", config.thread_pool_enable_monitoring);

            // 连接池配置
            config.redis_pool_size = config_manager.get<size_t>("redis.pool_size", config.redis_pool_size);
            config.redis_max_pool_size = config_manager.get<size_t>("redis.max_pool_size", config.redis_max_pool_size);
            config.redis_min_pool_size = config_manager.get<size_t>("redis.min_pool_size", config.redis_min_pool_size);
            config.redis_pool_timeout_ms = config_manager.get<int>("redis.pool_timeout_ms", config.redis_pool_timeout_ms);
            config.redis_pool_max_idle_time_ms = config_manager.get<int>("redis.pool_max_idle_time_ms", config.redis_pool_max_idle_time_ms);
            config.redis_pool_test_on_borrow = config_manager.get<bool>("redis.pool_test_on_borrow", config.redis_pool_test_on_borrow);
            config.redis_pool_test_on_return = config_manager.get<bool>("redis.pool_test_on_return", config.redis_pool_test_on_return);
            config.redis_pool_validation_interval_ms = config_manager.get<int>("redis.pool_validation_interval_ms", config.redis_pool_validation_interval_ms);

            // 连接参数
            config.redis_connect_timeout_ms = config_manager.get<int>("redis.connect_timeout_ms", config.redis_connect_timeout_ms);
            config.redis_socket_timeout_ms = config_manager.get<int>("redis.socket_timeout_ms", config.redis_socket_timeout_ms);
            config.redis_command_timeout_ms = config_manager.get<int>("redis.command_timeout_ms", config.redis_command_timeout_ms);
            config.redis_tcp_keepalive = config_manager.get<bool>("redis.tcp_keepalive", config.redis_tcp_keepalive);
            config.redis_tcp_keepalive_idle = config_manager.get<int>("redis.tcp_keepalive_idle", config.redis_tcp_keepalive_idle);
            config.redis_tcp_keepalive_interval = config_manager.get<int>("redis.tcp_keepalive_interval", config.redis_tcp_keepalive_interval);
            config.redis_tcp_keepalive_count = config_manager.get<int>("redis.tcp_keepalive_count", config.redis_tcp_keepalive_count);

            // 重试和恢复配置
            config.redis_auto_reconnect = config_manager.get<bool>("redis.auto_reconnect", config.redis_auto_reconnect);
            config.redis_max_reconnect_attempts = config_manager.get<int>("redis.max_reconnect_attempts", config.redis_max_reconnect_attempts);
            config.redis_reconnect_interval_ms = config_manager.get<int>("redis.reconnect_interval_ms", config.redis_reconnect_interval_ms);
            config.redis_retry_attempts = config_manager.get<int>("redis.retry_attempts", config.redis_retry_attempts);
            config.redis_retry_delay_ms = config_manager.get<int>("redis.retry_delay_ms", config.redis_retry_delay_ms);

            // 集群配置
            config.redis_cluster_enabled = config_manager.get<bool>("redis.cluster_enabled", config.redis_cluster_enabled);
            config.redis_cluster_max_redirections = config_manager.get<int>("redis.cluster_max_redirections", config.redis_cluster_max_redirections);
            config.redis_cluster_require_full_coverage = config_manager.get<bool>("redis.cluster_require_full_coverage", config.redis_cluster_require_full_coverage);

            // 哨兵配置
            config.redis_sentinel_enabled = config_manager.get<bool>("redis.sentinel_enabled", config.redis_sentinel_enabled);
            config.redis_sentinel_master_name = config_manager.get<std::string>("redis.sentinel_master_name", config.redis_sentinel_master_name);
            config.redis_sentinel_password = config_manager.get<std::string>("redis.sentinel_password", config.redis_sentinel_password);
            config.redis_sentinel_timeout_ms = config_manager.get<int>("redis.sentinel_timeout_ms", config.redis_sentinel_timeout_ms);

            // SSL/TLS配置
            config.redis_use_ssl = config_manager.get<bool>("redis.use_ssl", config.redis_use_ssl);
            config.redis_ssl_cert_file = config_manager.get<std::string>("redis.ssl_cert_file", config.redis_ssl_cert_file);
            config.redis_ssl_key_file = config_manager.get<std::string>("redis.ssl_key_file", config.redis_ssl_key_file);
            config.redis_ssl_ca_file = config_manager.get<std::string>("redis.ssl_ca_file", config.redis_ssl_ca_file);
            config.redis_ssl_verify_peer = config_manager.get<bool>("redis.ssl_verify_peer", config.redis_ssl_verify_peer);
            config.redis_ssl_ciphers = config_manager.get<std::string>("redis.ssl_ciphers", config.redis_ssl_ciphers);

            // 性能配置
            config.redis_pipeline_size = config_manager.get<int>("redis.pipeline_size", config.redis_pipeline_size);
            config.redis_enable_pipeline = config_manager.get<bool>("redis.enable_pipeline", config.redis_enable_pipeline);
            config.redis_max_memory_policy = config_manager.get<int>("redis.max_memory_policy", config.redis_max_memory_policy);
            config.redis_compress_data = config_manager.get<bool>("redis.compress_data", config.redis_compress_data);

            return config;
        }

        void RedisConfig::validate() const {
            if (redis_host.empty()) {
                LOG_ERROR("Redis host cannot be empty");
                throw std::invalid_argument("Redis host cannot be empty");
            }
            if (redis_port < 1 || redis_port > 65535) {
                LOG_ERROR("Redis port must be between 1 and 65535");
                throw std::invalid_argument("Redis port must be between 1 and 65535");
            }
            if (redis_database < 0 || redis_database > 15) {
                LOG_ERROR("Redis database must be between 0 and 15");
                throw std::invalid_argument("Redis database must be between 0 and 15");
            }
            if (redis_pool_size == 0) {
                LOG_ERROR("Redis pool size must be greater than 0");
                throw std::invalid_argument("Redis pool size must be greater than 0");
            }
            if (redis_max_pool_size < redis_pool_size) {
                LOG_ERROR("Redis max pool size must be >= pool size");
                throw std::invalid_argument("Redis max pool size must be >= pool size");
            }

            // 线程池配置验证
            if (enable_async_operations) {
                if (thread_pool_core_size <= 0) {
                    LOG_ERROR("Redis thread_pool_core_size must be greater than 0");
                    throw std::runtime_error("Redis thread_pool_core_size must be greater than 0");
                }
                if (thread_pool_max_size < thread_pool_core_size) {
                    LOG_ERROR("Redis thread_pool_max_size must be >= thread_pool_core_size");
                    throw std::runtime_error("Redis thread_pool_max_size must be >= thread_pool_core_size");
                }
                if (thread_pool_queue_capacity <= 0) {
                    LOG_ERROR("Redis thread_pool_queue_capacity must be greater than 0");
                    throw std::runtime_error("Redis thread_pool_queue_capacity must be greater than 0");
                }
                if (thread_pool_keep_alive_ms < 0) {
                    LOG_ERROR("Redis thread_pool_keep_alive_ms must be >= 0");
                    throw std::runtime_error("Redis thread_pool_keep_alive_ms must be >= 0");
                }
            }
        }

        ServerConfig ServerConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            ServerConfig config;

            // 基本信息 - 优先从 service 节点读取，如果不存在则回退到 server 节点
            // 这样可以同时支持两种配置文件格式
            std::string serviceName = config_manager.get<std::string>("service.name", "");
            if (serviceName.empty()) {
                serviceName = config_manager.get<std::string>("server.name", config.serviceName);
            }
            config.serviceName = serviceName;

            std::string version = config_manager.get<std::string>("service.version", "");
            if (version.empty()) {
                version = config_manager.get<std::string>("server.version", config.version);
            }
            config.version = version;

            std::string description = config_manager.get<std::string>("service.description", "");
            if (description.empty()) {
                description = config_manager.get<std::string>("server.description", config.description);
            }
            config.description = description;

            std::string environment = config_manager.get<std::string>("service.environment", "");
            if (environment.empty()) {
                environment = config_manager.get<std::string>("server.environment", config.environment);
            }
            config.environment = environment;

            config.max_connections = config_manager.get<int>("network.max_connections", config.max_connections);
            return config;
        }

        void ServerConfig::validate() const {
            std::vector<std::string> errors;

            // 验证必填字段
            if (serviceName.empty()) {
                errors.emplace_back("serviceName cannot be empty");
            }
            if (version.empty()) {
                errors.emplace_back("version cannot be empty");
            }
            if (environment.empty()) {
                errors.emplace_back("environment cannot be empty");
            }

            // 如果有错误，合并并抛出异常
            if (!errors.empty()) {
                std::ostringstream oss;
                oss << "Invalid ServerConfig: ";
                for (size_t i = 0; i < errors.size(); ++i) {
                    if (i > 0) oss << "; ";
                    oss << errors[i];
                }
                LOG_ERROR(oss.str());
                throw std::invalid_argument(oss.str());
            }
        }

        kafkaProducerConfig kafkaProducerConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            kafkaProducerConfig config;

            config.brokers = config_manager.get<std::string>("kafka.producer.brokers", config.brokers);
            config.client_id = config_manager.get<std::string>("kafka.producer.client_id", config.client_id);
            config.acks = config_manager.get<int>("kafka.producer.acks", config.acks);
            config.retries = config_manager.get<int>("kafka.producer.retries", config.retries);
            config.batch_size = config_manager.get<int>("kafka.producer.batch_size", config.batch_size);
            config.linger_ms = config_manager.get<int>("kafka.producer.linger_ms", config.linger_ms);
            config.buffer_memory = config_manager.get<int>("kafka.producer.buffer_memory", config.buffer_memory);
            config.compression_type = config_manager.get<std::string>("kafka.producer.compression_type", config.compression_type);
            config.max_request_size = config_manager.get<int>("kafka.producer.max_request_size", config.max_request_size);
            config.request_timeout_ms = config_manager.get<int>("kafka.producer.request_timeout_ms", config.request_timeout_ms);
            config.delivery_timeout_ms = config_manager.get<int>("kafka.producer.delivery_timeout_ms", config.delivery_timeout_ms);
            config.enable_idempotence = config_manager.get<bool>("kafka.producer.enable_idempotence", config.enable_idempotence);
            config.max_in_flight_requests = config_manager.get<int>("kafka.producer.max_in_flight_requests", config.max_in_flight_requests);

            // 业务配置
            config.enable_async_operations = config_manager.get<bool>("kafka.producer.enable_async_operations", config.enable_async_operations);

            // 安全配置
            config.security_protocol = config_manager.get<std::string>("kafka.producer.security_protocol", config.security_protocol);
            config.sasl_mechanism = config_manager.get<std::string>("kafka.producer.sasl.mechanism", config.sasl_mechanism);
            config.sasl_username = config_manager.get<std::string>("kafka.producer.sasl.username", config.sasl_username);
            config.sasl_password = config_manager.get<std::string>("kafka.producer.sasl.password", config.sasl_password);
            config.ssl_ca_location = config_manager.get<std::string>("kafka.producer.ssl.ca_location", config.ssl_ca_location);
            config.ssl_certificate_location = config_manager.get<std::string>("kafka.producer.ssl.certificate_location", config.ssl_certificate_location);
            config.ssl_key_location = config_manager.get<std::string>("kafka.producer.ssl.key_location", config.ssl_key_location);
            config.ssl_key_password = config_manager.get<std::string>("kafka.producer.ssl.key_password", config.ssl_key_password);

            return config;
        }

        void kafkaProducerConfig::validate() const {
            if (brokers.empty()) {
                throw std::invalid_argument("Kafka producer brokers cannot be empty");
            }
            if (acks != 0 && acks != 1 && acks != -1) {
                throw std::invalid_argument("Kafka producer acks must be '0', '1', or 'all'");
            }
            if (retries < 0) {
                throw std::invalid_argument("Kafka producer retries cannot be negative");
            }
            if (batch_size < 0) {
                throw std::invalid_argument("Kafka producer batch size cannot be negative");
            }
            if (linger_ms < 0) {
                throw std::invalid_argument("Kafka producer linger ms cannot be negative");
            }
            if (buffer_memory < 1024) {
                throw std::invalid_argument("Kafka producer buffer memory must be at least 1024 bytes");
            }
            std::set<std::string> valid_compression = {"none", "gzip", "snappy", "lz4", "zstd"};
            if (valid_compression.find(compression_type) == valid_compression.end()) {
                throw std::invalid_argument("Invalid Kafka producer compression type: " + compression_type);
            }
            if (max_request_size < 1024) {
                throw std::invalid_argument("Kafka producer max request size must be at least 1024 bytes");
            }
            if (request_timeout_ms < 1000) {
                throw std::invalid_argument("Kafka producer request timeout must be at least 1000 ms");
            }
            if (delivery_timeout_ms < request_timeout_ms) {
                throw std::invalid_argument("Kafka producer delivery timeout must be >= request timeout");
            }
            if (max_in_flight_requests < 1) {
                throw std::invalid_argument("Kafka producer max in flight requests must be at least 1");
            }
        }

        kafkaConsumerConfig kafkaConsumerConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            kafkaConsumerConfig config;

            config.brokers = config_manager.get<std::string>("kafka.consumer.brokers", config.brokers);
            config.group_id = config_manager.get<std::string>("kafka.consumer.group_id", config.group_id);
            config.client_id = config_manager.get<std::string>("kafka.consumer.client_id", config.client_id);
            config.auto_offset_reset = config_manager.get<std::string>("kafka.consumer.auto_offset_reset", config.auto_offset_reset);
            config.enable_auto_commit = config_manager.get<bool>("kafka.consumer.enable_auto_commit", config.enable_auto_commit);
            config.auto_commit_interval_ms = config_manager.get<int>("kafka.consumer.auto_commit_interval_ms", config.auto_commit_interval_ms);
            config.session_timeout_ms = config_manager.get<int>("kafka.consumer.session_timeout_ms", config.session_timeout_ms);
            config.heartbeat_interval_ms = config_manager.get<int>("kafka.consumer.heartbeat_interval_ms", config.heartbeat_interval_ms);
            config.max_poll_interval_ms = config_manager.get<int>("kafka.consumer.max_poll_interval_ms", config.max_poll_interval_ms);
            config.max_poll_records = config_manager.get<int>("kafka.consumer.max_poll_records", config.max_poll_records);
            config.fetch_min_bytes = config_manager.get<int>("kafka.consumer.fetch_min_bytes", config.fetch_min_bytes);
            config.fetch_max_wait_ms = config_manager.get<int>("kafka.consumer.fetch_max_wait_ms", config.fetch_max_wait_ms);
            config.max_partition_fetch_bytes = config_manager.get<int>("kafka.consumer.max_partition_fetch_bytes", config.max_partition_fetch_bytes);
            config.check_crcs = config_manager.get<bool>("kafka.consumer.check_crcs", config.check_crcs);
            config.isolation_level = config_manager.get<std::string>("kafka.consumer.isolation_level", config.isolation_level);

            // 业务配置
            config.enable_async_operations = config_manager.get<bool>("kafka.producer.enable_async_operations", config.enable_async_operations);

            // 安全配置
            config.security_protocol = config_manager.get<std::string>("kafka.consumer.security_protocol", config.security_protocol);
            config.sasl_mechanism = config_manager.get<std::string>("kafka.consumer.sasl.mechanism", config.sasl_mechanism);
            config.sasl_username = config_manager.get<std::string>("kafka.consumer.sasl.username", config.sasl_username);
            config.sasl_password = config_manager.get<std::string>("kafka.consumer.sasl.password", config.sasl_password);
            config.ssl_ca_location = config_manager.get<std::string>("kafka.consumer.ssl.ca_location", config.ssl_ca_location);
            config.ssl_certificate_location = config_manager.get<std::string>("kafka.consumer.ssl.certificate_location", config.ssl_certificate_location);
            config.ssl_key_location = config_manager.get<std::string>("kafka.consumer.ssl.key_location", config.ssl_key_location);
            config.ssl_key_password = config_manager.get<std::string>("kafka.consumer.ssl.key_password", config.ssl_key_password);

            return config;
        }

        void kafkaConsumerConfig::validate() const {
            if (brokers.empty()) {
                throw std::invalid_argument("Kafka consumer brokers cannot be empty");
            }
            if (auto_offset_reset != "earliest" && auto_offset_reset != "latest" && auto_offset_reset != "none") {
                throw std::invalid_argument("Kafka consumer auto offset reset must be 'earliest', 'latest', or 'none'");
            }
            if (auto_commit_interval_ms < 1000) {
                throw std::invalid_argument("Kafka consumer auto commit interval must be at least 1000 ms");
            }
            if (session_timeout_ms < 6000 || session_timeout_ms > 300000) {
                throw std::invalid_argument("Kafka consumer session timeout must be between 6000 and 300000 ms");
            }
            if (heartbeat_interval_ms >= session_timeout_ms / 3) {
                throw std::invalid_argument("Kafka consumer heartbeat interval must be < session timeout / 3");
            }
            if (max_poll_interval_ms < session_timeout_ms) {
                throw std::invalid_argument("Kafka consumer max poll interval must be >= session timeout");
            }
            if (max_poll_records < 1) {
                throw std::invalid_argument("Kafka consumer max poll records must be at least 1");
            }
            if (fetch_min_bytes < 1) {
                throw std::invalid_argument("Kafka consumer fetch min bytes must be at least 1");
            }
            if (fetch_max_wait_ms < 0) {
                throw std::invalid_argument("Kafka consumer fetch max wait cannot be negative");
            }
            if (max_partition_fetch_bytes < 1024) {
                throw std::invalid_argument("Kafka consumer max partition fetch bytes must be at least 1024");
            }
            if (isolation_level != "read_uncommitted" && isolation_level != "read_committed") {
                throw std::invalid_argument("Kafka consumer isolation level must be 'read_uncommitted' or 'read_committed'");
            }
        }

        KafkaConfig KafkaConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            KafkaConfig config;

            // 加载生产者和消费者配置
            config.producer = kafkaProducerConfig::fromConfigManager();
            config.consumer = kafkaConsumerConfig::fromConfigManager();

            config.enable = config_manager.get<bool>("kafka.enable", config.enable);
            config.topic_prefix = config_manager.get<std::string>("kafka.topic_prefix", config.topic_prefix);

            return config;
        }

        void KafkaConfig::validate() const {
            // 验证生产者和消费者配置
            try {
                producer.validate();
            } catch (const std::exception& e) {
                throw std::invalid_argument("Kafka producer config validation failed: " + std::string(e.what()));
            }

            try {
                consumer.validate();
            } catch (const std::exception& e) {
                throw std::invalid_argument("Kafka consumer config validation failed: " + std::string(e.what()));
            }

        }


        NetworkConfig NetworkConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            NetworkConfig config;

            config.bind_address = config_manager.get<std::string>("network.host", config.bind_address);
            config.listen_port = config_manager.get<int>("network.port", config.listen_port);
            config.backlog = config_manager.get<int>("network.backlog", config.backlog);
            config.worker_threads = config_manager.get<int>("network.worker_threads", config.worker_threads);
            config.io_threads = config_manager.get<int>("network.io_threads", config.io_threads);
            config.tcp_nodelay = config_manager.get<bool>("network.tcp_no_delay", config.tcp_nodelay);
            config.so_reuseaddr = config_manager.get<bool>("network.reuse_address", config.so_reuseaddr);
            config.so_reuseport = config_manager.get<bool>("network.reuse_port", config.so_reuseport);
            config.so_keepalive = config_manager.get<bool>("network.keep_alive", config.so_keepalive);
            config.send_buffer_size = config_manager.get<int>("network.send_buffer_size", config.send_buffer_size);
            config.recv_buffer_size = config_manager.get<int>("network.recv_buffer_size", config.recv_buffer_size);
            config.connection_timeout = config_manager.get<int>("network.connection_timeout", config.connection_timeout);
            config.read_timeout = config_manager.get<int>("network.read_timeout", config.read_timeout);
            config.write_timeout = config_manager.get<int>("network.write_timeout", config.write_timeout);
            config.max_connections = config_manager.get<int>("network.max_connections", config.max_connections);
            config.enable_epoll = config_manager.get<bool>("network.enable_epoll", config.enable_epoll);

            return config;
        }

        void NetworkConfig::validate() const {
            if (bind_address.empty()) {
                LOG_ERROR("Network bind address cannot be empty");
                throw std::invalid_argument("Network bind address cannot be empty");
            }
            if (listen_port < 1 || listen_port > 65535) {
                LOG_ERROR("Network listen port must be between 1 and 65535");
                throw std::invalid_argument("Network listen port must be between 1 and 65535");
            }
            if (backlog < 1) {
                LOG_ERROR("Network backlog must be greater than 0");
                throw std::invalid_argument("Network backlog must be greater than 0");
            }
            if (worker_threads < 1) {
                LOG_ERROR("Network worker threads must be greater than 0");
                throw std::invalid_argument("Network worker threads must be greater than 0");
            }
            if (io_threads < 1) {
                LOG_ERROR("Network IO threads must be greater than 0");
                throw std::invalid_argument("Network IO threads must be greater than 0");
            }
            if (send_buffer_size < 1024) {
                LOG_ERROR("Send buffer size must be at least 1024 bytes");
                throw std::invalid_argument("Send buffer size must be at least 1024 bytes");
            }
            if (recv_buffer_size < 1024) {
                LOG_ERROR("Receive buffer size must be at least 1024 bytes");
                throw std::invalid_argument("Receive buffer size must be at least 1024 bytes");
            }
            if (connection_timeout < 1) {
                LOG_ERROR("Connection timeout must be greater than 0");
                throw std::invalid_argument("Connection timeout must be greater than 0");
            }
            if (read_timeout < 1) {
                LOG_ERROR("Read timeout must be greater than 0");
                throw std::invalid_argument("Read timeout must be greater than 0");
            }
            if (write_timeout < 1) {
                LOG_ERROR("Write timeout must be greater than 0");
                throw std::invalid_argument("Write timeout must be greater than 0");
            }
            if (max_connections < 1) {
                LOG_ERROR("Max connections must be greater than 0");
                throw std::invalid_argument("Max connections must be greater than 0");
            }
        }

        SchedulerConfig SchedulerConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            SchedulerConfig config;

            config.worker_threads = config_manager.get<int>("scheduler_worker_threads", config.worker_threads);
            config.max_pending_tasks = config_manager.get<int>("scheduler_max_pending_tasks", config.max_pending_tasks);
            config.task_queue_size = config_manager.get<int>("scheduler_task_queue_size", config.task_queue_size);
            config.tick_interval_ms = config_manager.get<int>("scheduler_tick_interval_ms", config.tick_interval_ms);
            config.enable_metrics = config_manager.get<bool>("scheduler_enable_metrics", config.enable_metrics);
            config.enable_task_timeout = config_manager.get<bool>("scheduler_enable_task_timeout", config.enable_task_timeout);
            config.default_task_timeout_ms = config_manager.get<int>("scheduler_default_task_timeout_ms", config.default_task_timeout_ms);
            config.max_retry_attempts = config_manager.get<int>("scheduler_max_retry_attempts", config.max_retry_attempts);
            config.retry_delay_ms = config_manager.get<int>("scheduler_retry_delay_ms", config.retry_delay_ms);
            config.enable_priority_queue = config_manager.get<bool>("scheduler_enable_priority_queue", config.enable_priority_queue);
            config.high_priority_threshold = config_manager.get<int>("scheduler_high_priority_threshold", config.high_priority_threshold);

            return config;
        }

        void SchedulerConfig::validate() const {
            if (worker_threads < 1) {
                throw std::invalid_argument("Scheduler worker threads must be greater than 0");
            }
            if (max_pending_tasks < 1) {
                throw std::invalid_argument("Max pending tasks must be greater than 0");
            }
            if (task_queue_size < 1) {
                throw std::invalid_argument("Task queue size must be greater than 0");
            }
            if (tick_interval_ms < 1) {
                throw std::invalid_argument("Tick interval must be greater than 0");
            }
            if (default_task_timeout_ms < 1000) {
                throw std::invalid_argument("Default task timeout must be at least 1000 ms");
            }
            if (max_retry_attempts < 0) {
                throw std::invalid_argument("Max retry attempts cannot be negative");
            }
            if (retry_delay_ms < 0) {
                throw std::invalid_argument("Retry delay cannot be negative");
            }
            if (high_priority_threshold < 0) {
                throw std::invalid_argument("High priority threshold cannot be negative");
            }
        }

        ThreadPoolConfig ThreadPoolConfig::fromConfigManager() {
            auto& config_manager = ConfigManager::getInstance();
            ThreadPoolConfig config;

            config.core_pool_size = config_manager.get<int>("thread_pool.core_size", config.core_pool_size);
            config.maximum_pool_size = config_manager.get<int>("thread_pool.max_size", config.maximum_pool_size);
            config.keep_alive_time_ms = config_manager.get<int>("thread_pool.keep_alive_ms", config.keep_alive_time_ms);
            config.queue_capacity = config_manager.get<int>("thread_pool.queue_capacity", config.queue_capacity);
            config.allow_core_thread_timeout = config_manager.get<bool>("thread_pool.allow_core_timeout", config.allow_core_thread_timeout);
            config.rejection_policy = config_manager.get<std::string>("thread_pool.rejection_policy", config.rejection_policy);
            config.prestartAllCoreThreads = config_manager.get<bool>("thread_pool.prestart_core_threads", config.prestartAllCoreThreads);
            config.thread_priority = config_manager.get<int>("thread_pool.thread_priority", config.thread_priority);
            config.thread_name_prefix = config_manager.get<std::string>("thread_pool.name_prefix", config.thread_name_prefix);
            config.enable_monitoring = config_manager.get<bool>("thread_pool.enable_monitoring", config.enable_monitoring);
            config.monitoring_interval_ms = config_manager.get<int>("thread_pool.monitoring_interval_ms", config.monitoring_interval_ms);

            return config;
        }

        void ThreadPoolConfig::validate() const {
            if (core_pool_size < 0) {
                LOG_ERROR("Core pool size cannot be negative");
                throw std::invalid_argument("Core pool size cannot be negative");
            }
            if (maximum_pool_size < 1) {
                LOG_ERROR("Maximum pool size must be greater than 0");
                throw std::invalid_argument("Maximum pool size must be greater than 0");
            }
            if (maximum_pool_size < core_pool_size) {
                LOG_ERROR("Maximum pool size must be >= core pool size");
                throw std::invalid_argument("Maximum pool size must be >= core pool size");
            }
            if (keep_alive_time_ms < 0) {
                LOG_ERROR("Keep alive time cannot be negative");
                throw std::invalid_argument("Keep alive time cannot be negative");
            }
            if (queue_capacity < 0) {
                LOG_ERROR("Queue capacity cannot be negative");
                throw std::invalid_argument("Queue capacity cannot be negative");
            }
            std::set<std::string> valid_policies = {"abort", "discard", "caller_runs", "discard_oldest"};
            if (valid_policies.find(rejection_policy) == valid_policies.end()) {
                LOG_ERROR("Invalid rejection policy: " + rejection_policy);
                throw std::invalid_argument("Invalid rejection policy: " + rejection_policy);
            }
            if (thread_priority < -20 || thread_priority > 19) {
                LOG_ERROR("Thread priority must be between -20 and 19");
                throw std::invalid_argument("Thread priority must be between -20 and 19");
            }
            if (monitoring_interval_ms < 1000) {
                LOG_ERROR("Monitoring interval must be at least 1000 ms");
                throw std::invalid_argument("Monitoring interval must be at least 1000 ms");
            }
        }
    }
}