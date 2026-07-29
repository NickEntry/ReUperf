#ifndef THREAD_CACHE_HPP
#define THREAD_CACHE_HPP

#include <cstdint>
#include <optional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../config/config_types.hpp"
#include "../core/thread_matcher.hpp"

struct ThreadBaseline {
    std::vector<int> affinity;
    bool has_affinity = false;
    int scheduler_policy = -1;
    int scheduler_priority = 0;
    int nice = 0;
    bool has_scheduler = false;
    std::string cpuset_path;
    bool has_cpuset_path = false;
    std::string cpuctl_path;
    bool has_cpuctl_path = false;
};

struct ThreadCacheEntry {
    int pid;
    int tid;
    std::string thread_name;
    ProcessState actual_state;
    MatchResult result;
    MatchResult applied_result;
    std::string cpuset_base;
    std::string cpuctl_base;
    std::optional<ThreadBaseline> baseline;
};

struct CacheKeyHash {
    size_t operator()(const std::pair<int, int>& key) const {
        const uint64_t combined = (static_cast<uint64_t>(static_cast<uint32_t>(key.first)) << 32)
            | static_cast<uint32_t>(key.second);
        return std::hash<uint64_t>{}(combined);
    }
};

class ThreadCache {
public:
    std::optional<ThreadCacheEntry> lookup(int pid, int tid, const std::string& thread_name,
                                           ProcessState actual_state) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::make_pair(pid, tid);
        const auto it = cache_.find(key);
        if (it == cache_.end()) {
            return std::nullopt;
        }
        if (it->second.thread_name == thread_name && it->second.actual_state == actual_state) {
            return it->second;
        }
        return std::nullopt;
    }

    void update(int pid, int tid, const std::string& thread_name,
                ProcessState actual_state, const MatchResult& result,
                const std::string& cpuset_base, const std::string& cpuctl_base) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::make_pair(pid, tid);
        const auto it = cache_.find(key);
        if (it != cache_.end()) {
            it->second.thread_name = thread_name;
            it->second.actual_state = actual_state;
            it->second.result = result;
            it->second.cpuset_base = cpuset_base;
            it->second.cpuctl_base = cpuctl_base;
            return;
        }
        cache_.emplace(key, ThreadCacheEntry{
            pid, tid, thread_name, actual_state, result, result, cpuset_base, cpuctl_base, std::nullopt});
    }

    std::optional<MatchResult> get_applied_result(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) {
            return it->second.applied_result;
        }
        return std::nullopt;
    }

    void update_applied_result(int pid, int tid, const MatchResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) {
            it->second.applied_result = result;
        }
    }

    bool has_baseline(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        return it != cache_.end() && it->second.baseline.has_value();
    }

    void set_baseline(int pid, int tid, ThreadBaseline baseline) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end() && !it->second.baseline.has_value()) {
            it->second.baseline = std::move(baseline);
        }
    }

    std::optional<ThreadBaseline> take_baseline(int pid, int tid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it == cache_.end() || !it->second.baseline.has_value()) {
            return std::nullopt;
        }
        auto baseline = std::move(it->second.baseline);
        it->second.baseline.reset();
        return baseline;
    }

    std::vector<std::pair<std::pair<int, int>, ThreadBaseline>> take_all_baselines() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::pair<std::pair<int, int>, ThreadBaseline>> baselines;
        baselines.reserve(cache_.size());
        for (auto& [key, entry] : cache_) {
            if (entry.baseline.has_value()) {
                baselines.emplace_back(key, std::move(*entry.baseline));
                entry.baseline.reset();
            }
        }
        return baselines;
    }

    void reset_for_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
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

#endif
