#ifndef CPUSET_SETTER_HPP
#define CPUSET_SETTER_HPP

#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/cpu_mask.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"

struct AffinityTakeoverResult {
    bool pre_affinity_succeeded = false;
    bool initial_cpuset_succeeded = false;
    bool recovery_pre_parent_affinity_succeeded = false;
    bool parent_cpuset_succeeded = false;
    bool recovery_pre_target_affinity_succeeded = false;
    bool cpuset_succeeded = false;
    bool post_affinity_succeeded = false;
    bool cpuset_verified = false;
    bool affinity_verified = false;
    int attempts = 0;

    bool success() const {
        return cpuset_verified && affinity_verified;
    }
};

class CpusetSetter {
public:
    CpusetSetter(ThreadMatcher& matcher, const TimingConfig& timing = {})
        : matcher_(matcher), timing_(timing) {}

    std::vector<int> get_cpus_for_affinity(const std::string& affinity_class,
                                           ProcessState state) const {
        return matcher_.get_cpus_for_affinity(affinity_class, state);
    }

    bool set_affinity(int tid, const std::string& affinity_class, ProcessState state) {
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        const auto cpus = matcher_.get_cpus_for_affinity(affinity_class, state);
        if (cpus.empty()) return true;
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
        if (cpumask_name.empty()) return true;

        const std::string path = "/dev/cpuset/top-app/ReUperf_" + cpumask_name;
        if (!is_group_ready(path)) return false;
        for (int retry = 0; retry < timing_.cpuset_retry_count; ++retry) {
            if (FileUtils::write_kernel_control_file(path + "/tasks", std::to_string(tid))) {
                LOG_T("CpusetSetter", "Moved tid " + std::to_string(tid) + " to cpuset " + path);
                return true;
            }
            if (retry < timing_.cpuset_retry_count - 1 && timing_.cpuset_retry_interval_ms > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(timing_.cpuset_retry_interval_ms));
            }
        }
        invalidate_group(path);
        LOG_W("CpusetSetter", "Failed to move tid " + std::to_string(tid) + " to " + path
              + " after " + std::to_string(timing_.cpuset_retry_count) + " attempts");
        return false;
    }

    bool move_to_top_app_parent(int tid) {
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        static const std::string parent = "/dev/cpuset/top-app";
        if (!FileUtils::write_cgroup_tasks(parent, tid)) {
            LOG_W("CpusetSetter", "Failed to move tid " + std::to_string(tid)
                  + " to top-app parent cpuset");
            return false;
        }
        LOG_T("CpusetSetter", "Moved tid " + std::to_string(tid)
              + " to top-app parent cpuset");
        return true;
    }

    bool restore_affinity(int tid, const std::vector<int>& cpus) {
        if (tid <= 0 || cpus.empty()) return false;
        if (!CpuMask::set_affinity(tid, cpus)) {
            LOG_W("CpusetSetter", "Failed to restore affinity for tid " + std::to_string(tid));
            return false;
        }
        return true;
    }

    bool restore_cpuset_cgroup(int tid, const std::string& cgroup_path) {
        if (tid <= 0 || cgroup_path.empty()) return false;
        if (!FileUtils::write_cgroup_tasks(cgroup_path, tid)) {
            LOG_W("CpusetSetter", "Failed to restore tid " + std::to_string(tid)
                  + " to cpuset " + cgroup_path);
            return false;
        }
        return true;
    }

    template <typename SetAffinityOperation, typename MoveTargetOperation,
              typename MoveParentOperation, typename VerifyCpusetOperation,
              typename VerifyAffinityOperation>
    static AffinityTakeoverResult run_takeover_sequence(
        int pid, int tid, const std::string& cpumask_name, const std::vector<int>& expected_cpus,
        SetAffinityOperation&& set_affinity_operation,
        MoveTargetOperation&& move_target_operation,
        MoveParentOperation&& move_parent_operation,
        VerifyCpusetOperation&& verify_cpuset_operation,
        VerifyAffinityOperation&& verify_affinity_operation,
        int max_attempts = 2) {
        AffinityTakeoverResult result;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            ++result.attempts;
            // Every state-changing call is independent. In particular, an affinity
            // failure caused by a disjoint current cpuset never suppresses migration.
            result.pre_affinity_succeeded = set_affinity_operation(tid, expected_cpus);
            result.initial_cpuset_succeeded = move_target_operation(tid, cpumask_name);
            result.cpuset_succeeded = result.initial_cpuset_succeeded;

            if (!result.initial_cpuset_succeeded) {
                // Enhanced recovery: first enter the known top-app parent, then retry the
                // target child. Affinity is attempted around both writes, without gating.
                result.recovery_pre_parent_affinity_succeeded =
                    set_affinity_operation(tid, expected_cpus);
                result.parent_cpuset_succeeded = move_parent_operation(tid);
                result.recovery_pre_target_affinity_succeeded =
                    set_affinity_operation(tid, expected_cpus);
                result.cpuset_succeeded = move_target_operation(tid, cpumask_name);
            }

            result.post_affinity_succeeded = set_affinity_operation(tid, expected_cpus);
            result.cpuset_verified = verify_cpuset_operation(pid, tid, cpumask_name);
            result.affinity_verified = verify_affinity_operation(tid, expected_cpus);
            if (result.success()) break;
        }
        return result;
    }

    AffinityTakeoverResult apply_with_result(int pid, int tid, const MatchResult& result,
                                             [[maybe_unused]] const std::string& cgroup_base) {
        AffinityTakeoverResult applied;
        if (tid <= 0) {
            LOG_W("CpusetSetter", "Invalid tid: " + std::to_string(tid));
            return applied;
        }
        if (!result.matched || result.affinity_class.empty()
            || result.affinity_class == "auto" || result.cpumask_name.empty()) {
            applied.cpuset_verified = true;
            applied.affinity_verified = true;
            return applied;
        }

        const auto expected_cpus = get_cpus_for_affinity(
            result.affinity_class, result.effective_state);
        if (expected_cpus.empty()) return applied;

        applied = run_takeover_sequence(
            pid, tid, result.cpumask_name, expected_cpus,
            [](int target_tid, const std::vector<int>& cpus) {
                return CpuMask::set_affinity(target_tid, cpus);
            },
            [this](int target_tid, const std::string& mask_name) {
                return move_to_cpuset_cgroup(target_tid, mask_name);
            },
            [this](int target_tid) {
                return move_to_top_app_parent(target_tid);
            },
            [](int target_pid, int target_tid, const std::string& mask_name) {
                const auto paths = FileUtils::get_thread_cgroup_paths_uncached(
                    target_pid, target_tid);
                return paths.readable
                    && paths.cpuset == "/top-app/ReUperf_" + mask_name;
            },
            [](int target_tid, const std::vector<int>& cpus) {
                return !CpuMask::is_affinity_changed(target_tid, cpus);
            });

        if (!applied.success()) {
            LOG_W("CpusetSetter", "Affinity takeover incomplete for tid "
                  + std::to_string(tid) + " after " + std::to_string(applied.attempts)
                  + " attempts (pre=" + std::to_string(applied.pre_affinity_succeeded)
                  + ", initial_cpuset=" + std::to_string(applied.initial_cpuset_succeeded)
                  + ", parent_cpuset=" + std::to_string(applied.parent_cpuset_succeeded)
                  + ", final_cpuset=" + std::to_string(applied.cpuset_succeeded)
                  + ", post=" + std::to_string(applied.post_affinity_succeeded)
                  + ", verify_cpuset=" + std::to_string(applied.cpuset_verified)
                  + ", verify_affinity=" + std::to_string(applied.affinity_verified) + ")");
        }
        return applied;
    }

private:
    ThreadMatcher& matcher_;
    TimingConfig timing_;
    struct GroupCheck {
        bool ready = false;
        std::chrono::steady_clock::time_point checked_at;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, GroupCheck> group_checks_;

    bool is_group_ready(const std::string& path) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = group_checks_.find(path);
            if (it != group_checks_.end()
                && now - it->second.checked_at
                    < std::chrono::milliseconds(timing_.cpuset_group_check_ttl_ms)) {
                return it->second.ready;
            }
        }

        const bool cpus_ready = !FileUtils::read_file(path + "/cpus").empty();
        const bool mems_ready = !FileUtils::read_file(path + "/mems").empty();
        const bool tasks_ready = FileUtils::file_exists(path + "/tasks");
        const bool ready = cpus_ready && mems_ready && tasks_ready;
        if (!ready) {
            LOG_W("CpusetSetter", "cpuset group is not ready: " + path
                  + " (cpus=" + std::to_string(cpus_ready)
                  + ", mems=" + std::to_string(mems_ready)
                  + ", tasks=" + std::to_string(tasks_ready) + ")");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            group_checks_[path] = {ready, now};
        }
        return ready;
    }

    void invalidate_group(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        group_checks_.erase(path);
    }
};

#endif
