#ifndef LOGGER_HPP
#define LOGGER_HPP

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <sstream>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <thread>
#include <errno.h>
#include <cstring>
#include <unordered_map>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

enum class LogLevel {
    ERR,
    WARN,
    INFO,
    DEBUG,
    TRACE
};

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void init(LogLevel level, const std::string& log_file, bool console_output = true, bool structured = false) {
        std::scoped_lock lock(config_mutex_, output_mutex_);
        level_ = level;
        log_file_ = log_file;
        console_output_ = console_output;
        structured_logging_ = structured;
        module_levels_.clear();

        if (file_.is_open()) file_.close();
        if (!log_file_.empty()) file_.open(log_file_, std::ios::app);
    }

    void set_level(LogLevel level) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        level_ = level;
    }

    void set_module_level(const std::string& module, LogLevel level) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        module_levels_[module] = level;
    }

    LogLevel get_level() const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return level_;
    }

    LogLevel get_module_level(const std::string& module) const {
        std::lock_guard<std::mutex> lock(config_mutex_);
        return get_module_level_unlocked(module);
    }

private:
    // 调用方必须已持有 config_mutex_
    LogLevel get_module_level_unlocked(const std::string& module) const {
        auto it = module_levels_.find(module);
        if (it != module_levels_.end()) {
            return it->second;
        }
        return level_;
    }

public:

    void enable_structured_logging(bool enable) {
        std::lock_guard<std::mutex> lock(config_mutex_);
        structured_logging_ = enable;
    }

    template <typename MessageFactory>
    void log_lazy(LogLevel level, const std::string& tag, MessageFactory&& message_factory) {
        bool structured = false;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (level > get_module_level_unlocked(tag)) return;
            structured = structured_logging_;
        }
        const std::string message = message_factory();
        write_output(format_output(level, tag, message, structured));
    }

    void log(LogLevel level, const std::string& tag, const std::string& msg) {
        bool structured = false;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (level > get_module_level_unlocked(tag)) return;
            structured = structured_logging_;
        }
        write_output(format_output(level, tag, msg, structured));
    }

    void log(LogLevel level, const std::string& tag, const std::string& msg, bool structured) {
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            if (level > get_module_level_unlocked(tag)) return;
        }
        write_output(format_output(level, tag, msg, structured));
    }

private:
    static std::string format_output(LogLevel level, const std::string& tag,
                                     const std::string& msg, bool structured) {
        std::string level_str;
        switch (level) {
            case LogLevel::ERR: level_str = "ERROR"; break;
            case LogLevel::WARN: level_str = "WARN"; break;
            case LogLevel::INFO: level_str = "INFO"; break;
            case LogLevel::DEBUG: level_str = "DEBUG"; break;
            case LogLevel::TRACE: level_str = "TRACE"; break;
        }

        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        if (structured) {
            json j;
            j["timestamp"] = get_timestamp_str(time, ms);
            j["level"] = level_str;
            j["module"] = tag;
            j["message"] = msg;
            j["thread_id"] = std::hash<std::thread::id>{}(std::this_thread::get_id());
            return j.dump();
        }

        std::tm tm_buf;
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif
        std::ostringstream output;
        output << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "."
               << std::setfill('0') << std::setw(3) << ms.count() << "]"
               << "[" << level_str << "] " << tag << ": " << msg;
        return output.str();
    }

    void write_output(const std::string& output) {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (console_output_) std::cerr << output << std::endl;

        if (file_.is_open()) {
            file_ << output << std::endl;
            if (file_.fail()) {
                file_.flush();
                file_.clear();
                file_.close();
                file_.open(log_file_, std::ios::app);
                if (file_.is_open()) file_ << output << std::endl;
                if (!file_.is_open()) {
                    std::cerr << "[Logger ERROR] Cannot open log file: "
                              << log_file_ << " (" << strerror(errno) << ")" << std::endl;
                }
            }
        }
    }

public:
    void e(const std::string& tag, const std::string& msg) { log(LogLevel::ERR, tag, msg); }
    void w(const std::string& tag, const std::string& msg) { log(LogLevel::WARN, tag, msg); }
    void i(const std::string& tag, const std::string& msg) { log(LogLevel::INFO, tag, msg); }
    void d(const std::string& tag, const std::string& msg) { log(LogLevel::DEBUG, tag, msg); }
    void t(const std::string& tag, const std::string& msg) { log(LogLevel::TRACE, tag, msg); }

private:
    Logger() : level_(LogLevel::INFO), log_file_(""), console_output_(true), structured_logging_(false) {}
    ~Logger() {
        std::lock_guard<std::mutex> lock(output_mutex_);
        if (file_.is_open()) file_.close();
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    static std::string get_timestamp_str(std::time_t time, std::chrono::milliseconds ms) {
        std::tm tm_buf;
        #ifdef _WIN32
        localtime_s(&tm_buf, &time);
        #else
        localtime_r(&time, &tm_buf);
        #endif
        std::ostringstream ts;
        ts << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "."
           << std::setfill('0') << std::setw(3) << ms.count();
        return ts.str();
    }

    LogLevel level_;
    std::string log_file_;
    bool console_output_;
    bool structured_logging_;
    std::ofstream file_;
    mutable std::mutex config_mutex_;
    mutable std::mutex output_mutex_;
    std::unordered_map<std::string, LogLevel> module_levels_;
};

// 兼容旧的宏定义
#define LOG_E(tag, msg) Logger::instance().log_lazy(LogLevel::ERR, tag, [&]() { return (msg); })
#define LOG_W(tag, msg) Logger::instance().log_lazy(LogLevel::WARN, tag, [&]() { return (msg); })
#define LOG_I(tag, msg) Logger::instance().log_lazy(LogLevel::INFO, tag, [&]() { return (msg); })
#define LOG_D(tag, msg) Logger::instance().log_lazy(LogLevel::DEBUG, tag, [&]() { return (msg); })
#define LOG_T(tag, msg) Logger::instance().log_lazy(LogLevel::TRACE, tag, [&]() { return (msg); })

// 结构化日志宏（线程安全：直接传递 structured 参数，无需切换全局状态）
#define LOG_S_E(tag, msg) Logger::instance().log(LogLevel::ERR, tag, msg, true)
#define LOG_S_W(tag, msg) Logger::instance().log(LogLevel::WARN, tag, msg, true)
#define LOG_S_I(tag, msg) Logger::instance().log(LogLevel::INFO, tag, msg, true)
#define LOG_S_D(tag, msg) Logger::instance().log(LogLevel::DEBUG, tag, msg, true)
#define LOG_S_T(tag, msg) Logger::instance().log(LogLevel::TRACE, tag, msg, true)

#endif
