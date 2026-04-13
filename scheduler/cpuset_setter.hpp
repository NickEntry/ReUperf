#ifndef CPUSET_SETTER_HPP
#define CPUSET_SETTER_HPP

#include <string>
#include <vector>
#include <set>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <fcntl.h>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/cpu_mask.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

class CpusetSetter {
public:
    CpusetSetter(ThreadMatcher& matcher)
        : matcher_(matcher) {}

    std::vector<int> get_cpus_for_affinity(const std::string& affinity_class, ProcessState state) {
        return matcher_.get_cpus_for_affinity(affinity_class, state);
    }

    bool set_affinity(int tid, const std::string& affinity_class, ProcessState state) {
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        auto cpus = matcher_.get_cpus_for_affinity(affinity_class, state);
        
        if (cpus.empty()) {
            return true;
        }
        
        if (!CpuMask::set_affinity(tid, cpus)) {
            LOG_W("CpusetSetter", "Failed to set affinity for tid " + std::to_string(tid));
            return false;
        }
        
        LOG_T("CpusetSetter", "Set affinity for tid " + std::to_string(tid) 
              + " to " + CpuMask::to_string(cpus));
        return true;
    }
    
    bool move_to_cpuset_cgroup(int tid, const std::string& affinity_class, 
                               ProcessState state, [[maybe_unused]] const std::string& cgroup_base) {
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        auto cpus = matcher_.get_cpus_for_affinity(affinity_class, state);
        
        if (cpus.empty()) {
            return true;
        }
        
        std::string cpumask_name;
        for (const auto& kv : matcher_.sched_config().cpumask) {
            if (kv.second.size() == cpus.size() &&
                std::equal(kv.second.begin(), kv.second.end(), cpus.begin())) {
                cpumask_name = kv.first;
                break;
            }
        }
        
        if (cpumask_name.empty()) {
            return true;
        }
        
        std::string path = "/dev/cpuset/ReUperf_" + cpumask_name;
        
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (skip_groups_.count(path) > 0) {
            return false;
        }
        
        if (!checked_groups_.count(path)) {
            checked_groups_.insert(path);
            
            // 缓存 cpus 文件内容，避免重复读取
            if (cpus_cache_.find(path) == cpus_cache_.end()) {
                std::string cpus_val = FileUtils::read_file(path + "/cpus");
                cpus_cache_[path] = cpus_val;
            }
            
            if (cpus_cache_[path].empty()) {
                LOG_W("CpusetSetter", path + "/cpus is empty, skipping all moves to this group");
                skip_groups_.insert(path);
                return false;
            }
        }
        
        // 增加重试机制
        constexpr int kMaxRetries = 3;
        constexpr int kRetryIntervalMs = 10;
        
        for (int retry = 0; retry < kMaxRetries; ++retry) {
            if (FileUtils::write_file(path + "/cgroup.procs", std::to_string(tid))) {
                LOG_T("CpusetSetter", "Moved tid " + std::to_string(tid) + " to cpuset " + path);
                return true;
            }
            
            if (retry < kMaxRetries - 1) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kRetryIntervalMs));
            }
        }
        
        LOG_W("CpusetSetter", "Failed to move tid " + std::to_string(tid) + " to " + path + " after " + std::to_string(kMaxRetries) + " retries");
        return false;
    }
    
    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result,
                           const std::string& cgroup_base) {
        LOG_D("CpusetSetter", "apply_with_result called: tid=" + std::to_string(tid) 
              + ", matched=" + std::to_string(result.matched)
              + ", affinity_class='" + result.affinity_class + "'"
              + ", effective_state=" + std::to_string((int)result.effective_state));
        
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        if (!result.matched) {
            return true;
        }
        
        if (!result.affinity_class.empty() && result.affinity_class != "auto") {
            if (!set_affinity(tid, result.affinity_class, result.effective_state)) {
                return false;
            }
            return move_to_cpuset_cgroup(tid, result.affinity_class, 
                                        result.effective_state, cgroup_base);
        }
        
        LOG_D("CpusetSetter", "Skipped cpuset for tid " + std::to_string(tid) 
              + " (affinity_class is empty or auto)");
        return true;
    }

private:
    ThreadMatcher& matcher_;
    mutable std::mutex mutex_;
    std::set<std::string> checked_groups_;  // 已检查过 cpus 的组
    std::set<std::string> skip_groups_;     // cpus 为空需跳过的组
    std::unordered_map<std::string, std::string> cpus_cache_;  // 缓存 cpus 文件内容
    
    // 静默写入 cgroup.procs（不记日志，用于非关键的父组先行写入）
    static bool write_cgroup_procs_silent(const std::string& path, int tid) {
        int fd = open(path.c_str(), O_WRONLY);
        if (fd < 0) {
            LOG_D("CpusetSetter", "write_cgroup_procs_silent: cannot open " + path);
            return false;
        }
        std::string val = std::to_string(tid);
        ssize_t ret = write(fd, val.c_str(), val.size());
        int err = errno;
        close(fd);
        if (ret < 0 || ret != static_cast<ssize_t>(val.size())) {
            LOG_D("CpusetSetter", "write_cgroup_procs_silent failed: " + std::string(strerror(err)));
            return false;
        }
        return true;
    }
};

#endif
