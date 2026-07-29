#ifndef EVENT_ROUTER_HPP
#define EVENT_ROUTER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
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
                                            const std::set<int>& dead_pids,
                                            const std::set<int>& recycled_pids,
                                            bool events_dropped)>;
    using HandlerFailureCallback = std::function<void()>;

    explicit EventRouter(int throttle_ms = 50)
        : throttle_ms_(throttle_ms), running_(false) {}

    ~EventRouter() {
        stop();
    }

    void start(EventHandler on_events, HandlerFailureCallback on_handler_failure = {}) {
        if (running_.load()) {
            LOG_W("EventRouter", "Already running");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_events_.clear();
            events_dropped_ = false;
            on_event_ = std::move(on_events);
            on_handler_failure_ = std::move(on_handler_failure);
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
        tracked_pids_.clear();
        on_event_ = nullptr;
        on_handler_failure_ = nullptr;
        LOG_I("EventRouter", "Stopped");
    }

    void on_process_created(int pid) {
        add_event(pid, true);
    }

    void on_process_exited(int pid) {
        add_event(pid, false);
    }

private:
    struct PendingEvent {
        bool is_create = false;
        bool saw_delete = false;
    };

    static constexpr size_t kMaxPendingEvents = 4096;

    int throttle_ms_;
    std::atomic<bool> running_;
    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    EventHandler on_event_;
    HandlerFailureCallback on_handler_failure_;
    std::unordered_map<int, PendingEvent> pending_events_;
    bool events_dropped_ = false;
    std::unordered_set<int> tracked_pids_;

    void add_event(int pid, bool is_create) {
        if (pid <= 0 || !running_.load()) {
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = pending_events_.find(pid);
            if (it == pending_events_.end()) {
                if (pending_events_.size() >= kMaxPendingEvents) {
                    if (!events_dropped_) {
                        LOG_W("EventRouter", "Pending event limit reached; requesting a compensating full scan");
                    }
                    events_dropped_ = true;
                    cv_.notify_one();
                    return;
                }
                it = pending_events_.emplace(pid, PendingEvent{}).first;
            }

            it->second.is_create = is_create;
            if (!is_create) {
                it->second.saw_delete = true;
            }
        }
        cv_.notify_one();
    }

    static void notify_handler_failure(const HandlerFailureCallback& callback) {
        if (!callback) {
            return;
        }
        try {
            callback();
        } catch (const std::exception& e) {
            LOG_E("EventRouter", "Handler-failure callback failed: " + std::string(e.what()));
        } catch (...) {
            LOG_E("EventRouter", "Handler-failure callback failed with an unknown exception");
        }
    }

    void router_loop() {
        while (running_.load()) {
            std::unordered_map<int, PendingEvent> events;
            EventHandler handler;
            HandlerFailureCallback handler_failure;
            bool events_dropped = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(throttle_ms_), [this]() {
                    return !running_.load() || !pending_events_.empty();
                });

                if (!running_.load() && pending_events_.empty()) {
                    break;
                }
                events.swap(pending_events_);
                events_dropped = events_dropped_;
                events_dropped_ = false;
                handler = on_event_;
                handler_failure = on_handler_failure_;
            }

            std::set<int> new_pids;
            std::set<int> dead_pids;
            std::set<int> recycled_pids;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const auto& [pid, event] : events) {
                    if (event.is_create) {
                        const bool newly_tracked = tracked_pids_.insert(pid).second;
                        if (event.saw_delete) {
                            recycled_pids.insert(pid);
                            new_pids.insert(pid);
                        } else if (newly_tracked) {
                            new_pids.insert(pid);
                        }
                    } else {
                        tracked_pids_.erase(pid);
                        dead_pids.insert(pid);
                    }
                }
            }

            if ((!new_pids.empty() || !dead_pids.empty() || !recycled_pids.empty() || events_dropped)
                && handler) {
                try {
                    handler(new_pids, dead_pids, recycled_pids, events_dropped);
                } catch (const std::exception& e) {
                    LOG_E("EventRouter", "Event handler failed: " + std::string(e.what())
                          + "; requesting a compensating full scan");
                    notify_handler_failure(handler_failure);
                } catch (...) {
                    LOG_E("EventRouter", "Event handler failed with an unknown exception; requesting a compensating full scan");
                    notify_handler_failure(handler_failure);
                }
            }
        }
    }
};

#endif
