//
// Created by 29108 on 2025/6/29.
//

#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <memory>
#include <mutex>
#include <iostream>
#include <fstream>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <vector>

namespace common {
    namespace logger {
        /**
         * @brief 日志级别枚举
         * 
         * 定义了不同的日志级别，数字越小级别越高
         */
        enum class LogLevel {
            TRACE = 0,  ///< 跟踪信息，最详细的日志级别
            DEBUG = 1,  ///< 调试信息，用于调试过程中输出
            INFO = 2,   ///< 一般信息，程序正常运行时的信息
            WARN = 3,   ///< 警告信息，潜在的问题但不影响程序运行
            ERROR = 4,  ///< 错误信息，发生了错误但程序可以继续运行
            FATAL = 5   ///< 致命错误，程序无法继续运行
        };

        /**
         * @brief 日志条目结构体
         * 
         * 包含一条日志的所有必要信息，用于统一处理不同来源的日志。
         */
        struct LogEntry {
            std::string message;                    ///< 日志消息内容
            LogLevel level;                         ///< 日志级别
            std::chrono::system_clock::time_point timestamp;  ///< 时间戳
            std::thread::id threadId;               ///< 线程ID
            const char* file;                       ///< 源文件名
            int line;                               ///< 行号

            /**
             * @brief 构造函数
             * 
             * @param msg 日志消息
             * @param lvl 日志级别
             * @param file 源文件名
             * @param line 行号
             */
            LogEntry(const std::string& msg, LogLevel lvl, const char* file, int line)
                : message(msg), level(lvl),
                  timestamp(std::chrono::system_clock::now()),
                  threadId(std::this_thread::get_id()),
                  file(file), line(line) {}
        };

        /**
         * @brief 日志输出接口抽象基类
         * 
         * 定义了日志输出的基本接口，所有具体的日志输出实现都需要继承此类。
         */
        class LogSink {
        public:
            virtual ~LogSink() = default;
            
            /**
             * @brief 写入日志条目
             * 
             * 纯虚函数，具体的日志输出实现需要重写此方法。
             * 
             * @param entry 要写入的日志条目
             */
            virtual void write(const LogEntry& entry) = 0;
            
            /**
             * @brief 刷新输出缓冲区
             * 
             * 纯虚函数，具体的日志输出实现需要重写此方法。
             */
            virtual void flush() = 0;
        };

        /**
         * @brief 控制台日志输出实现
         * 
         * 将日志输出到标准控制台，支持ANSI颜色显示。
         */
        class ConsoleSink : public LogSink {
        public:
            /**
             * @brief 写入日志条目到控制台
             * 
             * @param entry 要写入的日志条目
             */
            void write(const LogEntry& entry) override;
            
            /**
             * @brief 刷新控制台输出缓冲区
             */
            void flush() override { std::cout.flush(); }
            
        private:
            /**
             * @brief 获取日志级别的ANSI颜色代码
             * 
             * @param level 日志级别
             * @return 对应的颜色代码字符串
             */
            std::string getColorCode(LogLevel level);
            
            /**
             * @brief 格式化控制台日志消息
             * 
             * @param entry 日志条目
             * @return 格式化后的带颜色日志字符串
             */
            std::string formatConsoleMessage(const LogEntry& entry);
        };

        /**
         * @brief 文件日志输出实现
         * 
         * 将日志输出到指定的文件中，支持文件轮转功能。
         */
        class FileSink : public LogSink {
        public:
            /**
             * @brief 构造函数
             * 
             * @param filename 日志文件路径
             */
            explicit FileSink(const std::string& filename);
            FileSink(const std::string& filename, size_t max_file_size, int max_files);
            
            /**
             * @brief 析构函数
             */
            ~FileSink();

            /**
             * @brief 写入日志条目到文件
             * 
             * @param entry 要写入的日志条目
             */
            void write(const LogEntry& entry) override;
            
            /**
             * @brief 刷新文件输出缓冲区
             */
            void flush() override;

        private:
            std::ofstream file_;        ///< 文件输出流
            std::mutex fileMutex_;      ///< 文件操作互斥锁
            std::string filename_;      ///< 文件名
            size_t max_file_size_;      ///< 最大文件大小
            int max_files_;             ///< 最大文件数量
            size_t current_file_size_;  ///< 当前文件大小
            
            /**
             * @brief 轮转日志文件
             */
            void rotateLogFile();
        };

        /**
         * @brief 日志管理器类（单例模式）
         * 
         * 负责管理所有的日志配置、日志级别控制和日志输出。
         * 使用单例模式确保全局只有一个日志管理器实例。
         */
        class Logger {
        public:
            /**
             * @brief 获取Logger单例实例
             * 
             * @return Logger& Logger实例的引用
             */
            static Logger& getInstance();
            
            // 禁用拷贝构造和赋值
            Logger(const Logger&) = delete;
            Logger& operator=(const Logger&) = delete;

            // 辅助方法
            std::string levelToString(LogLevel level);
            LogLevel stringToLogLevel(const std::string& level_str);
            std::string getCurrentTimeString();
            std::string extractFileName(const std::string& file_path);
            
            /**
             * @brief 设置日志级别
             * 
             * @param level 新的日志级别
             */
            void setLogLevel(LogLevel level) { log_level_ = level; }
            
            /**
             * @brief 启用或禁用控制台输出
             * 
             * @param enabled true启用，false禁用
             */
            void setConsoleEnabled(bool enabled) { console_enabled_ = enabled; }
            
            /**
             * @brief 启用或禁用文件输出
             * 
             * @param enabled true启用，false禁用
             */
            void setFileEnabled(bool enabled) { file_enabled_ = enabled; }
            
            /**
             * @brief 启用或禁用异步日志
             * 
             * @param enabled true启用，false禁用
             */
            void setAsyncLogging(bool enabled) { async_logging_ = enabled; }
            
            // 日志记录方法
            void trace(const std::string& message, const char* file = "", int line = 0);
            void debug(const std::string& message, const char* file = "", int line = 0);
            void info(const std::string& message, const char* file = "", int line = 0);
            void warn(const std::string& message, const char* file = "", int line = 0);
            void error(const std::string& message, const char* file = "", int line = 0);
            void fatal(const std::string& message, const char* file = "", int line = 0);

            /**
             * @brief 添加日志输出目标
             * 
             * @param sink 日志输出目标的唯一指针
             */
            void addSink(std::unique_ptr<LogSink> sink);
            
            /**
             * @brief 清除所有日志输出目标
             */
            void clearSinks();

        private:
            Logger();
            ~Logger();

            // 格式化日志消息
            std::string formatMessage(const LogEntry& entry);
            
            // 输出日志消息
            void logMessage(const LogEntry& entry);
            
            // 🔧 添加异步日志处理方法
            void asyncLogMessage(const LogEntry& entry);


            // 配置属性
            std::atomic<LogLevel> log_level_;
            std::atomic<bool> console_enabled_;
            std::atomic<bool> file_enabled_;
            std::atomic<bool> async_logging_{false};  // 🔧 添加异步日志标志
            
            // Sink管理
            std::vector<std::unique_ptr<LogSink>> sinks_;  // 日志输出目标列表
            std::mutex sinks_mutex_;                       // Sink列表互斥锁
            
            // 线程安全
            mutable std::mutex console_mutex_;
            mutable std::mutex file_mutex_;
            
            // 🔧 添加异步日志队列
            std::queue<LogEntry> async_log_queue_;
            mutable std::mutex queue_mutex_;
            std::condition_variable queue_cv_;
            std::atomic<bool> is_shutting_down_{false};
            std::unique_ptr<std::thread> async_thread_;

            // ⭐⭐⭐ 内存泄漏修复：添加队列大小限制
            static constexpr size_t MAX_ASYNC_QUEUE_SIZE = 10000;  // 最大10000条日志
            std::atomic<size_t> async_queue_dropped_count_{0};     // 丢弃计数
            
            // 启动异步处理线程
            void startAsyncThread();
            void asyncLogWorker();
        };

        // 宏定义简化日志调用
        #define LOG_TRACE(msg) common::logger::Logger::getInstance().trace(msg, __FILE__, __LINE__)
        #define LOG_DEBUG(msg) common::logger::Logger::getInstance().debug(msg, __FILE__, __LINE__)
        #define LOG_INFO(msg) common::logger::Logger::getInstance().info(msg, __FILE__, __LINE__)
        #define LOG_WARNING(msg) common::logger::Logger::getInstance().warn(msg, __FILE__, __LINE__)
        #define LOG_ERROR(msg) common::logger::Logger::getInstance().error(msg, __FILE__, __LINE__)
        #define LOG_FATAL(msg) common::logger::Logger::getInstance().fatal(msg, __FILE__, __LINE__)

    } // namespace logger
} // namespace common

#endif // LOGGER_H