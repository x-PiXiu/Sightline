#include "logger/logger.h"
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <memory>
#include <iomanip>

// 添加标准输出的宏定义，避免循环依赖
#define SAFE_COUT(msg) do { std::cout << __FILE__ << ":" << __LINE__ << msg << std::endl; } while(0)
#define SAFE_CERR(msg) do { std::cerr << __FILE__ << ":" << __LINE__ << msg << std::endl; } while(0)

namespace common {
    namespace logger {
        
        // ConsoleSink实现
        /**
         * @brief 获取日志级别的ANSI颜色代码
         * 
         * 根据不同的日志级别返回对应的ANSI转义序列颜色代码，
         * 用于在支持ANSI的终端中显示彩色日志。
         * 
         * @param level 日志级别
         * @return std::string ANSI颜色代码字符串
         * 
         * @example
         * // LogLevel::ERROR 返回 "\033[31m" (红色)
         * // LogLevel::INFO  返回 "\033[32m" (绿色)
         */
        std::string ConsoleSink::getColorCode(LogLevel level) {
            switch (level) {
                case LogLevel::TRACE: return "\033[36m";  // 青色
                case LogLevel::DEBUG: return "\033[34m";  // 蓝色
                case LogLevel::INFO:  return "\033[32m";  // 绿色
                case LogLevel::WARN:  return "\033[33m";  // 黄色
                case LogLevel::ERROR: return "\033[31m";  // 红色
                case LogLevel::FATAL: return "\033[35m";  // 紫色
                default: return "\033[0m";   // 默认颜色
            }
        }

        /**
         * @brief 格式化控制台日志消息
         * 
         * 将LogEntry结构体中的信息格式化为带颜色的控制台输出字符串。
         * 包含时间戳、日志级别、线程ID、文件位置和消息内容。
         * 
         * @param entry 日志条目对象
         * @return std::string 格式化后的带颜色日志字符串
         * 
         * @example
         * // 输入: LogEntry包含消息"User login failed"，级别ERROR，文件auth.cpp，行号123
         * // 输出: "\033[31m[2025-07-01 10:30:45.123] [ERROR] [12345] [auth.cpp:123] User login failed\033[0m"
         */
        std::string ConsoleSink::formatConsoleMessage(const LogEntry& entry) {
            std::ostringstream oss;
            
            // 添加颜色代码
            oss << getColorCode(entry.level);
            
            // 时间戳格式: [YYYY-MM-DD HH:MM:SS.mmm]
            auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()) % 1000;
            
            oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
            oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
            
            // 日志级别格式: [LEVEL]
            oss << "[" << Logger::getInstance().levelToString(entry.level) << "] ";
            
            // 线程ID格式: [thread_id]
            oss << "[" << entry.threadId << "] ";
            
            // 文件和行号格式: [filename:line] (如果提供了文件信息)
            if (entry.file && entry.line > 0) {
                std::string filename = Logger::getInstance().extractFileName(entry.file);
                oss << "[" << filename << ":" << entry.line << "] ";
            }
            
            // 消息内容
            oss << entry.message;
            
            // 重置颜色
            oss << "\033[0m";
            
            return oss.str();
        }



        /**
         * @brief 向控制台写入日志条目
         * 
         * 将格式化后的彩色日志消息输出到标准输出流(std::cout)。
         * 
         * @param entry 要写入的日志条目
         * 
         * @example
         * // 输出一条红色的错误日志到控制台
         * // [2025-07-01 10:30:45.123] [ERROR] [12345] [auth.cpp:123] User login failed
         */
        void ConsoleSink::write(const LogEntry& entry) {
            std::cout << formatConsoleMessage(entry) << std::endl;
        }

        // FileSink实现
        /**
         * @brief 构造函数，初始化文件日志输出
         * 
         * 打开指定的文件用于日志记录，如果文件不存在则创建新文件。
         * 
         * @param filename 日志文件路径
         * 
         * @example
         * // 创建一个文件日志输出实例
         * // FileSink sink("app.log");
         */
        FileSink::FileSink(const std::string& filename):
            filename_(filename),
            max_file_size_(512 * 1024 * 1024),
            max_files_(5),
            current_file_size_(0) {
            file_.open(filename, std::ios::app);
            if (file_.is_open()) {
                current_file_size_ = file_.tellp();
            }
        }

        FileSink::FileSink(const std::string& filename, size_t max_file_size, int max_files) :
            filename_(filename),
            max_file_size_(max_file_size),
            max_files_(max_files),
            current_file_size_(0) {
            file_.open(filename, std::ios::app);
            if (file_.is_open()) {
                current_file_size_ = file_.tellp();
            }
        }

        /**
         * @brief 析构函数，确保文件正确关闭
         * 
         * 在对象销毁时确保日志文件被正确关闭，防止数据丢失。
         */
        FileSink::~FileSink() {
            if (file_.is_open()) {
                file_.close();
            }
        }

        /**
         * @brief 轮转日志文件
         * 
         * 当当前日志文件大小超过设定的最大值时，执行文件轮转操作。
         * 轮转过程：
         * 1. 关闭当前日志文件
         * 2. 将现有的日志文件按序号重命名（example.log.1, example.log.2, ...）
         * 3. 保留最多max_files_个历史文件，超出的文件会被删除
         * 4. 创建新的日志文件
         * 
         * @example
         * // 假设max_files_ = 3，当前有example.log文件
         * // 轮转后会生成: example.log.1 (原example.log重命名)
         * // 新的example.log文件被创建用于记录新日志
         * // 如果之前已有example.log.1和example.log.2，则example.log.2会被删除
         */
        void FileSink::rotateLogFile() {
            // 确保当前文件已关闭
            if (file_.is_open()) {
                file_.close();
            }
            
            // 按序号重命名现有日志文件（从大到小）
            // 例如：example.log.2 -> example.log.3, example.log.1 -> example.log.2
            for (int i = max_files_ - 1; i > 0; --i) {
                std::string old_name = filename_ + "." + std::to_string(i);
                std::string new_name = filename_ + "." + std::to_string(i + 1);
                std::rename(old_name.c_str(), new_name.c_str());
            }
            
            // 将当前日志文件重命名为第一个备份文件
            std::string first_backup = filename_ + ".1";
            std::rename(filename_.c_str(), first_backup.c_str());
            
            // 重新打开日志文件（创建新的空文件）
            file_.open(filename_, std::ios::out | std::ios::trunc);
            current_file_size_ = 0;  // 重置文件大小计数器
        }

        /**
         * @brief 向文件写入日志条目
         * 
         * 将日志条目格式化后写入到日志文件中，不包含颜色代码。
         * 使用互斥锁确保线程安全。
         * 
         * @param entry 要写入的日志条目
         * 
         * @example
         * // 向文件写入一条日志
         * // [2025-07-01 10:30:45.123] [INFO ] [12345] [service.cpp:456] Service started successfully
         */
        void FileSink::write(const LogEntry& entry) {
            std::lock_guard<std::mutex> lock(fileMutex_);
            if (!file_.is_open()) {
                return;
            }
            
            // 格式化文件日志消息（不包含颜色代码）
            std::ostringstream oss;
            
            // 时间戳格式: [YYYY-MM-DD HH:MM:SS.mmm]
            auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()) % 1000;
            
            oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
            oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
            
            // 日志级别格式: [LEVEL]
            oss << "[" << Logger::getInstance().levelToString(entry.level) << "] ";
            
            // 线程ID格式: [thread_id]
            oss << "[" << entry.threadId << "] ";
            
            // 文件和行号格式: [filename:line] (如果提供了文件信息)
            if (entry.file && entry.line > 0) {
                std::string filename = Logger::getInstance().extractFileName(entry.file);
                oss << "[" << filename << ":" << entry.line << "] ";
            }
            
            // 消息内容
            oss << entry.message;
            
            std::string message = oss.str();
            size_t message_size = message.length() + 1; // +1 for newline
            
            // 检查是否需要轮转文件
            if (current_file_size_ + message_size > max_file_size_) {
                rotateLogFile();
            }
            
            // 写入日志消息并刷新到磁盘
            file_ << message << std::endl;
            current_file_size_ += message_size;
            file_.flush();
        }

        /**
         * @brief 刷新文件输出缓冲区
         * 
         * 强制将文件输出缓冲区中的数据写入到磁盘，确保日志数据不会丢失。
         * 使用互斥锁确保线程安全。
         */
        void FileSink::flush() {
            std::lock_guard<std::mutex> lock(fileMutex_);
            if (file_.is_open()) {
                file_.flush();
            }
        }

        Logger& Logger::getInstance() {
            static Logger instance;
            return instance;
        }

        /**
         * @brief 构造函数，初始化Logger实例
         * 
         * 设置默认的日志级别为INFO，启用控制台输出，禁用文件输出和异步日志。
         * 初始化文件轮转参数和线程池支持。
         */
        Logger::Logger() : 
            log_level_(LogLevel::INFO),
            console_enabled_(true),
            file_enabled_(false){
            
            // 🔧 修复：使用标准输出避免循环依赖
            SAFE_COUT("[Logger] Logger instance created");
        }

        /**
         * @brief 析构函数，清理Logger资源
         * 
         * 确保所有异步日志都被处理完毕，然后关闭所有相关资源。
         * 不在析构函数中使用Logger自身的方法，避免循环依赖。
         */
        Logger::~Logger() {
            // 确保所有异步日志都被处理
            is_shutting_down_ = true;
            queue_cv_.notify_all();
            
            if (async_thread_ && async_thread_->joinable()) {
                async_thread_->join();
            }
        }

        /**
         * @brief 记录TRACE级别日志
         * 
         * 当日志级别设置为TRACE或更低时，记录详细的调试信息。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::trace(const std::string& message, const char* file, int line) {
            if (log_level_ <= LogLevel::TRACE) {
                LogEntry entry(message, LogLevel::TRACE, file, line);
                logMessage(entry);
            }
        }

        /**
         * @brief 记录DEBUG级别日志
         * 
         * 当日志级别设置为DEBUG或更低时，记录调试信息。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::debug(const std::string& message, const char* file, int line) {
            if (log_level_ <= LogLevel::DEBUG) {
                LogEntry entry(message, LogLevel::DEBUG, file, line);
                logMessage(entry);
            }
        }

        /**
         * @brief 记录INFO级别日志
         * 
         * 当日志级别设置为INFO或更低时，记录一般信息。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::info(const std::string& message, const char* file, int line) {
            if (log_level_ <= LogLevel::INFO) {
                LogEntry entry(message, LogLevel::INFO, file, line);
                logMessage(entry);
            }
        }

        /**
         * @brief 记录WARN级别日志
         * 
         * 当日志级别设置为WARN或更低时，记录警告信息。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::warn(const std::string& message, const char* file, int line) {
            if (log_level_ <= LogLevel::WARN) {
                LogEntry entry(message, LogLevel::WARN, file, line);
                logMessage(entry);
            }
        }

        /**
         * @brief 记录ERROR级别日志
         * 
         * 当日志级别设置为ERROR或更低时，记录错误信息。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::error(const std::string& message, const char* file, int line) {
            if (log_level_ <= LogLevel::ERROR) {
                LogEntry entry(message, LogLevel::ERROR, file, line);
                logMessage(entry);
            }
        }

        /**
         * @brief 记录FATAL级别日志
         * 
         * 总是记录致命错误信息，不受日志级别设置影响。
         * 
         * @param message 日志消息内容
         * @param file 源文件名（通常由宏自动填充）
         * @param line 行号（通常由宏自动填充）
         */
        void Logger::fatal(const std::string& message, const char* file, int line) {
            LogEntry entry(message, LogLevel::FATAL, file, line);
            logMessage(entry);
        }

        /**
         * @brief 格式化日志消息（已废弃，保留为兼容性）
         * 
         * 此函数已废弃，保留仅为向后兼容。新的实现使用LogEntry结构体。
         * 
         * @deprecated 使用LogEntry和Sink机制替代
         */
        std::string Logger::formatMessage(const LogEntry& entry) {
            std::ostringstream oss;
            
            // 时间戳
            auto time_t = std::chrono::system_clock::to_time_t(entry.timestamp);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                entry.timestamp.time_since_epoch()) % 1000;
            
            oss << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
            oss << '.' << std::setfill('0') << std::setw(3) << ms.count() << "] ";
            
            // 日志级别
            oss << "[" << levelToString(entry.level) << "] ";
            
            // 线程ID
            oss << "[" << entry.threadId << "] ";
            
            // 文件和行号
            if (entry.file && entry.line > 0) {
                std::string filename = extractFileName(entry.file);
                oss << "[" << filename << ":" << entry.line << "] ";
            }
            
            // 消息内容
            oss << entry.message;
            
            return oss.str();
        }

        /**
         * @brief 输出日志消息到所有已注册的Sink
         * 
         * 根据是否启用异步日志，选择同步或异步方式将日志消息发送到所有Sink。
         * 
         * @param entry 要输出的日志条目
         */
        void Logger::logMessage(const LogEntry& entry) {
            // 限频护栏：TRACE~WARN 按调用点(file:line)限流，ERROR/FATAL 直通
            if (rate_limit_per_sec_ > 0 && entry.level <= LogLevel::WARN) {
                if (!rateLimitAllows(entry)) return;
            }
            dispatch(entry);
        }

        void Logger::dispatch(const LogEntry& entry) {
            if (async_logging_) {
                asyncLogMessage(entry);
            } else {
                // 同步日志记录到所有Sinks
                std::lock_guard<std::mutex> lock(sinks_mutex_);
                for (const auto& sink : sinks_) {
                    sink->write(entry);
                }
            }
        }

        bool Logger::rateLimitAllows(const LogEntry& entry) {
            using namespace std::chrono;
            std::lock_guard<std::mutex> lock(rate_mutex_);

            // 签名表上限保护：异常多的调用点直接放行（不计数）
            std::string key = std::string(entry.file ? entry.file : "?") +
                              ":" + std::to_string(entry.line);
            auto it = rate_windows_.find(key);
            if (it == rate_windows_.end()) {
                if (rate_windows_.size() >= 1024) return true;
                it = rate_windows_.emplace(key, RateWindow{}).first;
            }
            auto& w = it->second;
            auto now = steady_clock::now();

            if (now - w.window_start >= seconds(1)) {
                // 窗口滚动：上窗口有丢弃则补一条聚合摘要（直通，不再过滤）
                if (w.suppressed > 0) {
                    LogEntry summary("[rate-limit] suppressed " + std::to_string(w.suppressed) +
                                         " logs from " + key + " in last 1s",
                                     LogLevel::WARN, entry.file, entry.line);
                    dispatch(summary);
                }
                w.window_start = now;
                w.count = 0;
                w.suppressed = 0;
            }

            if (w.count < rate_limit_per_sec_) {
                ++w.count;
                return true;
            }
            ++w.suppressed;
            return false;
        }
        
        /**
         * @brief 异步处理日志消息
         *
         * 将日志条目添加到异步处理队列中，由专门的工作线程处理。
         *
         * ⭐⭐⭐ 内存泄漏修复（2025-12-31）：添加队列大小限制，防止无限增长
         *
         * @param entry 要异步处理的日志条目
         */
        void Logger::asyncLogMessage(const LogEntry& entry) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex_);

                // 队列满时分级丢弃：低级别(TRACE~INFO)直接丢新条目；
                // WARN 以上才有资格挤掉最旧的——"丢 DEBUG 保 ERROR"
                if (async_log_queue_.size() >= MAX_ASYNC_QUEUE_SIZE) {
                    if (entry.level <= LogLevel::INFO) {
                        async_queue_dropped_count_.fetch_add(1);
                        return;   // 低级别不入队
                    }
                    async_log_queue_.pop();   // 高级别：丢最旧腾位
                    async_queue_dropped_count_.fetch_add(1);

                    // 定期警告（每丢弃1000条）
                    size_t dropped = async_queue_dropped_count_.load();
                    if (dropped % 1000 == 0) {
                        // 使用标准输出避免递归死锁
                        std::cerr << "[Logger] Async log queue full (max=" << MAX_ASYNC_QUEUE_SIZE
                                 << "), " << dropped << " messages dropped" << std::endl;
                    }
                }

                async_log_queue_.push(entry);
            }
            queue_cv_.notify_one();
        }
        
        /**
         * @brief 启动异步处理线程
         * 
         * 创建并启动异步日志处理工作线程。
         */
        void Logger::startAsyncThread() {
            if (async_thread_) return;   // 幂等：重复启用不重复建线程
            async_thread_ = std::make_unique<std::thread>(&Logger::asyncLogWorker, this);
        }
        
        /**
         * @brief 异步日志工作线程函数
         * 
         * 异步处理队列中的日志条目，将它们输出到所有已注册的Sink。
         */
        void Logger::asyncLogWorker() {
            // 批量写出：整队列 swap 后一次性下发（参考 EventLoop::doPendingFunctors
            // 的 swap 模式）——一次加锁搬走一批，写盘不再逐条抢锁
            while (!is_shutting_down_) {
                std::queue<LogEntry> batch;
                {
                    std::unique_lock<std::mutex> lock(queue_mutex_);
                    queue_cv_.wait(lock, [this] {
                        return !async_log_queue_.empty() || is_shutting_down_;
                    });

                    if (is_shutting_down_ && async_log_queue_.empty()) {
                        break;
                    }
                    batch.swap(async_log_queue_);   // O(1) 整批搬走
                }

                std::lock_guard<std::mutex> lock(sinks_mutex_);
                while (!batch.empty()) {
                    for (const auto& sink : sinks_) {
                        sink->write(batch.front());
                    }
                    batch.pop();
                }
            }
        }

        /*
        // 已废弃的文件操作方法，功能已移至FileSink类实现
        bool Logger::initializeFileLogging() {
            try {
                log_file_.open(log_file_path_, std::ios::app);
                if (log_file_.is_open()) {
                    current_file_size_ = log_file_.tellp();
                    return true;
                }
                return false;
            } catch (const std::exception& e) {
                SAFE_CERR("[Logger] Failed to open log file: " + std::string(e.what()));
                return false;
            }
        }
        */

        /*
        // 已废弃的文件操作方法，功能已移至FileSink类实现
        void Logger::rotateLogFile() {
            // 确保当前文件已关闭
            if (log_file_.is_open()) {
                log_file_.close();
            }
            
            // 按序号重命名现有日志文件（从大到小）
            // 例如：example.log.2 -> example.log.3, example.log.1 -> example.log.2
            for (int i = max_files_ - 1; i > 0; --i) {
                std::string old_name = log_file_path_ + "." + std::to_string(i);
                std::string new_name = log_file_path_ + "." + std::to_string(i + 1);
                std::rename(old_name.c_str(), new_name.c_str());
            }
            
            // 将当前日志文件重命名为第一个备份文件
            std::string first_backup = log_file_path_ + ".1";
            std::rename(log_file_path_.c_str(), first_backup.c_str());
            
            // 重新打开日志文件（创建新的空文件）
            log_file_.open(log_file_path_, std::ios::out | std::ios::trunc);
            current_file_size_ = 0;  // 重置文件大小计数器
        }
        */

        /*
        // 已废弃的文件操作方法，功能已移至FileSink类实现
        void Logger::writeToFile(const std::string& message) {
            // 检查文件是否已打开
            if (!log_file_.is_open()) {
                return;
            }
            
            // 计算消息大小（包括换行符）
            size_t message_size = message.length() + 1; // +1 for newline
            
            // 检查是否需要轮转文件
            if (current_file_size_ + message_size > max_file_size_) {
                rotateLogFile();
            }
            
            // 写入日志消息并刷新到磁盘
            log_file_ << message << std::endl;
            current_file_size_ += message_size;
            log_file_.flush();  // 确保数据写入磁盘
        }
        */

        std::string Logger::levelToString(LogLevel level) {
            switch (level) {
                case LogLevel::TRACE: return "TRACE";
                case LogLevel::DEBUG: return "DEBUG";
                case LogLevel::INFO: return "INFO ";
                case LogLevel::WARN: return "WARN ";
                case LogLevel::ERROR: return "ERROR";
                case LogLevel::FATAL: return "FATAL";
                default: return "UNKNOWN";
            }
        }

        LogLevel Logger::stringToLogLevel(const std::string& level_str) {
            std::string level = level_str;
            std::transform(level.begin(), level.end(), level.begin(), ::toupper);
            
            if (level == "TRACE") return LogLevel::TRACE;
            if (level == "DEBUG") return LogLevel::DEBUG;
            if (level == "INFO") return LogLevel::INFO;
            if (level == "WARN") return LogLevel::WARN;
            if (level == "ERROR") return LogLevel::ERROR;
            if (level == "FATAL") return LogLevel::FATAL;
            
            return LogLevel::INFO; // 默认级别
        }

        std::string Logger::getCurrentTimeString() {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()) % 1000;
            
            std::ostringstream oss;
            oss << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S");
            oss << '.' << std::setfill('0') << std::setw(3) << ms.count();
            
            return oss.str();
        }

        std::string Logger::extractFileName(const std::string& file_path) {
            size_t pos = file_path.find_last_of("/\\");
            if (pos != std::string::npos) {
                return file_path.substr(pos + 1);
            }
            return file_path;
        }

        void Logger::addSink(std::unique_ptr<LogSink> sink) {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            sinks_.push_back(std::move(sink));
        }

        void Logger::clearSinks() {
            std::lock_guard<std::mutex> lock(sinks_mutex_);
            sinks_.clear();
        }
    }
}