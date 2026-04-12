#ifndef THREAD_CACHE_HPP
#define THREAD_CACHE_HPP

#include <vector>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <optional>
#include <mutex>
#include "../config/config_types.hpp"
#include "../core/thread_matcher.hpp"
#include "../utils/logger.hpp"

struct ThreadCacheEntry {
    int pid;
    int tid;
    std::string thread_name;
    ProcessState actual_state;
    MatchResult result;
    MatchResult applied_result;  // 上次已应用的匹配结果，用于增量调度
    
    std::string cpuset_base;
    std::string cpuctl_base;

    // ✅ 新增：记录实际应用的控制器类型 ("uclamp" 或 "schedtune")
    // 默认初始化为 "uclamp"
    std::string applied_controller = "uclamp"; 
};

struct CacheKeyHash {
    size_t operator()(const std::pair<int, int>& k) const {
        uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(k.first)) << 32)
                     | static_cast<uint64_t>(static_cast<uint32_t>(k.second));
        return std::hash<uint64_t>{}(key);
    }
};

class ThreadCache {
public:
    std::optional<ThreadCacheEntry> lookup(int pid, int tid, const std::string& thread_name,
                                           ProcessState actual_state) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::make_pair(pid, tid);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            if (it->second.thread_name == thread_name &&
                it->second.actual_state == actual_state) {
                return it->second;
            }
            it->second.thread_name = thread_name;
            it->second.actual_state = actual_state;
            return it->second;
        }
        return std::nullopt;
    }

    // ✅ 修改：update 方法增加 controller 参数，默认 uclamp
    void update(int pid, int tid, const std::string& thread_name,
                ProcessState actual_state, const MatchResult& result,
                const std::string& cpuset_base, const std::string& cpuctl_base,
                const std::string& controller = "uclamp") {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::make_pair(pid, tid);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second.thread_name = thread_name;
            it->second.actual_state = actual_state;
            it->second.result = result;
            it->second.cpuset_base = cpuset_base;
            it->second.cpuctl_base = cpuctl_base;
            it->second.applied_controller = controller; // ✅ 记录控制器
        } else {
            ThreadCacheEntry entry{pid, tid, thread_name, actual_state, result, result,
                                   cpuset_base, cpuctl_base, controller};
            cache_[key] = std::move(entry);
        }
    }

    std::optional<MatchResult> get_applied_result(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::make_pair(pid, tid);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second.applied_result;
        }
        return std::nullopt;
    }

    void update_applied_result(int pid, int tid, const MatchResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto key = std::make_pair(pid, tid);
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second.applied_result = result;
        }
    }

    void reset_for_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (it->first.first == pid) {
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        cache_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return cache_.size();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::pair<int, int>, ThreadCacheEntry, CacheKeyHash> cache_;
};

#endif // THREAD_CACHE_HPP