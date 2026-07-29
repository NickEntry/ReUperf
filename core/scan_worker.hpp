#ifndef SCAN_WORKER_HPP
#define SCAN_WORKER_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <condition_variable>
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
    std::string thread_name;
    ProcessState state;
    std::string cpuset_base;
    std::string proc_name;
    std::string cmdline;
};

class ScanWorker {
public:
    explicit ScanWorker(const std::string& name) 
        : name_(name), running_(false), started_(false),
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
        
        running_ = true;
        thread_ = std::thread(&ScanWorker::worker_loop, this);
        
        #ifdef __linux__
        pthread_setname_np(thread_.native_handle(), name_.c_str());
        #endif
        
        LOG_I("ScanWorker", name_ + " started");
        started_ = true;
        return true;
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        cv_.notify_all();
        
        if (thread_.joinable()) {
            thread_.join();
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::queue<DispatchTask> empty;
            task_queue_.swap(empty);
            queued_tasks_.clear();
        }

        started_.store(false);
        LOG_I("ScanWorker", name_ + " stopped");
    }
    
    bool is_running() const { return started_.load(); }
    
    void enqueue(const DispatchTask& task) {
        const uint64_t key = task_key(task.pid, task.tid);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queued_tasks_.count(key) > 0) {
                return;
            }
            if (task_queue_.size() >= kMaxQueuedTasks) {
                LOG_W("ScanWorker", name_ + " task queue full, dropping TID " + std::to_string(task.tid));
                return;
            }
            task_queue_.push(task);
            queued_tasks_.insert(key);
        }
        cv_.notify_one();
    }

private:
    std::string name_;
    std::atomic<bool> running_;
    std::atomic<bool> started_;
    static constexpr size_t kMaxQueuedTasks = 4096;
    std::thread thread_;
    std::queue<DispatchTask> task_queue_;
    std::unordered_set<uint64_t> queued_tasks_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    std::shared_ptr<ThreadMatcher> matcher_;
    std::shared_ptr<CpusetSetter> cpuset_;
    std::shared_ptr<PrioritySetter> prio_;
    std::shared_ptr<CpuctlSetter> cpuctl_;
    std::shared_ptr<ThreadCache> cache_;

    static uint64_t task_key(int pid, int tid) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(pid)) << 32)
            | static_cast<uint32_t>(tid);
    }
    
    // 抖动抑制：记录每个 tid 的上次调度时间
    static constexpr int64_t kMinScheduleIntervalMs = 200;
    static constexpr int64_t kScheduleCleanupIntervalMs = 5000;
    static constexpr size_t kScheduleCleanupThreshold = 500;
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_schedule_time_;
    mutable std::mutex last_schedule_mutex_;
    size_t schedule_counter_ = 0;
    
    void cleanup_expired_schedule_times(const std::chrono::steady_clock::time_point& now) {
        for (auto it = last_schedule_time_.begin(); it != last_schedule_time_.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed > kScheduleCleanupIntervalMs) {
                it = last_schedule_time_.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // 检查是否需要跳过调度（抖动抑制）
    bool should_skip_schedule(int tid) {
        std::lock_guard<std::mutex> lock(last_schedule_mutex_);
        auto now = std::chrono::steady_clock::now();
        auto it = last_schedule_time_.find(tid);
        if (it != last_schedule_time_.end()) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second).count();
            if (elapsed < kMinScheduleIntervalMs) {
                return true;
            }
        }
        last_schedule_time_[tid] = now;
        if (++schedule_counter_ >= kScheduleCleanupThreshold) {
            schedule_counter_ = 0;
            cleanup_expired_schedule_times(now);
        }
        return false;
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
                    task = task_queue_.front();
                    task_queue_.pop();
                    queued_tasks_.erase(task_key(task.pid, task.tid));
                } else {
                    continue;
                }
            }
            
            process_dispatch_task(task);
        }
    }
    
    bool needs_managed_state(const MatchResult& result, ThreadMatcher& matcher) const {
        if (!result.matched) {
            return false;
        }
        const bool has_affinity = !result.affinity_class.empty() && result.affinity_class != "auto";
        const bool has_priority = matcher.get_prio_value(result.prio_class, result.effective_state) != 0;
        return has_affinity || has_priority || result.enable_limit;
    }

    void capture_baseline_if_needed(const DispatchTask& task, ThreadCache& cache,
                                    PrioritySetter& prio) {
        if (cache.has_baseline(task.pid, task.tid)) {
            return;
        }

        ThreadBaseline baseline;
        baseline.affinity = CpuMask::get_affinity_from_status(task.tid);
        baseline.has_affinity = !baseline.affinity.empty();
        baseline.has_scheduler = prio.capture_scheduler_baseline(
            task.tid, baseline.scheduler_policy, baseline.scheduler_priority, baseline.nice);

        const std::string cpuset_path = FileUtils::get_thread_cgroup_path(task.pid, task.tid, "cpuset");
        if (!cpuset_path.empty()) {
            baseline.cpuset_path = "/dev/cpuset" + cpuset_path;
            baseline.has_cpuset_path = true;
        }
        std::string cpuctl_path = FileUtils::get_thread_cgroup_path(task.pid, task.tid, "cpu");
        if (cpuctl_path.empty()) {
            cpuctl_path = FileUtils::get_thread_cgroup_path(task.pid, task.tid, "cpuctl");
        }
        if (!cpuctl_path.empty()) {
            baseline.cpuctl_path = "/dev/cpuctl" + cpuctl_path;
            baseline.has_cpuctl_path = true;
        }
        cache.set_baseline(task.pid, task.tid, std::move(baseline));
    }

    void restore_baseline(int pid, int tid, ThreadCache& cache, CpusetSetter& cpuset,
                          PrioritySetter& prio, CpuctlSetter& cpuctl) {
        const auto baseline = cache.take_baseline(pid, tid);
        if (!baseline) {
            return;
        }
        if (baseline->has_affinity && !cpuset.restore_affinity(tid, baseline->affinity)) {
            LOG_W("ScanWorker", name_ + " failed to restore affinity for tid " + std::to_string(tid));
        }
        if (baseline->has_scheduler && !prio.restore_scheduler(
                tid, baseline->scheduler_policy, baseline->scheduler_priority, baseline->nice)) {
            LOG_W("ScanWorker", name_ + " failed to restore scheduler for tid " + std::to_string(tid));
        }
        if (baseline->has_cpuset_path && !cpuset.restore_cpuset_cgroup(tid, baseline->cpuset_path)) {
            LOG_W("ScanWorker", name_ + " failed to restore cpuset cgroup for tid " + std::to_string(tid));
        }
        if (baseline->has_cpuctl_path && !cpuctl.restore_cpuctl_cgroup(tid, baseline->cpuctl_path)) {
            LOG_W("ScanWorker", name_ + " failed to restore cpuctl cgroup for tid " + std::to_string(tid));
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
        cache.update_applied_result(task.pid, task.tid, result);
        cpuset.apply_with_result(task.pid, task.tid, result, cpuset_base);
        prio.apply_with_result(task.pid, task.tid, result);
        cpuctl.apply_with_result(task.pid, task.tid, result);
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
        if (should_skip_schedule(task.tid)) {
            return;
        }

        if (const auto entry = cache->lookup(task.pid, task.tid, task.thread_name, task.state)) {
            const auto applied = cache->get_applied_result(task.pid, task.tid);
            if (applied && is_result_equal(*applied, entry->result)) {
                if (!needs_managed_state(*applied, *matcher)) {
                    return;
                }
                bool affinity_changed = false;
                if (!applied->affinity_class.empty() && applied->affinity_class != "auto") {
                    const auto expected_cpus = cpuset->get_cpus_for_affinity(
                        applied->affinity_class, applied->effective_state);
                    affinity_changed = CpuMask::is_affinity_changed_from_status(task.tid, expected_cpus);
                }
                const int expected_prio = matcher->get_prio_value(
                    applied->prio_class, applied->effective_state);
                const bool sched_changed = expected_prio != 0
                    && prio->is_sched_changed(task.tid, expected_prio);
                if (!affinity_changed && !sched_changed) {
                    return;
                }
            }
            apply_result(task, entry->result, entry->cpuset_base, *matcher, *cache,
                         *cpuset, *prio, *cpuctl);
            return;
        }

        const MatchResult result = matcher->match(task.proc_name, task.thread_name,
                                                   task.state, task.pid, task.cmdline);
        const std::string cpuctl_base = cpuctl->get_cpuctl_base(result.effective_state);
        cache->update(task.pid, task.tid, task.thread_name, task.state,
                      result, task.cpuset_base, cpuctl_base);
        apply_result(task, result, task.cpuset_base, *matcher, *cache,
                     *cpuset, *prio, *cpuctl);
    }
};

#endif
