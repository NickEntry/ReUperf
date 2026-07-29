#ifndef FILE_UTILS_HPP
#define FILE_UTILS_HPP

#include <cstdint>
#include <string>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstring>
#include <vector>
#include <mutex>
#include <climits>
#include <list>
#include <unordered_map>
#include <chrono>
#include "logger.hpp"

enum class ProcessState;

namespace FileUtils {

inline bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

inline bool dir_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool mkdir_recursive(const std::string& path) {
    if (dir_exists(path)) return true;
    
    size_t pos = 0;
    std::string dir;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        dir = path.substr(0, pos);
        if (!dir.empty() && !dir_exists(dir)) {
            if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
                LOG_E("FileUtils", "mkdir failed: " + dir + " (" + std::string(strerror(errno)) + ")");
                return false;
            }
        }
    }
    
    if (!dir_exists(path)) {
        if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
            LOG_E("FileUtils", "mkdir failed: " + path + " (" + std::string(strerror(errno)) + ")");
            return false;
        }
    }
    
    return true;
}

inline bool write_file(const std::string& path, const std::string& content) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        LOG_E("FileUtils", "write_file open failed: " + path + " (" + std::string(strerror(errno)) + ")");
        return false;
    }
    
    ssize_t written = 0;
    ssize_t total = static_cast<ssize_t>(content.size());
    const char* buf = content.c_str();
    
    while (written < total) {
        ssize_t ret = write(fd, buf + written, total - written);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG_E("FileUtils", "write failed: " + path + " (" + std::string(strerror(errno)) + ")");
            close(fd);
            return false;
        }
        if (ret == 0) {
            LOG_E("FileUtils", "write returned zero bytes: " + path);
            close(fd);
            return false;
        }
        written += ret;
    }
    
    close(fd);
    return true;
}

namespace {
    struct FileCacheEntry {
        std::string content;
        std::chrono::steady_clock::time_point timestamp;
    };
    inline std::unordered_map<std::string, FileCacheEntry> file_cache;
    inline std::mutex file_cache_mutex;
    inline std::unordered_map<std::string, std::list<std::string>::iterator> file_cache_order;
    inline std::list<std::string> file_cache_lru;
    static constexpr int kFileCacheTTLMs = 100;
    static constexpr size_t kMaxCacheSize = 1000;

    inline void touch_file_cache_entry(const std::string& path) {
        auto order_it = file_cache_order.find(path);
        if (order_it != file_cache_order.end()) {
            file_cache_lru.erase(order_it->second);
        }
        file_cache_lru.push_back(path);
        file_cache_order[path] = --file_cache_lru.end();
    }

    inline void erase_file_cache_entry(const std::string& path) {
        file_cache.erase(path);
        auto order_it = file_cache_order.find(path);
        if (order_it != file_cache_order.end()) {
            file_cache_lru.erase(order_it->second);
            file_cache_order.erase(order_it);
        }
    }
}

inline std::string read_file(const std::string& path) {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(file_cache_mutex);
        auto it = file_cache.find(path);
        if (it != file_cache.end() && std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.timestamp).count() < kFileCacheTTLMs) {
            touch_file_cache_entry(path);
            return it->second.content;
        }
    }
    
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return "";
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string content = ss.str();
    while (!content.empty() && content.back() == '\n') {
        content.pop_back();
    }
    
    {
        std::lock_guard<std::mutex> lock(file_cache_mutex);
        const bool replacing = file_cache.find(path) != file_cache.end();
        if (!replacing && file_cache.size() >= kMaxCacheSize && !file_cache_lru.empty()) {
            const std::string oldest = file_cache_lru.front();
            erase_file_cache_entry(oldest);
        }
        file_cache[path] = {content, now};
        touch_file_cache_entry(path);
    }
    return content;
}

inline bool is_valid_pid(long pid) {
    return pid > 0 && pid <= INT_MAX;
}

inline bool is_valid_tid(long tid) {
    return tid > 0 && tid <= INT_MAX;
}

inline bool is_all_digits(const char* s) {
    if (!s || !*s) return false;
    for (; *s; ++s) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

inline std::vector<int> list_pids() {
    std::vector<int> pids;
    DIR* dir = opendir("/proc");
    if (!dir) return pids;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (is_all_digits(entry->d_name)) {
            errno = 0;
            char* end = nullptr;
            long pid = strtol(entry->d_name, &end, 10);
            // Validate the parsed value before narrowing it to int.
            if (errno == 0 && end != entry->d_name && *end == '\0' && is_valid_pid(pid)) {
                pids.push_back(static_cast<int>(pid));
            }
        }
    }
    closedir(dir);
    return pids;
}

inline std::vector<int> list_tids(int pid) {
    std::vector<int> tids;
    // Validate the parsed value before constructing a /proc path.
    if (!is_valid_pid(pid)) {
        return tids;
    }
    std::string task_path = "/proc/" + std::to_string(pid) + "/task";
    
    if (!dir_exists(task_path)) {
        return tids;
    }
    
    DIR* dir = opendir(task_path.c_str());
    if (!dir) return tids;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (is_all_digits(entry->d_name)) {
            errno = 0;
            char* end = nullptr;
            long tid = strtol(entry->d_name, &end, 10);
            // Validate the parsed value before narrowing it to int.
            if (errno == 0 && end != entry->d_name && *end == '\0' && is_valid_tid(tid)) {
                tids.push_back(static_cast<int>(tid));
            }
        }
    }
    closedir(dir);
    return tids;
}

inline std::string get_process_name(int pid) {
    std::string cmdline = read_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline.empty()) {
        size_t pos = cmdline.find('\0');
        if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
        pos = cmdline.rfind('/');
        if (pos != std::string::npos) cmdline = cmdline.substr(pos + 1);
        return cmdline;
    }
    std::string comm = read_file("/proc/" + std::to_string(pid) + "/comm");
    // Handle case where process disappeared during read
    if (comm.empty()) {
        return "[dead]";
    }
    return comm;
}

inline std::string get_process_comm(int pid) {
    std::string comm = read_file("/proc/" + std::to_string(pid) + "/comm");
    if (comm.empty()) {
        return "[dead]";
    }
    return comm;
}

inline std::string get_process_cmdline(int pid) {
    std::string cmdline = read_file("/proc/" + std::to_string(pid) + "/cmdline");
    if (!cmdline.empty()) {
        size_t pos = cmdline.find('\0');
        if (pos != std::string::npos) cmdline = cmdline.substr(0, pos);
    }
    return cmdline;
}

inline std::string get_thread_comm(int pid, int tid) {
    std::string comm = read_file("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/comm");
    if (comm.empty()) {
        return "[dead]";
    }
    return comm;
}

inline std::string parse_process_name_from_status(const std::string& status_content) {
    if (status_content.empty()) {
        return "[dead]";
    }
    size_t start = status_content.find("Name:");
    if (start == std::string::npos) {
        return "[dead]";
    }
    start += 5;
    while (start < status_content.size() && (status_content[start] == ' ' || status_content[start] == '\t')) {
        start++;
    }
    size_t end = status_content.find('\n', start);
    if (end == std::string::npos) {
        return "[dead]";
    }
    return status_content.substr(start, end - start);
}

inline std::string get_process_name_from_status(int pid) {
    return parse_process_name_from_status(read_file("/proc/" + std::to_string(pid) + "/status"));
}

inline std::string get_thread_name_from_sched(int pid, int tid) {
    std::string sched_path = "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/sched";
    std::string content = read_file(sched_path);
    if (content.empty()) {
        return "[dead]";
    }
    
    size_t pos = content.find('\n');
    if (pos == std::string::npos || pos < 20) {
        return "[dead]";
    }
    
    std::string first_line = content.substr(0, pos);
    
    size_t start = first_line.find('(');
    size_t end = first_line.rfind(')');
    
    if (start == std::string::npos || end == std::string::npos || end <= start + 1) {
        return "[dead]";
    }
    
    std::string name = first_line.substr(start + 1, end - start - 1);
    
    size_t comma_pos = name.find(',');
    if (comma_pos != std::string::npos) {
        while (comma_pos > 0 && (name[comma_pos - 1] == ' ' || name[comma_pos - 1] == '\t')) {
            comma_pos--;
        }
        name = name.substr(0, comma_pos);
    }
    
    return name;
}

inline std::string get_thread_name(int pid, int tid) {
    return get_thread_comm(pid, tid);
}

struct CgroupCacheKey {
    uint64_t pid;
    std::string controller;

    bool operator==(const CgroupCacheKey& other) const {
        return pid == other.pid && controller == other.controller;
    }
};

struct CgroupCacheKeyHash {
    size_t operator()(const CgroupCacheKey& key) const {
        return std::hash<uint64_t>{}(key.pid) ^ (std::hash<std::string>{}(key.controller) << 1);
    }
};

struct CgroupCacheEntry {
    std::string path;
    std::chrono::steady_clock::time_point timestamp;
};

inline std::unordered_map<CgroupCacheKey, CgroupCacheEntry, CgroupCacheKeyHash>& get_cgroup_cache() {
    static std::unordered_map<CgroupCacheKey, CgroupCacheEntry, CgroupCacheKeyHash> cache;
    return cache;
}

inline std::mutex& get_cgroup_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::string get_cgroup_path_from_file(const std::string& cgroup_file,
                                              uint64_t cache_pid,
                                              const std::string& controller) {
    static constexpr int64_t kCacheTTLMs = 100;
    static constexpr size_t kMaxCgroupCacheSize = 1000;

    const auto now = std::chrono::steady_clock::now();
    const CgroupCacheKey key{cache_pid, controller};
    {
        std::lock_guard<std::mutex> lock(get_cgroup_cache_mutex());
        auto& cache = get_cgroup_cache();
        const auto it = cache.find(key);
        if (it != cache.end()
            && std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.timestamp).count()
                < kCacheTTLMs) {
            return it->second.path;
        }
    }

    std::string result;
    const std::string cgroups = read_file(cgroup_file);
    std::istringstream iss(cgroups);
    std::string line;
    while (std::getline(iss, line)) {
        const size_t first_colon = line.find(':');
        const size_t second_colon = first_colon == std::string::npos
            ? std::string::npos : line.find(':', first_colon + 1);
        if (second_colon == std::string::npos) {
            continue;
        }

        const std::string controllers = line.substr(first_colon + 1, second_colon - first_colon - 1);
        std::istringstream controller_list(controllers);
        std::string listed_controller;
        while (std::getline(controller_list, listed_controller, ',')) {
            if (listed_controller == controller) {
                // Android targets expose cgroup paths with v1-compatible semantics here.
                // Do not over-generalize this parser for non-Android cgroup layouts.
                result = line.substr(second_colon + 1);
                break;
            }
        }
        if (!result.empty()) {
            break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(get_cgroup_cache_mutex());
        auto& cache = get_cgroup_cache();
        if (cache.size() >= kMaxCgroupCacheSize) {
            for (auto it = cache.begin(); it != cache.end();) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second.timestamp).count();
                if (elapsed >= kCacheTTLMs) {
                    it = cache.erase(it);
                } else {
                    ++it;
                }
            }
            if (cache.size() >= kMaxCgroupCacheSize) {
                cache.erase(cache.begin());
            }
        }
        cache[key] = {result, now};
    }
    return result;
}

inline std::string get_cgroup_path(int pid, const std::string& controller) {
    return get_cgroup_path_from_file(
        "/proc/" + std::to_string(pid) + "/cgroup", pid, controller);
}

inline std::string get_thread_cgroup_path(int pid, int tid, const std::string& controller) {
    const uint64_t cache_key = (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32)
        | static_cast<uint32_t>(tid);
    return get_cgroup_path_from_file(
        "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/cgroup",
        cache_key, controller);
}

// Security: Get process UID for ownership verification
// Returns -1 on error, otherwise the UID
inline int get_process_uid(int pid) {
    if (!is_valid_pid(pid)) return -1;
    
    std::string status = read_file("/proc/" + std::to_string(pid) + "/status");
    if (status.empty()) return -1;
    
    // Parse "Uid:" line
    std::istringstream iss(status);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.compare(0, 5, "Uid:") == 0) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                try {
                    return std::stoi(line.substr(pos + 1));
                } catch (...) {
                    return -1;
                }
            }
        }
    }
    return -1;
}

// Security: Check if process belongs to root user (system process)
inline bool is_system_process(int pid) {
    return get_process_uid(pid) == 0;
}

inline bool is_in_cgroup(int pid, const std::string& cgroup_name) {
    // Use cached version for better performance
    std::string path = get_cgroup_path(pid, "cpuset");
    return path.find(cgroup_name) != std::string::npos;
}

enum class CgroupState { TOP, FG, BG, OTHER };

inline ProcessState cgroup_state_to_process_state(CgroupState state) {
    switch (state) {
        case CgroupState::TOP: return ProcessState::TOP;
        case CgroupState::FG: return ProcessState::FG;
        case CgroupState::BG:
        case CgroupState::OTHER: return ProcessState::BG;
    }
    return ProcessState::BG;
}

inline CgroupState get_cgroup_state(int pid) {
    std::string cgroup = get_cgroup_path(pid, "cpuset");
    if (cgroup.empty()) {
        return CgroupState::OTHER;
    }
    if (cgroup.find("top-app") != std::string::npos) {
        return CgroupState::TOP;
    } else if (cgroup.find("foreground") != std::string::npos) {
        return CgroupState::FG;
    } else if (cgroup.find("background") != std::string::npos ||
               cgroup.find("system-background") != std::string::npos) {
        return CgroupState::BG;
    }
    return CgroupState::OTHER;
}

inline std::vector<int> read_cgroup_procs(const std::string& path) {
    std::vector<int> pids;
    std::string content = read_file(path + "/cgroup.procs");
    std::istringstream iss(content);
    std::string line;
    while (std::getline(iss, line)) {
        errno = 0;
        char* end = nullptr;
        long pid = strtol(line.c_str(), &end, 10);
        if (errno == 0 && end != line.c_str() && *end == '\0' && pid > 0 && pid <= INT_MAX) {
            pids.push_back(static_cast<int>(pid));
        }
    }
    return pids;
}

inline bool write_cgroup_procs(const std::string& path, int tid) {
    return write_file(path + "/cgroup.procs", std::to_string(tid));
}

// Write a single thread (TID) to a cgroup's tasks file.
// Unlike cgroup.procs (which moves the whole process), tasks moves only
// the specified thread, enabling per-thread cgroup grouping.
inline bool write_cgroup_tasks(const std::string& path, int tid) {
    return write_file(path + "/tasks", std::to_string(tid));
}

// 进程信息缓存
struct ProcInfoCache {
    std::string name;
    std::string cmdline;
    std::chrono::steady_clock::time_point timestamp;
};

static constexpr int64_t kProcInfoCacheTTLMs = 500;
static constexpr size_t kMaxProcInfoCacheSize = 500;

inline std::unordered_map<int, ProcInfoCache>& get_proc_info_cache() {
    static std::unordered_map<int, ProcInfoCache> cache;
    return cache;
}

inline std::mutex& get_proc_info_cache_mutex() {
    static std::mutex mutex;
    return mutex;
}

// 获取进程信息（带缓存）
inline std::pair<std::string, std::string> get_process_info_cached(int pid) {
    std::lock_guard<std::mutex> lock(get_proc_info_cache_mutex());
    auto& cache = get_proc_info_cache();
    auto now = std::chrono::steady_clock::now();
    
    auto it = cache.find(pid);
    if (it != cache.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.timestamp).count();
        if (elapsed < kProcInfoCacheTTLMs) {
            return {it->second.name, it->second.cmdline};
        }
    }
    
    // 缓存未命中，读取文件
    std::string name = get_process_name_from_status(pid);
    std::string cmdline = get_process_cmdline(pid);
    
    // 限制缓存大小
    if (cache.size() >= kMaxProcInfoCacheSize) {
        // 清理过期的缓存项
        for (auto iter = cache.begin(); iter != cache.end();) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - iter->second.timestamp).count();
            if (elapsed >= kProcInfoCacheTTLMs) {
                iter = cache.erase(iter);
            } else {
                ++iter;
            }
        }
        // 如果仍然满了，随机删除一些
        if (cache.size() >= kMaxProcInfoCacheSize) {
            size_t to_remove = cache.size() - kMaxProcInfoCacheSize / 2;
            for (size_t i = 0; i < to_remove && !cache.empty(); ++i) {
                cache.erase(cache.begin());
            }
        }
    }
    
    cache[pid] = {name, cmdline, now};
    return {name, cmdline};
}

}

#endif
