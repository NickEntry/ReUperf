#ifndef CONFIG_TYPES_HPP
#define CONFIG_TYPES_HPP

#include <string>
#include <vector>
#include <map>
#include <optional>
#include "../utils/logger.hpp"

enum class ProcessState {
    BG,
    FG,
    TOP
};

struct LogConfig {
    std::string level = "info";
    std::string output = "/data/adb/ReUperf/ReUperf.log";
};

struct ThreadRule {
    std::string keyword;
    std::string affinity_class;
    std::string prio_class;
    std::optional<int> uclamp_max;
    std::optional<int> cpu_share;
    bool enable_limit = false;
};

struct ProcessRule {
    std::string name;
    std::string regex_str;       // 匹配 cmdline
    std::string comm_regex_str;  // 匹配 comm (线程名)
    bool pinned = false;
    bool topfore = false;
    std::vector<ThreadRule> thread_rules;
};

struct AffinityScene {
    std::string bg;
    std::string fg;
    std::string top;
};

struct PrioScene {
    int bg = 0;
    int fg = 0;
    int top = 0;
};

struct SchedConfig {
    bool enable = true;
    int refresh_interval_ms = 2000;
    int highspeed_sched_ms = 300;
    int top_scan_budget_us = 4000;
    int full_scan_budget_us = 12000;
    int scan_batch_size = 32;
    int scan_batch_yield_us = 0;
    bool case_insensitive = false;
    LogConfig log;
    
    std::map<std::string, std::vector<int>> cpumask;
    std::map<std::string, AffinityScene> affinity;
    std::map<std::string, PrioScene> prio;
    std::vector<ProcessRule> rules;
};

struct Config {
    std::string meta_name;
    std::string meta_author;
    SchedConfig sched;
    std::string launcher_package;
};

inline LogLevel parse_log_level(const std::string& level) {
    if (level == "err") return LogLevel::ERR;
    if (level == "warn") return LogLevel::WARN;
    if (level == "info") return LogLevel::INFO;
    if (level == "debug") return LogLevel::DEBUG;
    if (level == "trace") return LogLevel::TRACE;
    return LogLevel::INFO;
}

#endif