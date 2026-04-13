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
        std::lock_guard<std::mutex> lock(mutex_); 
        level_ = level; 
        log_file_ = log_file; 
        console_output_ = console_output;
        structured_logging_ = structured;

        module_levels_.clear();

        if (file_.is_open()) { 
            file_.close(); 
        }

        if (!log_file_.empty()) { 
            file_.open(log_file_, std::ios::app); 
        }
    }

    void set_level(LogLevel level) { 
        std::lock_guard<std::mutex> lock(mutex_); 
        level_ = level; 
    }

    void set_module_level(const std::string& module, LogLevel level) { 
        std::lock_guard<std::mutex> lock(mutex_); 
        module_levels_[module] = level; 
    }

    LogLevel get_level() const { 
        std::lock_guard<std::mutex> lock(mutex_); 
        return level_; 
    }

    LogLevel get_module_level(const std::string& module) const { 
        std::lock_guard<std::mutex> lock(mutex_); 
        auto it = module_levels_.find(module); 
        if (it != module_levels_.end()) {
            return it->second;
        }
        return level_;
    }

    void enable_structured_logging(bool enable) {
        std::lock_guard<std::mutex> lock(mutex_);
        structured_logging_ = enable;
    }

    void log(LogLevel level, const std::string& tag, const std::string& msg) { 
        std::lock_guard<std::mutex> lock(mutex_);

        LogLevel effective_level = get_module_level(tag);
        if (level > effective_level) return;

        std::string level_str; 
        switch (level) { 
            case LogLevel::ERR: level_str = "ERROR"; break; 
            case LogLevel::WARN: level_str = "WARN"; break; 
            case LogLevel::INFO: level_str = "INFO"; break; 
            case LogLevel::DEBUG: level_str = "DEBUG"; break; 
            case LogLevel::TRACE: level_str = "TRACE"; break; 
        }

        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::string output;
        if (structured_logging_) {
            json j;
            j["timestamp"] = get_timestamp_str(time, ms);
            j["level"] = level_str;
            j["module"] = tag;
            j["message"] = msg;
            j["thread_id"] = static_cast<int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            output = j.dump();
        } else {
            std::tm tm_buf;
            #ifdef _WIN32
            localtime_s(&tm_buf, &time);
            #else
            localtime_r(&time, &tm_buf);
            #endif
            std::ostringstream ts;
            ts << "[" << std::put_time(&tm_buf, "%H:%M:%S") << "."
               << std::setfill('0') << std::setw(3) << ms.count() << "]"
               << "[" << level_str << "] " << tag << ": " << msg;
            output = ts.str();
        }

        if (console_output_) { 
            std::cerr << output << std::endl; 
        }

        if (file_.is_open()) { 
            file_ << output << std::endl; 
            if (file_.fail()) { 
                file_.flush(); 
                file_.clear(); 
                file_.close(); 
                for (int retry = 0; retry < 3; ++retry) { 
                    file_.open(log_file_, std::ios::app); 
                    if (file_.is_open()) { 
                        file_ << output << std::endl; 
                        file_.flush(); 
                        break; 
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!file_.is_open()) { 
                    std::cerr << "[Logger ERROR] Cannot open log file: " 
                              << log_file_ << " (" << strerror(errno) << ")" << std::endl; 
                }
            }
        }
    }

    void e(const std::string& tag, const std::string& msg) { log(LogLevel::ERR, tag, msg); }
    void w(const std::string& tag, const std::string& msg) { log(LogLevel::WARN, tag, msg); }
    void i(const std::string& tag, const std::string& msg) { log(LogLevel::INFO, tag, msg); }
    void d(const std::string& tag, const std::string& msg) { log(LogLevel::DEBUG, tag, msg); }
    void t(const std::string& tag, const std::string& msg) { log(LogLevel::TRACE, tag, msg); }

private:
    Logger() : level_(LogLevel::INFO), log_file_(""), console_output_(true), structured_logging_(false) {}
    ~Logger() { 
        std::lock_guard<std::mutex> lock(mutex_); 
        if (file_.is_open()) file_.close(); 
    }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::string get_timestamp_str(std::time_t time, std::chrono::milliseconds ms) {
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

    std::atomic<LogLevel> level_; 
    std::string log_file_; 
    bool console_output_; 
    bool structured_logging_; 
    std::ofstream file_; 
    mutable std::mutex mutex_;
    std::unordered_map<std::string, LogLevel> module_levels_;
};

// 兼容旧的宏定义
#define LOG_E(tag, msg) Logger::instance().e(tag, msg)
#define LOG_W(tag, msg) Logger::instance().w(tag, msg)
#define LOG_I(tag, msg) Logger::instance().i(tag, msg)
#define LOG_D(tag, msg) Logger::instance().d(tag, msg)
#define LOG_T(tag, msg) Logger::instance().t(tag, msg)

// 新增宏定义：结构化日志
#define LOG_S_E(tag, msg) Logger::instance().enable_structured_logging(true); Logger::instance().e(tag, msg); Logger::instance().enable_structured_logging(false)
#define LOG_S_W(tag, msg) Logger::instance().enable_structured_logging(true); Logger::instance().w(tag, msg); Logger::instance().enable_structured_logging(false)
#define LOG_S_I(tag, msg) Logger::instance().enable_structured_logging(true); Logger::instance().i(tag, msg); Logger::instance().enable_structured_logging(false)
#define LOG_S_D(tag, msg) Logger::instance().enable_structured_logging(true); Logger::instance().d(tag, msg); Logger::instance().enable_structured_logging(false)
#define LOG_S_T(tag, msg) Logger::instance().enable_structured_logging(true); Logger::instance().t(tag, msg); Logger::instance().enable_structured_logging(false)

#endif