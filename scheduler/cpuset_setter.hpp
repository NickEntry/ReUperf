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
    
    bool move_to_cpuset_cgroup(int tid, const std::string& cpumask_name) {
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        if (cpumask_name.empty()) {
            return true;
        }
        
        std::string path = "/dev/cpuset/ReUperf_" + cpumask_name;
        bool group_ready = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (skip_groups_.count(path) > 0) {
                return false;
            }

            if (!checked_groups_.count(path)) {
                checked_groups_.insert(path);
                cpus_cache_[path] = FileUtils::read_file(path + "/cpus");
                if (cpus_cache_[path].empty()) {
                    LOG_W("CpusetSetter", path + "/cpus is empty, skipping all moves to this group");
                    skip_groups_.insert(path);
                    return false;
                }
            }
            group_ready = true;
        }
        if (!group_ready) {
            return false;
        }

        // Keep this lightweight; do not over-defend against trusted local configuration.
        // 增加重试机制
        constexpr int kMaxRetries = 3;
        constexpr int kRetryIntervalMs = 10;
        
        for (int retry = 0; retry < kMaxRetries; ++retry) {
            if (FileUtils::write_file(path + "/tasks", std::to_string(tid))) {
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
    
    bool restore_affinity(int tid, const std::vector<int>& cpus) {
        if (tid <= 0 || cpus.empty()) {
            return false;
        }
        if (!CpuMask::set_affinity(tid, cpus)) {
            LOG_W("CpusetSetter", "Failed to restore affinity for tid " + std::to_string(tid));
            return false;
        }
        return true;
    }

    bool restore_cpuset_cgroup(int tid, const std::string& cgroup_path) {
        if (tid <= 0 || cgroup_path.empty()) {
            return false;
        }
        if (!FileUtils::write_cgroup_tasks(cgroup_path, tid)) {
            LOG_W("CpusetSetter", "Failed to restore tid " + std::to_string(tid)
                  + " to cpuset " + cgroup_path);
            return false;
        }
        return true;
    }

    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result,
                           [[maybe_unused]] const std::string& cgroup_base) {
        LOG_D("CpusetSetter", "apply_with_result called: tid=" + std::to_string(tid) 
              + ", matched=" + std::to_string(result.matched)
              + ", affinity_class='" + result.affinity_class + "'"
              + ", cpumask_name='" + result.cpumask_name + "'"
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
            return move_to_cpuset_cgroup(tid, result.cpumask_name);
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
};

#endif