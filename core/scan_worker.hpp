#ifndef SCAN_WORKER_HPP
#define SCAN_WORKER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <pthread.h>
#include "../utils/logger.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/cpu_mask.hpp"
#include "../config/config_types.hpp"
#include "../core/thread_matcher.hpp"
#include "../core/thread_cache.hpp"
#include "../scheduler/cpuset_setter.hpp"
#include "../scheduler/priority_setter.hpp"
#include "../scheduler/cpuctl_setter.hpp"

struct DispatchTask {
    int pid;
    int tid;
    uint64_t process_start_time;
    uint64_t thread_start_time;
    std::string thread_name;
    ProcessState state;
    std::string cpuset_base;
    std::string proc_name;
    std::string cmdline;
};

class ScanWorker {
    struct TaskKey {
        int pid;
        int tid;
        uint64_t process_start_time;
        uint64_t thread_start_time;

        bool operator==(const TaskKey& other) const {
            return pid == other.pid && tid == other.tid
                && process_start_time == other.process_start_time
                && thread_start_time == other.thread_start_time;
        }
    };

    struct TaskKeyHash {
        size_t operator()(const TaskKey& key) const {
            size_t hash = std::hash<int>{}(key.pid);
            hash ^= std::hash<int>{}(key.tid) + 0x9e3779b9U + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint64_t>{}(key.process_start_time) + 0x9e3779b9U
                + (hash << 6) + (hash >> 2);
            hash ^= std::hash<uint64_t>{}(key.thread_start_time) + 0x9e3779b9U
                + (hash << 6) + (hash >> 2);
            return hash;
        }
    };

public:
    explicit ScanWorker(const std::string& name, const TimingConfig& timing = {})
        : name_(name), running_(false), started_(false), timing_(timing),
          matcher_(nullptr), cpuset_(nullptr), prio_(nullptr),
          cpuctl_(nullptr), cache_(nullptr) {}
    
    ~ScanWorker() {
        stop();
    }
    
    void set_configs(std::shared_ptr<ThreadMatcher> matcher,
                     std::shared_ptr<CpusetSetter> cpuset,
                     std::shared_ptr<PrioritySetter> prio,
                     std::shared_ptr<CpuctlSetter> cpuctl,
                     std::shared_ptr<ThreadCache> cache) {
        matcher_ = matcher;
        cpuset_ = cpuset;
        prio_ = prio;
        cpuctl_ = cpuctl;
        cache_ = cache;
    }
    
    bool start() {
        if (started_.load()) {
            LOG_W("ScanWorker", name_ + " already running");
            return true;
        }
        
        if (!matcher_ || !cpuset_ || !prio_ || !cpuctl_ || !cache_) {
            LOG_E("ScanWorker", name_ + " configs not set before start");
            return false;
        }
        
        running_.store(true);
        started_.store(true);
        try {
            thread_ = std::thread(&ScanWorker::worker_loop, this);
        } catch (...) {
            started_.store(false);
            running_.store(false);
            throw;
        }
        
        #ifdef __linux__
        const int setname_result = pthread_setname_np(thread_.native_handle(), name_.c_str());
        if (setname_result != 0) {
            LOG_W("ScanWorker", "Failed to set thread name for " + name_ + ": "
                  + std::string(strerror(setname_result)));
        }
        #endif
        
        LOG_I("ScanWorker", name_ + " started");
        return true;
    }
    
    void stop() {
        const bool was_started = started_.exchange(false);
        running_.store(false);
        cv_.notify_all();
        
        if (thread_.joinable()) {
            thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::queue<TaskKey> empty;
            task_queue_.swap(empty);
            pending_tasks_.clear();
        }

        if (was_started) LOG_I("ScanWorker", name_ + " stopped");
    }
    
    bool is_running() const { return started_.load(); }
    
    enum class EnqueueResult {
        Queued,
        Updated,
        QueueFull,
        Stopped
    };

    EnqueueResult enqueue(const DispatchTask& task) {
        const TaskKey key = task_key(task);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_.load()) {
                return EnqueueResult::Stopped;
            }
            const auto pending = pending_tasks_.find(key);
            if (pending != pending_tasks_.end()) {
                // Preserve one queue node while replacing stale state with the newest task.
                pending->second = task;
                return EnqueueResult::Updated;
            }
            if (task_queue_.size() >= kMaxQueuedTasks) {
                LOG_W("ScanWorker", name_ + " task queue full, dropping TID " + std::to_string(task.tid));
                return EnqueueResult::QueueFull;
            }
            task_queue_.push(key);
            pending_tasks_.emplace(key, task);
        }
        cv_.notify_one();
        return EnqueueResult::Queued;
    }

private:
    std::string name_;
    std::atomic<bool> running_;
    std::atomic<bool> started_;
    TimingConfig timing_;
    static constexpr size_t kMaxQueuedTasks = 4096;
    std::thread thread_;
    std::queue<TaskKey> task_queue_;
    std::unordered_map<TaskKey, DispatchTask, TaskKeyHash> pending_tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    std::shared_ptr<ThreadMatcher> matcher_;
    std::shared_ptr<CpusetSetter> cpuset_;
    std::shared_ptr<PrioritySetter> prio_;
    std::shared_ptr<CpuctlSetter> cpuctl_;
    std::shared_ptr<ThreadCache> cache_;

    static TaskKey task_key(const DispatchTask& task) {
        return {task.pid, task.tid, task.process_start_time, task.thread_start_time};
    }
    
    static constexpr size_t kScheduleCleanupThreshold = 500;
    std::unordered_map<TaskKey, std::chrono::steady_clock::time_point, TaskKeyHash>
        last_schedule_time_;
    std::unordered_map<TaskKey, ProcessState, TaskKeyHash> last_schedule_state_;
    std::unordered_map<TaskKey, std::chrono::steady_clock::time_point, TaskKeyHash>
        next_cgroup_check_time_;
    mutable std::mutex last_schedule_mutex_;
    size_t schedule_counter_ = 0;
    size_t cgroup_check_counter_ = 0;

    void cleanup_expired_schedule_times(const std::chrono::steady_clock::time_point& now) {
        for (auto it = last_schedule_time_.begin(); it != last_schedule_time_.end(); ) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second).count();
            if (elapsed > timing_.schedule_cleanup_interval_ms) {
                last_schedule_state_.erase(it->first);
                it = last_schedule_time_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = next_cgroup_check_time_.begin(); it != next_cgroup_check_time_.end(); ) {
            if (now - it->second > std::chrono::milliseconds(timing_.schedule_cleanup_interval_ms)) {
                it = next_cgroup_check_time_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool should_skip_schedule(const DispatchTask& task) {
        std::lock_guard<std::mutex> lock(last_schedule_mutex_);
        const auto now = std::chrono::steady_clock::now();
        const TaskKey key = task_key(task);
        const auto time_it = last_schedule_time_.find(key);
        const auto state_it = last_schedule_state_.find(key);
        const bool same_state = state_it != last_schedule_state_.end()
            && state_it->second == task.state;
        if (task.state != ProcessState::TOP && same_state
            && time_it != last_schedule_time_.end()) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - time_it->second).count();
            if (elapsed < timing_.min_schedule_interval_ms) {
                return true;
            }
        }
        last_schedule_time_[key] = now;
        last_schedule_state_[key] = task.state;
        if (++schedule_counter_ >= kScheduleCleanupThreshold) {
            schedule_counter_ = 0;
            cleanup_expired_schedule_times(now);
        }
        return false;
    }

    bool should_check_cgroup(const DispatchTask& task) {
        std::lock_guard<std::mutex> lock(last_schedule_mutex_);
        const auto now = std::chrono::steady_clock::now();
        const TaskKey key = task_key(task);
        auto it = next_cgroup_check_time_.find(key);
        if (timing_.cgroup_check_interval_ms == 0) {
            return true;
        }
        if (it == next_cgroup_check_time_.end()) {
            const auto stagger_ms = static_cast<int64_t>(
                TaskKeyHash{}(key) % static_cast<size_t>(timing_.cgroup_check_interval_ms));
            next_cgroup_check_time_[key] = now + std::chrono::milliseconds(stagger_ms);
            return stagger_ms == 0;
        }
        if (now < it->second) {
            return false;
        }
        it->second = now + std::chrono::milliseconds(timing_.cgroup_check_interval_ms);
        if (++cgroup_check_counter_ >= kScheduleCleanupThreshold) {
            cgroup_check_counter_ = 0;
            cleanup_expired_schedule_times(now);
        }
        return true;
    }

    bool is_cgroup_changed(const DispatchTask& task, const MatchResult& result) {
        const bool check_cpuset = !result.affinity_class.empty()
            && result.affinity_class != "auto" && !result.cpumask_name.empty();
        const bool check_cpuctl = result.enable_limit && result.thread_rule_index >= 0;
        const bool aggressive_check = result.effective_state == ProcessState::TOP
            || result.pinned || result.topfore;
        if ((!check_cpuset && !check_cpuctl)
            || (!aggressive_check && !should_check_cgroup(task))) {
            return false;
        }

        const FileUtils::ThreadCgroupPaths paths =
            FileUtils::get_thread_cgroup_paths_uncached(task.pid, task.tid);
        if (!paths.readable) {
            // Unknown is conservatively treated as drift; task identity is revalidated
            // before applying and no stale success is cached.
            return true;
        }
        if (check_cpuset
            && paths.cpuset != "/top-app/ReUperf_" + result.cpumask_name) {
            return true;
        }
        const std::string expected_cpuctl = "/ReUperf/" + result.matched_rule_name
            + "/A" + std::to_string(result.thread_rule_index + 1);
        return check_cpuctl && paths.cpuctl != expected_cpuctl;
    }
    
    void worker_loop() {
        while (running_.load()) {
            DispatchTask task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [this] { 
                    return !running_.load() || !task_queue_.empty(); 
                });
                
                if (!running_.load() && task_queue_.empty()) {
                    break;
                }
                
                if (!task_queue_.empty()) {
                    const TaskKey key = task_queue_.front();
                    task_queue_.pop();
                    const auto pending = pending_tasks_.find(key);
                    if (pending == pending_tasks_.end()) {
                        continue;
                    }
                    task = std::move(pending->second);
                    pending_tasks_.erase(pending);
                } else {
                    continue;
                }
            }
            
            try {
                process_dispatch_task(task);
            } catch (const std::exception& e) {
                LOG_E("ScanWorker", name_ + " task failed: " + e.what());
            } catch (...) {
                LOG_E("ScanWorker", name_ + " task failed with unknown exception");
            }
        }
    }
    
    bool needs_affinity_management(const MatchResult& result) const {
        return has_resolved_affinity(result);
    }

    bool needs_priority_management(const MatchResult& result, ThreadMatcher& matcher) const {
        return matcher.get_prio_value(result.prio_class, result.effective_state) != 0;
    }

    bool needs_managed_state(const MatchResult& result, ThreadMatcher& matcher) const {
        return result.matched && (needs_affinity_management(result)
            || needs_priority_management(result, matcher) || result.enable_limit);
    }

    void capture_baseline_if_needed(const DispatchTask& task, ThreadCache& cache,
                                    PrioritySetter& prio) {
        ThreadBaseline baseline;
        baseline.affinity = CpuMask::get_affinity_from_status(task.tid);
        baseline.has_affinity = !baseline.affinity.empty();
        baseline.has_scheduler = prio.capture_scheduler_baseline(
            task.tid, baseline.scheduler_policy, baseline.scheduler_priority, baseline.nice);

        const FileUtils::ThreadCgroupPaths cgroups =
            FileUtils::get_thread_cgroup_paths_uncached(task.pid, task.tid);
        if (!cgroups.cpuset.empty()) {
            baseline.cpuset_path = "/dev/cpuset" + cgroups.cpuset;
            baseline.has_cpuset_path = true;
        }
        if (!cgroups.cpuctl.empty()) {
            baseline.cpuctl_path = "/dev/cpuctl" + cgroups.cpuctl;
            baseline.has_cpuctl_path = true;
        }
        cache.set_baseline(task.pid, task.tid, std::move(baseline));
    }

    bool restore_affinity_baseline(int tid, const ThreadBaseline& baseline,
                                   CpusetSetter& cpuset) {
        bool restored = true;
        if (baseline.has_cpuset_path
            && !cpuset.restore_cpuset_cgroup(tid, baseline.cpuset_path)) {
            LOG_W("ScanWorker", name_ + " failed to restore cpuset cgroup for tid "
                  + std::to_string(tid));
            restored = false;
        }
        if (baseline.has_affinity && !cpuset.restore_affinity(tid, baseline.affinity)) {
            LOG_W("ScanWorker", name_ + " failed to restore affinity for tid "
                  + std::to_string(tid));
            restored = false;
        }
        return restored;
    }

    bool restore_scheduler_baseline(int tid, const ThreadBaseline& baseline,
                                    PrioritySetter& prio) {
        if (!baseline.has_scheduler || !prio.restore_scheduler(
                tid, baseline.scheduler_policy, baseline.scheduler_priority, baseline.nice)) {
            LOG_W("ScanWorker", name_ + " failed to restore scheduler for tid "
                  + std::to_string(tid));
            return false;
        }
        return true;
    }

    bool restore_cpuctl_baseline(int tid, const ThreadBaseline& baseline,
                                 CpuctlSetter& cpuctl) {
        if (!baseline.has_cpuctl_path
            || !cpuctl.restore_cpuctl_cgroup(tid, baseline.cpuctl_path)) {
            LOG_W("ScanWorker", name_ + " failed to restore cpuctl cgroup for tid "
                  + std::to_string(tid));
            return false;
        }
        return true;
    }

    void restore_baseline(int pid, int tid, ThreadCache& cache, CpusetSetter& cpuset,
                          PrioritySetter& prio, CpuctlSetter& cpuctl) {
        const auto baseline = cache.get_baseline(pid, tid);
        if (!baseline) {
            cache.clear_applied_result(pid, tid);
            cache.clear_managed_components(pid, tid);
            return;
        }

        bool restored = true;
        // Reverse the apply order: cpuctl, scheduler, cpuset cgroup, then affinity.
        if (baseline->has_cpuctl_path && !restore_cpuctl_baseline(tid, *baseline, cpuctl)) {
            restored = false;
        }
        if (baseline->has_scheduler && !restore_scheduler_baseline(tid, *baseline, prio)) {
            restored = false;
        }
        if (!restore_affinity_baseline(tid, *baseline, cpuset)) {
            restored = false;
        }
        if (restored) {
            cache.clear_baseline(pid, tid);
            cache.clear_applied_result(pid, tid);
            cache.clear_managed_components(pid, tid);
        }
    }

    void apply_result(const DispatchTask& task, const MatchResult& result,
                      const std::string& cpuset_base, ThreadMatcher& matcher, ThreadCache& cache,
                      CpusetSetter& cpuset, PrioritySetter& prio, CpuctlSetter& cpuctl) {
        if (!needs_managed_state(result, matcher)) {
            restore_baseline(task.pid, task.tid, cache, cpuset, prio, cpuctl);
            return;
        }
        capture_baseline_if_needed(task, cache, prio);
        const auto baseline = cache.get_baseline(task.pid, task.tid);
        const bool needs_affinity = needs_affinity_management(result);
        const bool needs_priority = needs_priority_management(result, matcher);
        if (!baseline || (needs_affinity && (!baseline->has_affinity || !baseline->has_cpuset_path))
            || (needs_priority && !baseline->has_scheduler)
            || (result.enable_limit && !baseline->has_cpuctl_path)) {
            LOG_W("ScanWorker", name_ + " skipped scheduling for tid " + std::to_string(task.tid)
                  + " because its required baseline could not be captured");
            return;
        }

        ManagedComponents managed = cache.get_managed_components(task.pid, task.tid);
        if (should_restore_managed_component(managed.affinity, needs_affinity)) {
            if (!restore_affinity_baseline(task.tid, *baseline, cpuset)) {
                LOG_W("ScanWorker", name_ + " skipped scheduling for tid "
                      + std::to_string(task.tid)
                      + " because its affinity baseline could not be restored");
                return;
            }
            managed.affinity = false;
            cache.set_managed_components(task.pid, task.tid, managed);
        }
        if (should_restore_managed_component(managed.priority, needs_priority)) {
            if (!restore_scheduler_baseline(task.tid, *baseline, prio)) {
                LOG_W("ScanWorker", name_ + " skipped scheduling for tid "
                      + std::to_string(task.tid)
                      + " because its scheduler baseline could not be restored");
                return;
            }
            managed.priority = false;
            cache.set_managed_components(task.pid, task.tid, managed);
        }
        if (should_restore_managed_component(managed.cpuctl, result.enable_limit)) {
            if (!restore_cpuctl_baseline(task.tid, *baseline, cpuctl)) {
                LOG_W("ScanWorker", name_ + " skipped scheduling for tid "
                      + std::to_string(task.tid)
                      + " because its cpuctl baseline could not be restored");
                return;
            }
            managed.cpuctl = false;
            cache.set_managed_components(task.pid, task.tid, managed);
        }

        // Do not connect state-changing operations with short-circuit operators. A
        // temporary failure in one dimension must not suppress any later operation.
        // Mark every requested dimension as managed after attempting it: a failed call
        // can still have changed an earlier syscall or moved the thread partway.
        const AffinityTakeoverResult affinity_result =
            cpuset.apply_with_result(task.pid, task.tid, result, cpuset_base,
                                     task.process_start_time, task.thread_start_time);
        if (!is_current_task_identity(task)) {
            cache.erase_thread_if_identity(task.pid, task.tid, task.process_start_time,
                                           task.thread_start_time);
            return;
        }
        const bool priority_applied = prio.apply_with_result(task.pid, task.tid, result);
        if (!is_current_task_identity(task)) {
            cache.erase_thread_if_identity(task.pid, task.tid, task.process_start_time,
                                           task.thread_start_time);
            return;
        }
        const bool cpuctl_applied = cpuctl.apply_with_result(task.pid, task.tid, result);
        managed.affinity = managed.affinity || needs_affinity;
        managed.priority = managed.priority || needs_priority;
        managed.cpuctl = managed.cpuctl || result.enable_limit;
        const bool affinity_applied = !needs_affinity || affinity_result.success();
        if (is_current_task_identity(task)) {
            cache.set_managed_components(task.pid, task.tid, managed);
            if (affinity_applied && priority_applied && cpuctl_applied) {
                cache.update_applied_result(task.pid, task.tid, result);
                return;
            }
        } else {
            cache.erase_thread_if_identity(task.pid, task.tid, task.process_start_time,
                                           task.thread_start_time);
            return;
        }

        LOG_W("ScanWorker", name_ + " scheduling takeover incomplete for tid "
              + std::to_string(task.tid) + "; preserving partial state for retry");
        // A failed takeover does not mean management ended. Keep both the baseline and
        // every successfully applied component; the next dispatch will retry. Only an
        // actual rule/component removal or shutdown performs baseline restoration.
        if (!is_current_task_identity(task)) {
            cache.erase_thread_if_identity(task.pid, task.tid, task.process_start_time,
                                           task.thread_start_time);
        }
    }

    bool is_current_task_identity(const DispatchTask& task) const {
        return FileUtils::get_process_start_time(task.pid) == task.process_start_time
            && FileUtils::get_thread_start_time(task.pid, task.tid) == task.thread_start_time
            && FileUtils::is_thread_in_process(task.pid, task.tid);
    }

    void process_dispatch_task(const DispatchTask& task) {
        auto matcher = matcher_;
        auto cpuset = cpuset_;
        auto prio = prio_;
        auto cpuctl = cpuctl_;
        auto cache = cache_;
        if (!matcher || !cpuset || !prio || !cpuctl || !cache) {
            LOG_E("ScanWorker", name_ + " configs null during task processing");
            return;
        }
        if (!is_current_task_identity(task)) {
            LOG_D("ScanWorker", name_ + " dropping stale task for pid " + std::to_string(task.pid)
                  + ", tid " + std::to_string(task.tid));
            cache->erase_thread_if_identity(task.pid, task.tid, task.process_start_time,
                                            task.thread_start_time);
            return;
        }
        if (should_skip_schedule(task)) {
            return;
        }

        if (const auto entry = cache->lookup(task.pid, task.tid, task.process_start_time,
                                             task.thread_start_time, task.thread_name, task.cmdline, task.state)) {
            const auto applied = cache->get_applied_result(task.pid, task.tid);
            if (applied && is_result_equal(*applied, entry->result)) {
                if (!needs_managed_state(*applied, *matcher)) {
                    return;
                }
                bool affinity_changed = false;
                if (needs_affinity_management(*applied)) {
                    const auto expected_cpus = cpuset->get_cpus_for_affinity(
                        applied->affinity_class, applied->effective_state);
                    affinity_changed = CpuMask::is_affinity_changed(task.tid, expected_cpus);
                }
                const int expected_prio = matcher->get_prio_value(
                    applied->prio_class, applied->effective_state);
                const bool sched_changed = expected_prio != 0
                    && prio->is_sched_changed(task.tid, expected_prio);
                // Check each drift source independently. Detecting one mismatch must not
                // suppress detection of another one in the same takeover cycle.
                const bool cgroup_changed = is_cgroup_changed(task, *applied);
                if (!affinity_changed && !sched_changed && !cgroup_changed) {
                    return;
                }
            }
            apply_result(task, entry->result, entry->cpuset_base, *matcher, *cache,
                         *cpuset, *prio, *cpuctl);
            return;
        }

        const MatchResult result = matcher->match(task.proc_name, task.thread_name,
                                                   task.state, task.pid, task.tid, task.cmdline);
        const std::string cpuctl_base = cpuctl->get_cpuctl_base(result.effective_state);
        cache->update(task.pid, task.tid, task.process_start_time, task.thread_start_time,
                      task.thread_name, task.cmdline,
                      task.state, result, task.cpuset_base, cpuctl_base);
        apply_result(task, result, task.cpuset_base, *matcher, *cache,
                     *cpuset, *prio, *cpuctl);
    }
};

#endif
