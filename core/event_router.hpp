#ifndef EVENT_ROUTER_HPP
#define EVENT_ROUTER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include "../utils/logger.hpp"

class EventRouter {
public:
    using EventHandler = std::function<void(const std::set<int>& new_pids,
                                            const std::set<int>& dead_pids)>;

    explicit EventRouter(int throttle_ms = 50)
        : throttle_ms_(throttle_ms), running_(false) {}

    ~EventRouter() {
        stop();
    }

    void start(EventHandler on_events) {
        if (running_.load()) {
            LOG_W("EventRouter", "Already running");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_.clear();
            on_event_ = std::move(on_events);
        }
        running_.store(true);
        thread_ = std::thread(&EventRouter::router_loop, this);
        LOG_I("EventRouter", "Started with throttle=" + std::to_string(throttle_ms_) + "ms");
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        cv_.notify_all();

        if (thread_.joinable()) {
            thread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);
        pending_events_.clear();
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
    static constexpr size_t kMaxPendingEvents = 4096;

    int throttle_ms_;
    std::atomic<bool> running_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    EventHandler on_event_;
    std::unordered_map<int, bool> pending_events_;
    std::unordered_set<int> tracked_pids_;

    void add_event(int pid, bool is_create) {
        if (pid <= 0 || !running_.load()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = pending_events_.find(pid);
            if (it == pending_events_.end() && pending_events_.size() >= kMaxPendingEvents) {
                LOG_W("EventRouter", "Pending event limit reached, dropping PID " + std::to_string(pid));
                return;
            }
            // Keep the latest lifecycle state for each PID; never discard a DELETE due to throttling.
            pending_events_[pid] = is_create;
        }
        cv_.notify_one();
    }

    void router_loop() {
        while (running_.load()) {
            std::unordered_map<int, bool> events;
            EventHandler handler;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(throttle_ms_), [this]() {
                    return !running_.load() || !pending_events_.empty();
                });

                if (!running_.load() && pending_events_.empty()) {
                    break;
                }
                events.swap(pending_events_);
                handler = on_event_;
            }

            std::set<int> new_pids;
            std::set<int> dead_pids;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [pid, is_create] : events) {
                    if (is_create) {
                        if (tracked_pids_.insert(pid).second) {
                            new_pids.insert(pid);
                        }
                    } else {
                        tracked_pids_.erase(pid);
                        dead_pids.insert(pid);
                    }
                }
            }

            if ((!new_pids.empty() || !dead_pids.empty()) && handler) {
                handler(new_pids, dead_pids);
            }
        }
    }
};

#endif
