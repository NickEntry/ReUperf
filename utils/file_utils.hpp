#ifndef FILE_UTILS_HPP
#define FILE_UTILS_HPP

#include <algorithm>
#include <atomic>
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

inline std::atomic<int>& file_cache_ttl_ms() {
    static std::atomic<int> value{100};
    return value;
}

inline std::atomic<int>& cgroup_cache_ttl_ms() {
    static std::atomic<int> value{100};
    return value;
}

inline void set_cache_ttls(int file_ttl_ms, int cgroup_ttl_ms) {
    file_cache_ttl_ms().store(file_ttl_ms, std::memory_order_relaxed);
    cgroup_cache_ttl_ms().store(cgroup_ttl_ms, std::memory_order_relaxed);
}

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

inline bool is_dynamic_kernel_path(const std::string& path) {
    return path.rfind("/proc/", 0) == 0 || path.rfind("/dev/cpuset/", 0) == 0
        || path.rfind("/dev/cpuctl/", 0) == 0;
}

inline void invalidate_file_cache(const std::string& path);

inline bool write_open_file(const std::string& path, const std::string& content, int flags,
                            const char* operation,
                            LogLevel failure_level = LogLevel::ERR) {
    const auto log_failure = [&](const std::string& message) {
        Logger::instance().log_lazy(failure_level, "FileUtils", [&]() { return message; });
    };
    const int fd = open(path.c_str(), flags, 0644);
    if (fd < 0) {
        const int error = errno;
        log_failure(std::string(operation) + " open failed: " + path
                    + " (errno=" + std::to_string(error) + ", "
                    + std::string(strerror(error)) + ")");
        return false;
    }

    size_t written = 0;
    while (written < content.size()) {
        const ssize_t ret = write(fd, content.data() + written, content.size() - written);
        if (ret < 0) {
            const int error = errno;
            if (error == EINTR) {
                continue;
            }
            log_failure(std::string(operation) + " write failed: " + path
                        + " (errno=" + std::to_string(error) + ", "
                        + std::string(strerror(error)) + ")");
            close(fd);
            return false;
        }
        if (ret == 0) {
            log_failure(std::string(operation) + " write returned zero bytes: " + path);
            close(fd);
            return false;
        }
        written += static_cast<size_t>(ret);
    }

    close(fd);
    invalidate_file_cache(path);
    return true;
}

inline bool write_file(const std::string& path, const std::string& content) {
    return write_open_file(path, content, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, "write_file");
}

// Kernel control files such as cgroup tasks, cpus and cpu.shares must already
// exist. Do not create or truncate them: these pseudo-files define write semantics.
inline bool write_kernel_control_file(const std::string& path, const std::string& content,
                                      LogLevel failure_level = LogLevel::ERR) {
    return write_open_file(path, content, O_WRONLY | O_CLOEXEC,
                           "write_kernel_control_file", failure_level);
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

inline void invalidate_file_cache(const std::string& path) {
    std::lock_guard<std::mutex> lock(file_cache_mutex);
    erase_file_cache_entry(path);
}

inline std::string read_file(const std::string& path) {
    const bool cacheable = !is_dynamic_kernel_path(path);
    auto now = std::chrono::steady_clock::now();
    if (cacheable) {
        std::lock_guard<std::mutex> lock(file_cache_mutex);
        auto it = file_cache.find(path);
        if (it != file_cache.end() && std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.timestamp).count()
                    < file_cache_ttl_ms().load(std::memory_order_relaxed)) {
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
    
    if (cacheable) {
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

inline uint64_t parse_start_time_from_stat(const std::string& stat) {
    const size_t closing_paren = stat.rfind(')');
    if (closing_paren == std::string::npos || closing_paren + 2 >= stat.size()) return 0;

    std::istringstream fields(stat.substr(closing_paren + 2));
    std::string field;
    // starttime is field 22; the suffix starts at field 3.
    for (int index = 3; index <= 22 && std::getline(fields, field, ' '); ++index) {
        if (index != 22 || field.empty()) continue;
        errno = 0;
        char* end = nullptr;
        const unsigned long long value = strtoull(field.c_str(), &end, 10);
        return errno == 0 && end != field.c_str() && *end == '\0'
            ? static_cast<uint64_t>(value) : 0;
    }
    return 0;
}

inline uint64_t get_process_start_time(int pid) {
    if (!is_valid_pid(pid)) return 0;
    return parse_start_time_from_stat(read_file("/proc/" + std::to_string(pid) + "/stat"));
}

inline uint64_t get_thread_start_time(int pid, int tid) {
    if (!is_valid_pid(pid) || !is_valid_tid(tid)) return 0;
    return parse_start_time_from_stat(read_file("/proc/" + std::to_string(pid) + "/task/"
        + std::to_string(tid) + "/stat"));
}

inline bool is_thread_in_process(int pid, int tid) {
    if (!is_valid_pid(pid) || !is_valid_tid(tid)) {
        return false;
    }

    const std::string status = read_file("/proc/" + std::to_string(pid) + "/task/"
        + std::to_string(tid) + "/status");
    if (status.empty()) {
        return false;
    }

    std::istringstream iss(status);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.compare(0, 5, "Tgid:") != 0) {
            continue;
        }
        errno = 0;
        char* end = nullptr;
        const long tgid = strtol(line.c_str() + 5, &end, 10);
        return errno == 0 && end != line.c_str() + 5 && is_valid_pid(tgid)
            && tgid == pid;
    }
    return false;
}

inline std::vector<int> list_tids(int pid) {
    std::vector<int> tids;
    // Validate the parsed value before constructing a /proc path.
    if (!is_valid_pid(pid)) {
        return tids;
    }
    std::string task_path = "/proc/" + std::to_string(pid) + "/task";
    
    // opendir is both the existence check and the operation. A separate stat() adds
    // another syscall and cannot eliminate the race with a concurrently exiting process.
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
    // readdir() order is unspecified. Keep the order stable because ScanCursor
    // resumes a partially dispatched PID by TID value.
    std::sort(tids.begin(), tids.end());
    return tids;
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
    static constexpr size_t kMaxCgroupCacheSize = 1000;

    const auto now = std::chrono::steady_clock::now();
    const CgroupCacheKey key{cache_pid, controller};
    {
        std::lock_guard<std::mutex> lock(get_cgroup_cache_mutex());
        auto& cache = get_cgroup_cache();
        const auto it = cache.find(key);
        if (it != cache.end()
            && std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second.timestamp).count()
                < cgroup_cache_ttl_ms().load(std::memory_order_relaxed)) {
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
                if (elapsed >= cgroup_cache_ttl_ms().load(std::memory_order_relaxed)) {
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
        "/proc/" + std::to_string(pid) + "/cgroup",
        static_cast<uint64_t>(static_cast<uint32_t>(pid)), controller);
}

inline std::string get_thread_cgroup_path(int pid, int tid, const std::string& controller) {
    const uint64_t cache_key = (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32)
        | static_cast<uint32_t>(tid);
    return get_cgroup_path_from_file(
        "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/cgroup",
        cache_key, controller);
}

struct ThreadCgroupPaths {
    std::string cpuset;
    std::string cpuctl;
    bool readable = false;
};

inline ThreadCgroupPaths get_thread_cgroup_paths_uncached(int pid, int tid) {
    ThreadCgroupPaths paths;
    if (!is_valid_pid(pid) || !is_valid_tid(tid)) {
        return paths;
    }

    const std::string cgroups = read_file(
        "/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/cgroup");
    if (cgroups.empty()) {
        return paths;
    }
    paths.readable = true;

    std::istringstream iss(cgroups);
    std::string line;
    while (std::getline(iss, line)) {
        const size_t first_colon = line.find(':');
        const size_t second_colon = first_colon == std::string::npos
            ? std::string::npos : line.find(':', first_colon + 1);
        if (second_colon == std::string::npos) {
            continue;
        }

        const std::string path = line.substr(second_colon + 1);
        std::istringstream controllers(line.substr(
            first_colon + 1, second_colon - first_colon - 1));
        std::string controller;
        while (std::getline(controllers, controller, ',')) {
            if (controller == "cpuset") {
                paths.cpuset = path;
            } else if (controller == "cpu" || controller == "cpuctl") {
                paths.cpuctl = path;
            }
        }
    }
    return paths;
}

enum class CgroupState { TOP, FG, BG, OTHER };

inline CgroupState cgroup_path_to_state(const std::string& path) {
    if (path.find("top-app") != std::string::npos) {
        return CgroupState::TOP;
    }
    if (path.find("foreground") != std::string::npos) {
        return CgroupState::FG;
    }
    if (path.find("background") != std::string::npos
        || path.find("system-background") != std::string::npos) {
        return CgroupState::BG;
    }
    return CgroupState::OTHER;
}

inline CgroupState cgroup_paths_to_state(const std::string& cpu_path,
                                         const std::string& cpuset_path) {
    // Prefer a recognized cpu-controller state because ReUperf does not migrate that
    // hierarchy. Some Android devices expose a non-empty but state-neutral cpu path
    // such as "/"; in that case the cpuset controller remains the useful fallback.
    const CgroupState cpu_state = cgroup_path_to_state(cpu_path);
    if (cpu_state != CgroupState::OTHER) {
        return cpu_state;
    }
    return cgroup_path_to_state(cpuset_path);
}

struct CgroupStateInfo {
    CgroupState state = CgroupState::OTHER;
    bool reuperf_owned = false;
};

inline bool is_reuperf_cpuset_path(const std::string& cpuset_path) {
    return cpuset_path.rfind("/top-app/ReUperf_", 0) == 0
        || cpuset_path.rfind("/ReUperf_", 0) == 0;
}

inline CgroupStateInfo cgroup_paths_to_state_info(const std::string& cpu_path,
                                                   const std::string& cpuset_path) {
    const bool reuperf_owned = cpu_path.rfind("/ReUperf/", 0) == 0
        || is_reuperf_cpuset_path(cpuset_path);
    // The cpu controller remains authoritative even when the independent cpuset
    // controller is nested under top-app. Never infer TOP from our own cpuset path.
    const CgroupState cpu_state = cgroup_path_to_state(cpu_path);
    if (cpu_state != CgroupState::OTHER) {
        return {cpu_state, reuperf_owned};
    }
    if (reuperf_owned) {
        return {CgroupState::OTHER, true};
    }
    return {cgroup_path_to_state(cpuset_path), false};
}

inline CgroupStateInfo get_cgroup_state_info(int pid) {
    return cgroup_paths_to_state_info(get_cgroup_path(pid, "cpu"),
                                      get_cgroup_path(pid, "cpuset"));
}

inline CgroupState get_cgroup_state(int pid) {
    return get_cgroup_state_info(pid).state;
}

// Write a single thread (TID) to a cgroup's tasks file.
// Unlike cgroup.procs (which moves the whole process), tasks moves only
// the specified thread, enabling per-thread cgroup grouping.
inline bool write_cgroup_tasks(const std::string& path, int tid) {
    return write_kernel_control_file(path + "/tasks", std::to_string(tid));
}

inline void invalidate_process_caches(int pid) {
    if (!is_valid_pid(pid)) return;
    {
        std::lock_guard<std::mutex> lock(get_cgroup_cache_mutex());
        auto& cache = get_cgroup_cache();
        for (auto it = cache.begin(); it != cache.end();) {
            const uint64_t key = it->first.pid;
            const int cached_pid = static_cast<int>(key >> 32) == 0
                ? static_cast<int>(key) : static_cast<int>(key >> 32);
            if (cached_pid == pid) it = cache.erase(it);
            else ++it;
        }
    }
}

}

#endif
