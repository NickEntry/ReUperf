#ifndef EVENT_ROUTER_HPP
#define EVENT_ROUTER_HPP

#include <string>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include <unordered_map>
#include <atomic>
#include <thread>
#include <chrono>
#include "../utils/logger.hpp"

struct ProcEvent {
    int pid;
    bool is_create;
    uint64_t timestamp_ms;
};

class EventRouter {
public:
    using EventHandler = std::function<void(const std::set<int>& new_pids, 
                                            const std::set<int>& dead_pids)>;
    
    explicit EventRouter(int throttle_ms = 50) 
        : throttle_ms_(throttle_ms), running_(false), last_event_time_(0) {}
    
    ~EventRouter() {
        stop();
    }
    
    void start(EventHandler on_events) {
        if (running_.load()) {
            LOG_W("EventRouter", "Already running");
            return;
        }
        
        on_event_ = on_events;
        running_ = true;
        
        thread_ = std::thread(&EventRouter::router_loop, this);
        LOG_I("EventRouter", "Started with throttle=" + std::to_string(throttle_ms_) + "ms");
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        cv_.notify_all();
        
        if (thread_.joinable()) {
            thread_.join();
        }
        
        LOG_I("EventRouter", "Stopped");
    }
    
    void on_process_created(int pid) {
        add_event(pid, true);
    }
    
    void on_process_exited(int pid) {
        add_event(pid, false);
    }
    
    std::unordered_set<int> get_tracked_pids() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracked_pids_;
    }
    
    bool is_tracked(int pid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tracked_pids_.count(pid) > 0;
    }
    
    void remove_tracked(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        tracked_pids_.erase(pid);
    }

private:
    int throttle_ms_;
    std::atomic<bool> running_;
    std::thread thread_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    EventHandler on_event_;
    
    std::queue<ProcEvent> event_queue_;
    std::unordered_set<int> tracked_pids_;
    std::unordered_map<int, uint64_t> last_event_time_;
    
    uint64_t get_timestamp_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count();
    }
    
    void add_event(int pid, bool is_create) {
        if (pid <= 0) return;
        
        uint64_t now = get_timestamp_ms();
        
        {
            std::lock_guard<std::mutex> lock(mutex_);
            
            auto it = last_event_time_.find(pid);
            if (it != last_event_time_.end()) {
                uint64_t elapsed = now - it->second;
                if (elapsed < static_cast<uint64_t>(throttle_ms_)) {
                    return;
                }
            }
            
            last_event_time_[pid] = now;
            event_queue_.push({pid, is_create, now});
        }
        
        cv_.notify_one();
    }
    
    void router_loop() {
        while (running_.load()) {
            std::set<int> new_pids;
            std::set<int> dead_pids;
            
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(throttle_ms_), [this]() {
                    return !running_.load() || !event_queue_.empty();
                });
                
                while (!event_queue_.empty()) {
                    ProcEvent event = event_queue_.front();
                    event_queue_.pop();
                    
                    if (event.is_create) {
                        if (tracked_pids_.insert(event.pid).second) {
                            new_pids.insert(event.pid);
                        }
                    } else {
                        if (tracked_pids_.erase(event.pid) > 0) {
                            dead_pids.insert(event.pid);
                        }
                    }
                }
            }
            
            if (!new_pids.empty() || !dead_pids.empty()) {
                if (on_event_) {
                    on_event_(new_pids, dead_pids);
                }
            }
        }
    }
};

#endif
