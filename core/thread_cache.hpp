#ifndef THREAD_CACHE_HPP
#define THREAD_CACHE_HPP

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
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

struct StoredThreadBaseline {
    int pid;
    int tid;
    uint64_t process_start_time;
    uint64_t thread_start_time;
    ThreadBaseline baseline;
};

struct ManagedComponents {
    bool affinity = false;
    bool priority = false;
    bool cpuctl = false;
};

struct ThreadCacheEntry {
    int pid;
    int tid;
    uint64_t process_start_time;
    uint64_t thread_start_time;
    std::string thread_name;
    std::string cmdline;
    ProcessState actual_state;
    MatchResult result;
    std::optional<MatchResult> applied_result;
    ManagedComponents managed_components;
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
    std::optional<ThreadCacheEntry> lookup(int pid, int tid, uint64_t process_start_time,
                                           uint64_t thread_start_time,
                                           const std::string& thread_name, const std::string& cmdline,
                                           ProcessState actual_state) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it == cache_.end() || it->second.process_start_time != process_start_time
            || it->second.thread_start_time != thread_start_time) {
            return std::nullopt;
        }
        if (it->second.thread_name == thread_name && it->second.cmdline == cmdline
            && it->second.actual_state == actual_state) {
            return it->second;
        }
        return std::nullopt;
    }

    void update(int pid, int tid, uint64_t process_start_time, uint64_t thread_start_time,
                const std::string& thread_name,
                const std::string& cmdline, ProcessState actual_state, const MatchResult& result,
                const std::string& cpuset_base, const std::string& cpuctl_base) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto key = std::make_pair(pid, tid);
        const auto it = cache_.find(key);
        if (it != cache_.end() && it->second.process_start_time == process_start_time
            && it->second.thread_start_time == thread_start_time) {
            it->second.thread_name = thread_name;
            it->second.cmdline = cmdline;
            it->second.actual_state = actual_state;
            it->second.result = result;
            it->second.cpuset_base = cpuset_base;
            it->second.cpuctl_base = cpuctl_base;
            return;
        }
        cache_[key] = ThreadCacheEntry{pid, tid, process_start_time, thread_start_time,
            thread_name, cmdline,
            actual_state, result, std::nullopt, {}, cpuset_base, cpuctl_base, std::nullopt};
    }

    std::optional<MatchResult> get_applied_result(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        return it == cache_.end() ? std::nullopt : it->second.applied_result;
    }

    void update_applied_result(int pid, int tid, const MatchResult& result) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) it->second.applied_result = result;
    }

    void clear_applied_result(int pid, int tid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) it->second.applied_result.reset();
    }

    ManagedComponents get_managed_components(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        return it == cache_.end() ? ManagedComponents{} : it->second.managed_components;
    }

    void set_managed_components(int pid, int tid, const ManagedComponents& components) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) it->second.managed_components = components;
    }

    void clear_managed_components(int pid, int tid) {
        set_managed_components(pid, tid, {});
    }

    void set_baseline(int pid, int tid, ThreadBaseline baseline) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it == cache_.end()) return;
        if (!it->second.baseline) {
            it->second.baseline = std::move(baseline);
            return;
        }
        auto& existing = *it->second.baseline;
        if (!existing.has_affinity && baseline.has_affinity) {
            existing.affinity = std::move(baseline.affinity);
            existing.has_affinity = true;
        }
        if (!existing.has_scheduler && baseline.has_scheduler) {
            existing.scheduler_policy = baseline.scheduler_policy;
            existing.scheduler_priority = baseline.scheduler_priority;
            existing.nice = baseline.nice;
            existing.has_scheduler = true;
        }
        if (!existing.has_cpuset_path && baseline.has_cpuset_path) {
            existing.cpuset_path = std::move(baseline.cpuset_path);
            existing.has_cpuset_path = true;
        }
        if (!existing.has_cpuctl_path && baseline.has_cpuctl_path) {
            existing.cpuctl_path = std::move(baseline.cpuctl_path);
            existing.has_cpuctl_path = true;
        }
    }

    std::optional<ThreadBaseline> get_baseline(int pid, int tid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it == cache_.end()) return std::nullopt;
        return it->second.baseline;
    }

    void clear_baseline(int pid, int tid) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end()) it->second.baseline.reset();
    }

    std::set<int> get_pids_with_baselines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::set<int> pids;
        for (const auto& [key, entry] : cache_) {
            if (entry.baseline) {
                pids.insert(entry.pid);
            }
        }
        return pids;
    }

    bool has_baseline_for_pid(int pid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [key, entry] : cache_) {
            (void)key;
            if (entry.pid == pid && entry.baseline) return true;
        }
        return false;
    }

    std::vector<StoredThreadBaseline> get_all_baselines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<StoredThreadBaseline> baselines;
        baselines.reserve(cache_.size());
        for (const auto& [key, entry] : cache_) {
            if (entry.baseline) {
                baselines.push_back({entry.pid, entry.tid, entry.process_start_time,
                    entry.thread_start_time, *entry.baseline});
            }
        }
        return baselines;
    }

    std::map<std::pair<int, int>, std::pair<uint64_t, uint64_t>> identity_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::map<std::pair<int, int>, std::pair<uint64_t, uint64_t>> identities;
        for (const auto& [key, entry] : cache_) {
            identities.emplace(key, std::make_pair(entry.process_start_time,
                                                    entry.thread_start_time));
        }
        return identities;
    }

    void retain_live_threads(
        const std::map<int, uint64_t>& live_processes,
        const std::map<std::pair<int, int>, uint64_t>& live_threads,
        const std::map<std::pair<int, int>, std::pair<uint64_t, uint64_t>>& scan_start_identities) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            const auto original = scan_start_identities.find(it->first);
            if (original == scan_start_identities.end()
                || original->second.first != it->second.process_start_time
                || original->second.second != it->second.thread_start_time) {
                ++it;
                continue;
            }
            const auto process = live_processes.find(it->second.pid);
            if (process == live_processes.end() || process->second != it->second.process_start_time) {
                // The owning process is confirmed dead or recycled, so restoration is unsafe.
                it = cache_.erase(it);
            } else {
                const auto live_thread = live_threads.find(it->first);
                if (live_thread != live_threads.end()
                    && live_thread->second != it->second.thread_start_time) {
                    // TID was recycled inside the same process; never restore old state to it.
                    it = cache_.erase(it);
                } else if (live_thread == live_threads.end() && !it->second.baseline) {
                    it = cache_.erase(it);
                } else {
                    // A transient /proc miss must not discard the only restoration baseline
                    // for a still-live process. The next dispatch revalidates thread identity.
                    ++it;
                }
            }
        }
    }

    void clear_scheduling_state_preserving_baselines() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (!it->second.baseline) {
                it = cache_.erase(it);
                continue;
            }
            it->second.thread_name.clear();
            it->second.cmdline.clear();
            it->second.applied_result.reset();
            it->second.managed_components = {};
            ++it;
        }
    }

    void erase_thread_if_identity(int pid, int tid, uint64_t process_start_time,
                                  uint64_t thread_start_time) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = cache_.find(std::make_pair(pid, tid));
        if (it != cache_.end() && it->second.process_start_time == process_start_time
            && it->second.thread_start_time == thread_start_time) {
            cache_.erase(it);
        }
    }

    void reset_for_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (it->first.first == pid) it = cache_.erase(it);
            else ++it;
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
