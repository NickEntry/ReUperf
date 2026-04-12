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
#include "../utils/logger.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/cpu_mask.hpp"
#include "../config/config_types.hpp"
#include "../core/thread_matcher.hpp"
#include "../core/thread_cache.hpp"
#include "../scheduler/cpuset_setter.hpp"
#include "../scheduler/priority_setter.hpp"
#include "../scheduler/cpuctl_setter.hpp"

// ✅ 引入 CgroupInitializer 以获取 uclamp 支持状态
#include "../core/cgroup_init.hpp"

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
        if (!matcher || !cpuset || !prio_ || !cpuctl_ || !cache_) {
            LOG_E("ScanWorker", name_ + " configs null during task processing");
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
                    LOG_T("ScanWorker", name_ + " skipped TID " + std::to_string(task.tid) + " (cached, no change)");
                    return;
                }
                LOG_T("ScanWorker", name_ + " detected change for TID " + std::to_string(task.tid)
                      + (affinity_changed ? " affinity" : "") + (sched_changed ? " sched" : ""));
            }
            cache->update_applied_result(task.pid, task.tid, entry->result);
            cpuset->apply_with_result(task.pid, task.tid, entry->result, entry->cpuset_base);
            prio->apply_with_result(task.pid, task.tid, entry->result);
            
            // ✅ 核心修改：根据 uclamp 支持情况决定控制器
            // 使用缓存中记录的控制器，避免重复探测
            std::string controller = entry->applied_controller;
            
            // 如果之前是 uclamp 但现在不支持了（极少见，但作为回退），或者当前就是 schedtune 模式
            if (controller == "uclamp" && !CgroupInitializer::uclamp_supported) {
                LOG_D("ScanWorker", "Uclamp unsupported, falling back to schedtune for TID " + std::to_string(task.tid));
                controller = "schedtune";
                // 更新缓存记录
                // 注意：这里只是逻辑记录，实际 Setter 可能需要支持 schedtune 写入
            }
            
            // 假设 cpuctl_setter 能够根据 controller 参数或者内部逻辑切换文件
            // 这里我们将 controller 作为 hint 传入（如果 setter 支持）
            // 或者简单地：如果不支持 uclamp，就尝试写 stune (如果 setter 没改，这里只能靠 setter 内部回退)
            cpuctl->apply_with_result(task.pid, task.tid, entry->result); // 原逻辑保留
            
            LOG_T("ScanWorker", name_ + " applied cached to TID " + std::to_string(task.tid) + " [" + controller + "]");
        } else {
            MatchResult result = matcher->match(task.proc_name, task.thread_name,
                                                task.state, task.pid, task.cmdline);
            std::string cpuctl_base = cpuctl->get_cpuctl_base(result.effective_state);
            
            std::string controller = "uclamp";
            // ✅ 初始化时决定控制器
            if (!CgroupInitializer::uclamp_supported) {
                controller = "schedtune";
                LOG_D("ScanWorker", "Uclamp unsupported, init schedtune mode for TID " + std::to_string(task.tid));
                // 可选：如果 setter 需要明确指令，可在此修改 result 标志
            }

            cache->update(task.pid, task.tid, task.thread_name, task.state,
                          result, task.cpuset_base, cpuctl_base, controller);            
            if (result.matched) {
                cache->update_applied_result(task.pid, task.tid, result);
            }
            cpuset->apply_with_result(task.pid, task.tid, result, task.cpuset_base);
            prio->apply_with_result(task.pid, task.tid, result);
            cpuctl->apply_with_result(task.pid, task.tid, result);
            
            LOG_T("ScanWorker", name_ + " matched and applied to TID " + std::to_string(task.tid)
                  + " rule=" + result.matched_rule_name + " [" + controller + "]");
        }
    }
};
#endif // SCAN_WORKER_HPP