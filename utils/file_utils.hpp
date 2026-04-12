#ifndef FILE_UTILS_HPP
#define FILE_UTILS_HPP

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
            LOG_E("FileUtils", "write failed: " + path + " (" + std::string(strerror(errno)) + ")");
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
    std::unordered_map<std::string, FileCacheEntry> file_cache;
    std::mutex file_cache_mutex;
    std::unordered_map<std::string, std::list<std::string>::iterator> file_cache_order;
    std::list<std::string> file_cache_lru;
    static constexpr int kFileCacheTTLMs = 100;
    static constexpr size_t kMaxCacheSize = 1000;
}

inline std::string read_file(const std::string& path) {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(file_cache_mutex);
        auto it = file_cache.find(path);
        if (it != file_cache.end() && std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.timestamp).count() < kFileCacheTTLMs) {
            auto lru_it = file_cache_order.find(path);
            if (lru_it != file_cache_order.end()) {
                file_cache_lru.erase(lru_it->second);
                file_cache_lru.push_back(path);
                file_cache_order[path] = --file_cache_lru.end();
            }
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
        if (file_cache.size() >= kMaxCacheSize && !file_cache_lru.empty()) {
            auto oldest = file_cache_lru.front();
            file_cache_lru.pop_front();
            file_cache.erase(oldest);
            file_cache_order.erase(oldest);
        }
        file_cache[path] = {content, now};
        file_cache_lru.push_back(path);
        file_cache_order[path] = --file_cache_lru.end();
    }
    return content;
}

// Android-specific PID range: 1 ~ 32768 (actual limit is task_max, typically 32768)
static constexpr int kMaxPid = 32768;
static constexpr int kMaxTid = 117616;

inline bool is_valid_pid(int pid) {
    return pid > 0 && pid <= kMaxPid;
}

inline bool is_valid_tid(int tid) {
    return tid > 0 && tid <= kMaxTid;
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
            // Security: validate PID range (Android-specific: 1~32768)
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
    // Security: validate PID range (Android-specific)
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
            // Security: validate TID range (Android-specific: 1~117616)
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

inline std::string get_cgroup_path(int pid, const std::string& controller) {
    struct CgroupCacheKey {
        int pid;
        std::string controller;
        
        bool operator==(const CgroupCacheKey& other) const {
            return pid == other.pid && controller == other.controller;
        }
    };
    
    struct CgroupCacheKeyHash {
        size_t operator()(const CgroupCacheKey& k) const {
            return std::hash<int>{}(k.pid) ^ (std::hash<std::string>{}(k.controller) << 1);
        }
    };
    
    static std::unordered_map<CgroupCacheKey, std::pair<std::string, int64_t>, CgroupCacheKeyHash> cache;
    static std::mutex cache_mutex;
    static constexpr int64_t kCacheTTLMs = 100;
    
    auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    
    CgroupCacheKey key{pid, controller};
    
    {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end() && (now - it->second.second) < kCacheTTLMs) {
            return it->second.first;
        }
    }
    
    std::string cgroups = read_file("/proc/" + std::to_string(pid) + "/cgroup");
    std::istringstream iss(cgroups);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find(controller) != std::string::npos) {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string result = line.substr(pos + 1);
                std::lock_guard<std::mutex> lock(cache_mutex);
                cache[key] = {result, now};
                return result;
            }
        }
    }
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[key] = {"", now};
    return "";
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

}

#endif
