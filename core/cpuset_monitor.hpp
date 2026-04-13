#ifndef CPUSET_MONITOR_HPP
#define CPUSET_MONITOR_HPP

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <functional>
#include <map>
#include <unordered_set>
#include <sstream>
#include <cerrno>
#include <fcntl.h>
#include <sys/inotify.h>
#include <unistd.h>
#include <pthread.h>
#include <poll.h>
#include "../utils/logger.hpp"
#include "../utils/file_utils.hpp"

class ProcMonitor {
public:
    using Callback = std::function<void(int pid)>;
    
    ProcMonitor() : running_(false), started_(false), inotify_fd_(-1) {}
    
    ~ProcMonitor() {
        stop();
    }
    
    bool start(std::function<void(int pid)> on_process_change) {
        if (started_.load()) {
            LOG_W("ProcMonitor", "start() called while already running, ignoring");
            return true;
        }
        
        on_process_change_ = on_process_change;
        
        inotify_fd_ = inotify_init();
        if (inotify_fd_ < 0) {
            LOG_W("ProcMonitor", "inotify_init failed: " + std::string(strerror(errno)));
            return false;
        }

        int wd = inotify_add_watch(inotify_fd_, "/proc", IN_CREATE | IN_DELETE | IN_ISDIR);
        if (wd < 0) {
            LOG_W("ProcMonitor", "Failed to watch /proc: " + std::string(strerror(errno)));
            close(inotify_fd_);
            inotify_fd_ = -1;
            return false;
        }
        watch_fds_["/proc"] = wd;
        LOG_D("ProcMonitor", "Watching: /proc");
        
        running_ = true;
        thread_ = std::thread(&ProcMonitor::monitor_loop, this);
        
        #ifdef __linux__
        int ret = pthread_setname_np(thread_.native_handle(), "ProcMonitor");
        if (ret != 0) {
            LOG_W("ProcMonitor", "Failed to set thread name: " + std::string(strerror(ret)));
        }
        #endif
        
        LOG_I("ProcMonitor", "Started monitoring /proc changes");
        started_ = true;
        return true;
    }
    
    void stop() {
        if (!running_.load()) return;
        
        running_.store(false);
        
        if (inotify_fd_ >= 0) {
            close(inotify_fd_);
            inotify_fd_ = -1;
        }
        
        if (thread_.joinable()) {
            thread_.join();
        }
        
        started_.store(false);
        LOG_I("ProcMonitor", "Stopped");
    }
    
    bool is_running() const { return started_.load(); }

private:
    std::atomic<bool> running_;
    std::atomic<bool> started_;
    int inotify_fd_;
    std::thread thread_;
    std::function<void(int pid)> on_process_change_;
    
    std::map<std::string, int> watch_fds_;
    std::unordered_set<int> tracked_pids_;
    
    void monitor_loop() {
        constexpr size_t EVENT_SIZE = sizeof(struct inotify_event);
        constexpr size_t BUF_LEN = 4096 * (EVENT_SIZE + 16);
        char buf[BUF_LEN];
        struct pollfd pfd = {inotify_fd_, POLLIN, 0};

        while (running_.load()) {
            int ret = poll(&pfd, 1, 100);
            if (ret < 0) {
                if (errno == EINTR) continue;
                LOG_E("ProcMonitor", "poll error: " + std::string(strerror(errno)));
                break;
            }
            if (ret == 0) {
                continue;
            }

            ssize_t len = read(inotify_fd_, buf, BUF_LEN);
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }
                LOG_E("ProcMonitor", "read error: " + std::string(strerror(errno)));
                break;
            }
            
            ssize_t i = 0;
            while (i < len) {
                struct inotify_event* event = reinterpret_cast<struct inotify_event*>(&buf[i]);
                
                if (event->len > 0) {
                    std::string name(event->name, event->len);
                    
                    if (event->mask & IN_CREATE) {
                        if (name.find_first_not_of("0123456789") == std::string::npos) {
                            int pid = std::stoi(name);
                            if (pid > 0) {
                                LOG_D("ProcMonitor", "Process created: " + name);
                                if (on_process_change_) {
                                    on_process_change_(pid);
                                }
                            }
                        }
                    } else if (event->mask & IN_DELETE) {
                        if (name.find_first_not_of("0123456789") == std::string::npos) {
                            int pid = std::stoi(name);
                            if (pid > 0) {
                                LOG_D("ProcMonitor", "Process deleted: " + name);
                                if (on_process_change_) {
                                    on_process_change_(-pid);
                                }
                            }
                        }
                    }
                }
                
                i += EVENT_SIZE + event->len;
            }
        }
    }
};

#endif
