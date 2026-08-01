#ifndef CPUSET_MONITOR_HPP
#define CPUSET_MONITOR_HPP

#include <atomic>
#include <cerrno>
#include <cstring>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"

class ProcMonitor {
public:
    using Callback = std::function<void(int pid)>;
    using OverflowCallback = std::function<void()>;

    ProcMonitor() : running_(false), started_(false), inotify_fd_(-1), stop_fd_(-1) {}

    ~ProcMonitor() {
        stop();
    }

    bool start(Callback on_process_change, OverflowCallback on_events_dropped = {}) {
        if (started_.load()) {
            LOG_W("ProcMonitor", "start() called while already running, ignoring");
            return true;
        }

        // A monitor thread may have terminated after an inotify/poll error. Join it
        // and release its descriptors before assigning a replacement std::thread.
        if (thread_.joinable() || inotify_fd_ >= 0 || stop_fd_ >= 0) {
            stop();
        }

        on_process_change_ = std::move(on_process_change);
        on_events_dropped_ = std::move(on_events_dropped);
        inotify_fd_ = inotify_init1(IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            LOG_W("ProcMonitor", "inotify_init1 failed: " + std::string(strerror(errno)));
            return false;
        }

        const int watch = inotify_add_watch(inotify_fd_, "/proc", IN_CREATE | IN_DELETE | IN_ISDIR);
        if (watch < 0) {
            LOG_W("ProcMonitor", "Failed to watch /proc: " + std::string(strerror(errno)));
            close(inotify_fd_);
            inotify_fd_ = -1;
            return false;
        }

        stop_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (stop_fd_ < 0) {
            LOG_W("ProcMonitor", "eventfd failed: " + std::string(strerror(errno)));
            close(inotify_fd_);
            inotify_fd_ = -1;
            return false;
        }

        running_.store(true);
        started_.store(true);
        try {
            thread_ = std::thread(&ProcMonitor::monitor_loop, this);
        } catch (...) {
            running_.store(false);
            started_.store(false);
            close(inotify_fd_);
            close(stop_fd_);
            inotify_fd_ = -1;
            stop_fd_ = -1;
            on_process_change_ = nullptr;
            throw;
        }
#ifdef __linux__
        const int ret = pthread_setname_np(thread_.native_handle(), "ProcMonitor");
        if (ret != 0) {
            LOG_W("ProcMonitor", "Failed to set thread name: " + std::string(strerror(ret)));
        }
#endif
        LOG_I("ProcMonitor", "Started monitoring /proc changes");
        return true;
    }

    void stop() {
        const bool was_started = started_.exchange(false);
        const bool was_running = running_.exchange(false);
        if (was_running && stop_fd_ >= 0) {
            const uint64_t signal = 1;
            if (write(stop_fd_, &signal, sizeof(signal)) < 0 && errno != EAGAIN) {
                LOG_W("ProcMonitor", "Failed to signal monitor stop: " + std::string(strerror(errno)));
            }
        }
        if (thread_.joinable()) {
            thread_.join();
        }
        if (inotify_fd_ >= 0) {
            close(inotify_fd_);
            inotify_fd_ = -1;
        }
        if (stop_fd_ >= 0) {
            close(stop_fd_);
            stop_fd_ = -1;
        }
        if (was_started) {
            LOG_I("ProcMonitor", "Stopped");
        }
    }

    bool is_running() const {
        return started_.load();
    }

private:
    std::atomic<bool> running_;
    std::atomic<bool> started_;
    int inotify_fd_;
    int stop_fd_;
    std::thread thread_;
    Callback on_process_change_;
    OverflowCallback on_events_dropped_;

    void monitor_loop() {
        constexpr size_t kEventSize = sizeof(inotify_event);
        constexpr size_t kBufferSize = 4096 * (kEventSize + 16);
        alignas(inotify_event) char buffer[kBufferSize];
        pollfd poll_fds[2] = {
            {inotify_fd_, POLLIN, 0},
            {stop_fd_, POLLIN, 0},
        };

        while (running_.load()) {
            const int ret = poll(poll_fds, 2, -1);
            if (ret < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_E("ProcMonitor", "poll error: " + std::string(strerror(errno)));
                break;
            }
            if (poll_fds[1].revents & POLLIN) {
                uint64_t ignored = 0;
                (void)read(stop_fd_, &ignored, sizeof(ignored));
                break;
            }
            if (!(poll_fds[0].revents & POLLIN)) {
                if (poll_fds[0].revents != 0) {
                    LOG_E("ProcMonitor", "inotify poll error");
                    break;
                }
                continue;
            }

            const ssize_t length = read(inotify_fd_, buffer, sizeof(buffer));
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    continue;
                }
                LOG_E("ProcMonitor", "read error: " + std::string(strerror(errno)));
                break;
            }

            for (ssize_t offset = 0; offset + static_cast<ssize_t>(kEventSize) <= length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(&buffer[offset]);
                const ssize_t record_size = static_cast<ssize_t>(kEventSize) + event->len;
                if (record_size > length - offset) {
                    LOG_W("ProcMonitor", "Truncated inotify event record");
                    break;
                }
                if (event->mask & IN_Q_OVERFLOW) {
                    LOG_W("ProcMonitor", "inotify queue overflow; requesting a compensating full scan");
                    if (on_events_dropped_) on_events_dropped_();
                } else if (event->len > 0) {
                    const std::string name(event->name, strnlen(event->name, event->len));
                    if (FileUtils::is_all_digits(name.c_str())) {
                        errno = 0;
                        char* end = nullptr;
                        const long parsed_pid = strtol(name.c_str(), &end, 10);
                        if (errno == 0 && end != name.c_str() && *end == '\0'
                            && FileUtils::is_valid_pid(parsed_pid) && on_process_change_) {
                            const int pid = static_cast<int>(parsed_pid);
                            if (event->mask & IN_CREATE) {
                                on_process_change_(pid);
                            } else if (event->mask & IN_DELETE) {
                                on_process_change_(-pid);
                            }
                        }
                    }
                }
                offset += record_size;
            }
        }
        running_.store(false);
        started_.store(false);
    }
};

#endif
