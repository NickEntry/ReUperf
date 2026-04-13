#ifndef SCAN_WORKER_HPP
#define SCAN_WORKER_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <unordered_map>
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
        
        started_.store(false);
        LOG_I("ScanWorker", name_ + " stopped");
    }
    
    bool is_running() const { return started_.load(); }
    
    void enqueue(const DispatchTask& task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task_queue_.push(task);
        }
        cv_.notify_one();
    }

private:
    std::string name_;
    std::atomic<bool> running_;
    std::atomic<bool> started_;
    std::thread thread_;
    std::queue<DispatchTask> task_queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    
    std::shared_ptr<ThreadMatcher> matcher_;
    std::shared_ptr<CpusetSetter> cpuset_;
    std::shared_ptr<PrioritySetter> prio_;
    std::shared_ptr<CpuctlSetter> cpuctl_;
    std::shared_ptr<ThreadCache> cache_;
    
    // 抖动抑制：记录每个 tid 的上次调度时间
    static constexpr int64_t kMinScheduleIntervalMs = 200;
    std::unordered_map<int, std::chrono::steady_clock::time_point> last_schedule_time_;
    mutable std::mutex last_schedule_mutex_;
    
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
                } else {
                    continue;
                }
            }
            
            process_dispatch_task(task);
        }
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

        // 抖动抑制：检查是否需要跳过调度
        if (should_skip_schedule(task.tid)) {
            LOG_T("ScanWorker", name_ + " skipped TID " + std::to_string(task.tid)
                  + " (throttled)");
            return;
        }

        bool affinity_changed = false;
        bool sched_changed = false;

        if (auto entry = cache->lookup(task.pid, task.tid, task.thread_name, task.state)) {
            auto applied = cache->get_applied_result(task.pid, task.tid);
            if (applied && is_result_equal(*applied, entry->result)) {
                auto expected_cpus = cpuset->get_cpus_for_affinity(applied->affinity_class, applied->effective_state);
                affinity_changed = CpuMask::is_affinity_changed_from_status(task.tid, expected_cpus);
                int expected_prio = matcher->get_prio_value(applied->prio_class, applied->effective_state);
                sched_changed = prio->is_sched_changed(task.tid, expected_prio);
                if (!affinity_changed && !sched_changed) {
                    LOG_T("ScanWorker", name_ + " skipped TID " + std::to_string(task.tid)
                          + " (cached, no change)");
                    return;
                }
                LOG_T("ScanWorker", name_ + " detected change for TID " + std::to_string(task.tid)
                      + (affinity_changed ? " affinity" : "") + (sched_changed ? " sched" : ""));
            }
            cache->update_applied_result(task.pid, task.tid, entry->result);
            cpuset->apply_with_result(task.pid, task.tid, entry->result, entry->cpuset_base);
            prio->apply_with_result(task.pid, task.tid, entry->result);
            cpuctl->apply_with_result(task.pid, task.tid, entry->result);
            LOG_T("ScanWorker", name_ + " applied cached to TID " + std::to_string(task.tid));
        } else {
            MatchResult result = matcher->match(task.proc_name, task.thread_name,
                                                task.state, task.pid, task.cmdline);
            std::string cpuctl_base = cpuctl->get_cpuctl_base(result.effective_state);
            cache->update(task.pid, task.tid, task.thread_name, task.state,
                         result, task.cpuset_base, cpuctl_base);
            if (result.matched) {
                cache->update_applied_result(task.pid, task.tid, result);
            }
            cpuset->apply_with_result(task.pid, task.tid, result, task.cpuset_base);
            prio->apply_with_result(task.pid, task.tid, result);
            cpuctl->apply_with_result(task.pid, task.tid, result);
            LOG_T("ScanWorker", name_ + " matched and applied to TID " + std::to_string(task.tid)
                  + " rule=" + result.matched_rule_name);
        }
    }
};

#endif
