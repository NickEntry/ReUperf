#include <iostream>
#include <chrono>
#include <cstdlib>
#include <limits.h>
#include <thread>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <map>
#include <set>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <cerrno>
#include <dirent.h>
#if defined(__linux__)
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/inotify.h>
    #include <poll.h>
#endif
#include <memory>
#include <mutex>
#include <ctime>
#include <regex>

#include "config/config_parser.hpp"
#include "config/config_types.hpp"
#include "utils/logger.hpp"
#include "utils/file_utils.hpp"
#include "core/cgroup_init.hpp"
#include "core/launcher_finder.hpp"
#include "core/cpuset_monitor.hpp"
#include "core/event_router.hpp"
#include "core/thread_matcher.hpp"
#include "core/thread_cache.hpp"
#include "core/scan_worker.hpp"
#include "scheduler/cpuset_setter.hpp"
#include "scheduler/priority_setter.hpp"
#include "scheduler/cpuctl_setter.hpp"

namespace {
    volatile std::sig_atomic_t signal_requested = 0;
    std::atomic<bool> running(true);
    std::atomic<bool> full_rescan_needed(true);
    std::atomic<bool> shutdown_requested(false);
    std::shared_ptr<EventRouter> g_event_router;
}

void signal_handler(int sig) {
    (void)sig;
    signal_requested = 1;
}

void ensure_data_dir() {
    std::string path = "/data/adb/ReUperf";
    if (!FileUtils::mkdir_recursive(path)) {
        LOG_E("Main", "Failed to create data dir: " + path);
    }
    (void)FileUtils::dir_exists(path);
}

time_t get_file_mtime(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        return st.st_mtime;
    }
    return 0;
}

// Simple hash for config change detection (non-cryptographic)
uint64_t compute_config_hash(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return 0;
    
    uint64_t hash = 1469598103934665603ULL; // FNV-1a offset basis
    char buf[4096];
    while (ifs.read(buf, sizeof(buf)) || ifs.gcount() > 0) {
        std::streamsize len = ifs.gcount();
        for (std::streamsize i = 0; i < len; ++i) {
            hash ^= static_cast<unsigned char>(buf[i]);
            hash *= 1099511628211ULL; // FNV-1a prime
        }
    }
    return hash;
}

class ConfigFileWatcher {
public:
    // The watcher reports observed writes; the main loop combines this signal
    // with mtime/hash detection and updates its baseline after a successful reload.
    explicit ConfigFileWatcher(const std::string& config_path)
        : config_path_(config_path), inotify_fd_(-1), watch_fd_(-1), config_changed_(false) {
        const size_t dir_pos = config_path_.rfind('/');
        watch_dir_ = dir_pos == std::string::npos ? "." : config_path_.substr(0, dir_pos);
        watch_basename_ = dir_pos == std::string::npos ? config_path_ : config_path_.substr(dir_pos + 1);
    }

    ~ConfigFileWatcher() {
        stop();
    }

    bool start() {
#ifdef __linux__
        stop();
        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            LOG_W("ConfigFileWatcher", "inotify_init1 failed: " + std::string(strerror(errno)));
            return false;
        }
        if (!add_watch()) {
            close(inotify_fd_);
            inotify_fd_ = -1;
            return false;
        }
        LOG_I("ConfigFileWatcher", "Watching config: " + config_path_);
        return true;
#else
        return false;
#endif
    }

    void stop() {
#ifdef __linux__
        if (inotify_fd_ >= 0) {
            close(inotify_fd_);
            inotify_fd_ = -1;
        }
        watch_fd_ = -1;
#endif
    }

    bool check_and_clear() {
#ifdef __linux__
        if (inotify_fd_ < 0 && !start()) {
            return false;
        }

        bool rewatch_needed = false;
        alignas(inotify_event) char buffer[4096];
        while (true) {
            const ssize_t length = read(inotify_fd_, buffer, sizeof(buffer));
            if (length < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                LOG_W("ConfigFileWatcher", "inotify read failed: " + std::string(strerror(errno)));
                rewatch_needed = true;
                break;
            }
            if (length == 0) {
                rewatch_needed = true;
                break;
            }

            for (ssize_t offset = 0;
                 offset + static_cast<ssize_t>(sizeof(inotify_event)) <= length;) {
                const auto* event = reinterpret_cast<const inotify_event*>(&buffer[offset]);
                const ssize_t record_size = static_cast<ssize_t>(sizeof(inotify_event)) + event->len;
                if (record_size > length - offset) {
                    LOG_W("ConfigFileWatcher", "Truncated inotify event record");
                    rewatch_needed = true;
                    break;
                }

                if (event->mask & IN_Q_OVERFLOW) {
                    LOG_W("ConfigFileWatcher", "inotify queue overflow; requesting config reload");
                    config_changed_.store(true, std::memory_order_release);
                }
                if (event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) {
                    rewatch_needed = true;
                }
                if (event->len > 0
                    && watch_basename_ == std::string(event->name, strnlen(event->name, event->len))
                    && (event->mask & (IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB))) {
                    LOG_D("ConfigFileWatcher", "Config file changed");
                    config_changed_.store(true, std::memory_order_release);
                }
                offset += record_size;
            }
        }

        if (rewatch_needed) {
            LOG_W("ConfigFileWatcher", "Recreating config directory watch");
            stop();
            (void)start();
        }
        return config_changed_.exchange(false, std::memory_order_acq_rel);
#else
        return false;
#endif
    }

private:
#ifdef __linux__
    bool add_watch() {
        constexpr uint32_t kWatchMask = IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_ATTRIB
            | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_Q_OVERFLOW;
        watch_fd_ = inotify_add_watch(inotify_fd_, watch_dir_.c_str(), kWatchMask);
        if (watch_fd_ < 0) {
            LOG_W("ConfigFileWatcher", "inotify_add_watch failed: " + std::string(strerror(errno)));
            return false;
        }
        return true;
    }
#endif

    std::string config_path_;
    std::string watch_dir_;
    std::string watch_basename_;
    int inotify_fd_;
    int watch_fd_;
    std::atomic<bool> config_changed_;
};

class PidCache {
public:
    void update(const std::set<int>& pids) {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_ = pids;
    }

    std::set<int> get() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pids_;
    }

    void remove_pid(int pid) {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_.erase(pid);
    }

    bool has_pid(int pid) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pids_.count(pid) > 0;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        pids_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::set<int> pids_;
};

using PinnedCache = PidCache;
using TopForeCache = PidCache;

struct ScanBudget {
    explicit ScanBudget(int budget_us, int configured_batch_size,
                        int configured_batch_yield_us)
        : deadline(std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us)),
          batch_size(std::max(configured_batch_size, 1)),
          batch_yield_us(std::max(configured_batch_yield_us, 0)) {}

    bool exhausted() const {
        return std::chrono::steady_clock::now() >= deadline;
    }

    void after_thread_read() {
        ++thread_reads;
        if (batch_yield_us > 0 && thread_reads % static_cast<size_t>(batch_size) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(batch_yield_us));
        }
    }

    std::chrono::steady_clock::time_point deadline;
    int batch_size;
    int batch_yield_us;
    size_t thread_reads = 0;
};

struct ScanCursor {
    int pid = 0;
    int tid = 0;
};

struct FullScanState {
    bool active = false;
    DIR* proc_dir = nullptr;
    bool pid_enumeration_complete = false;
    std::vector<int> pids;
    size_t pid_index = 0;
    int current_pid = 0;
    uint64_t current_process_start_time = 0;
    std::string current_proc_name;
    std::string current_cmdline;
    ProcessState current_state = ProcessState::BG;
    DIR* task_dir = nullptr;
    bool tid_enumeration_complete = false;
    std::vector<int> current_tids;
    size_t tid_index = 0;
    std::set<int> baseline_pids;
    std::map<int, uint64_t> live_processes;
    std::map<std::pair<int, int>, uint64_t> live_threads;
    std::map<std::pair<int, int>, std::pair<uint64_t, uint64_t>> scan_start_identities;
    std::map<int, std::pair<uint64_t, ProcessState>> previous_states;
    std::map<int, std::pair<uint64_t, ProcessState>> observed_states;
    std::set<int> pinned_pids;
    std::set<int> topfore_pids;
    std::set<int> top_app_pids;
    std::set<int> foreground_pids;
    std::set<int> background_pids;
    std::set<int> dead_pids;

    FullScanState() = default;
    FullScanState(const FullScanState&) = delete;
    FullScanState& operator=(const FullScanState&) = delete;

    ~FullScanState() {
        reset();
    }

    void begin(ThreadCache& cache,
               const std::map<int, std::pair<uint64_t, ProcessState>>& prior_states) {
        reset();
        active = true;
        baseline_pids = cache.get_pids_with_baselines();
        scan_start_identities = cache.identity_snapshot();
        previous_states = prior_states;
    }

    bool enumerate_pids(ScanBudget& budget) {
        if (pid_enumeration_complete) return true;
        if (proc_dir == nullptr) {
            proc_dir = opendir("/proc");
            if (proc_dir == nullptr) {
                LOG_W("Main", "Failed to open /proc for full scan: "
                      + std::string(strerror(errno)));
                return false;
            }
            pids.clear();
        }

        while (!budget.exhausted()) {
            errno = 0;
            const dirent* entry = readdir(proc_dir);
            if (entry == nullptr) {
                const int read_error = errno;
                closedir(proc_dir);
                proc_dir = nullptr;
                if (read_error != 0) {
                    LOG_W("Main", "Failed while enumerating /proc: "
                          + std::string(strerror(read_error)));
                    pids.clear();
                    return false;
                }
                std::sort(pids.begin(), pids.end());
                pids.erase(std::unique(pids.begin(), pids.end()), pids.end());
                pid_enumeration_complete = true;
                return true;
            }

            budget.after_thread_read();
            if (!FileUtils::is_all_digits(entry->d_name)) continue;
            errno = 0;
            char* end = nullptr;
            const long parsed_pid = strtol(entry->d_name, &end, 10);
            if (errno == 0 && end != entry->d_name && *end == '\0'
                && FileUtils::is_valid_pid(parsed_pid)) {
                pids.push_back(static_cast<int>(parsed_pid));
            }
        }
        return false;
    }

    bool enumerate_current_tids(ScanBudget& budget) {
        if (tid_enumeration_complete) return true;
        if (task_dir == nullptr) {
            const std::string task_path = "/proc/" + std::to_string(current_pid) + "/task";
            task_dir = opendir(task_path.c_str());
            if (task_dir == nullptr) {
                tid_enumeration_complete = true;
                return true;
            }
            current_tids.clear();
        }

        while (!budget.exhausted()) {
            errno = 0;
            const dirent* entry = readdir(task_dir);
            if (entry == nullptr) {
                const int read_error = errno;
                closedir(task_dir);
                task_dir = nullptr;
                if (read_error != 0) {
                    LOG_W("Main", "Failed while enumerating tasks for pid "
                          + std::to_string(current_pid) + ": "
                          + std::string(strerror(read_error)));
                    current_tids.clear();
                }
                std::sort(current_tids.begin(), current_tids.end());
                current_tids.erase(std::unique(current_tids.begin(), current_tids.end()),
                                   current_tids.end());
                tid_enumeration_complete = true;
                return true;
            }

            budget.after_thread_read();
            if (!FileUtils::is_all_digits(entry->d_name)) continue;
            errno = 0;
            char* end = nullptr;
            const long parsed_tid = strtol(entry->d_name, &end, 10);
            if (errno == 0 && end != entry->d_name && *end == '\0'
                && FileUtils::is_valid_tid(parsed_tid)) {
                current_tids.push_back(static_cast<int>(parsed_tid));
            }
        }
        return false;
    }

    void clear_current_pid() {
        if (task_dir != nullptr) {
            closedir(task_dir);
            task_dir = nullptr;
        }
        tid_enumeration_complete = false;
        current_pid = 0;
        current_process_start_time = 0;
        current_proc_name.clear();
        current_cmdline.clear();
        current_state = ProcessState::BG;
        current_tids.clear();
        tid_index = 0;
    }

    void reset() {
        if (proc_dir != nullptr) {
            closedir(proc_dir);
            proc_dir = nullptr;
        }
        active = false;
        pid_enumeration_complete = false;
        pids.clear();
        pid_index = 0;
        clear_current_pid();
        baseline_pids.clear();
        live_processes.clear();
        live_threads.clear();
        scan_start_identities.clear();
        previous_states.clear();
        observed_states.clear();
        pinned_pids.clear();
        topfore_pids.clear();
        top_app_pids.clear();
        foreground_pids.clear();
        background_pids.clear();
        dead_pids.clear();
    }
};

bool advance_full_scan(ThreadMatcher& matcher, ThreadCache& cache,
                       FullScanState& scan, ScanBudget& budget) {
    if (!scan.active) {
        return false;
    }
    if (!scan.enumerate_pids(budget)) {
        return false;
    }

    while (scan.pid_index < scan.pids.size()) {
        if (budget.exhausted()) return false;

        const int pid = scan.pids[scan.pid_index];
        if (scan.current_pid != pid) {
            scan.clear_current_pid();
            scan.current_pid = pid;
            scan.current_process_start_time = FileUtils::get_process_start_time(pid);
            budget.after_thread_read();
            if (scan.current_process_start_time == 0) {
                ++scan.pid_index;
                scan.clear_current_pid();
                continue;
            }

            scan.current_proc_name = FileUtils::get_process_name_from_status(pid);
            if (scan.current_proc_name == "[dead]") {
                scan.dead_pids.insert(pid);
                ++scan.pid_index;
                scan.clear_current_pid();
                continue;
            }

            scan.live_processes[pid] = scan.current_process_start_time;
            scan.current_cmdline = FileUtils::get_process_cmdline(pid);
            const FileUtils::CgroupStateInfo cgroup = FileUtils::get_cgroup_state_info(pid);
            if (cgroup.state == FileUtils::CgroupState::TOP) {
                scan.current_state = ProcessState::TOP;
            } else if (cgroup.state == FileUtils::CgroupState::FG) {
                scan.current_state = ProcessState::FG;
            } else if (cgroup.state == FileUtils::CgroupState::BG) {
                scan.current_state = ProcessState::BG;
            } else if (const auto previous = scan.previous_states.find(pid);
                       cgroup.reuperf_owned && previous != scan.previous_states.end()
                       && previous->second.first == scan.current_process_start_time) {
                // ReUperf may replace both visible controller paths. Preserve only the
                // state of the same process identity while those paths remain ours.
                scan.current_state = previous->second.second;
            }
            scan.observed_states[pid] = {scan.current_process_start_time,
                                         scan.current_state};
            if (scan.current_state == ProcessState::TOP) {
                scan.top_app_pids.insert(pid);
            } else if (scan.current_state == ProcessState::FG) {
                scan.foreground_pids.insert(pid);
            }
        }

        if (!scan.enumerate_current_tids(budget)) return false;
        while (scan.tid_index < scan.current_tids.size()) {
            if (budget.exhausted()) return false;
            const int tid = scan.current_tids[scan.tid_index++];
            const uint64_t thread_start_time = FileUtils::get_thread_start_time(pid, tid);
            budget.after_thread_read();
            if (thread_start_time != 0) {
                scan.live_threads.emplace(std::make_pair(pid, tid), thread_start_time);
            }
        }

        if (budget.exhausted()) return false;
        const uint64_t verified_start_time = FileUtils::get_process_start_time(pid);
        if (verified_start_time != scan.current_process_start_time) {
            scan.live_processes.erase(pid);
            for (auto it = scan.live_threads.begin(); it != scan.live_threads.end();) {
                if (it->first.first == pid) it = scan.live_threads.erase(it);
                else ++it;
            }
            scan.top_app_pids.erase(pid);
            scan.foreground_pids.erase(pid);
            scan.background_pids.erase(pid);
            scan.observed_states.erase(pid);
            scan.pinned_pids.erase(pid);
            scan.topfore_pids.erase(pid);
            scan.clear_current_pid();
            continue;
        }
        const MatchResult result = matcher.match_process_only(
            scan.current_proc_name, scan.current_proc_name, scan.current_state,
            pid, scan.current_cmdline);
        if (scan.current_state == ProcessState::BG
            && should_dispatch_background_process(
                result, scan.baseline_pids.count(pid) > 0)) {
            scan.background_pids.insert(pid);
        }
        if (result.matched) {
            if (result.pinned) {
                scan.pinned_pids.insert(pid);
            }
            if (result.topfore && result.effective_state == ProcessState::TOP) {
                scan.topfore_pids.insert(pid);
            }
        }

        ++scan.pid_index;
        scan.clear_current_pid();
    }

    cache.retain_live_threads(scan.live_processes, scan.live_threads,
                              scan.scan_start_identities);
    return true;
}

inline bool dispatch_pids_to_workers(
    std::vector<std::unique_ptr<ScanWorker>>& workers,
    const std::set<int>& pids, ProcessState state, const std::string& cpuset_base,
    std::set<int>& processed_tids, ScanBudget& budget, ScanCursor& cursor) {
    if (workers.empty() || pids.empty()) {
        return false;
    }

    auto pid_it = cursor.pid > 0 ? pids.lower_bound(cursor.pid) : pids.begin();
    if (pid_it == pids.end()) {
        cursor = {0, 0};
        return false;
    }

    while (pid_it != pids.end()) {
        const int pid = *pid_it;
        const std::string proc_name = FileUtils::get_process_name_from_status(pid);
        if (proc_name != "[dead]") {
            const uint64_t process_start_time = FileUtils::get_process_start_time(pid);
            if (process_start_time == 0) {
                cursor = {pid, 0};
                ++pid_it;
                continue;
            }
            const std::string cmdline = FileUtils::get_process_cmdline(pid);
            const auto tids = FileUtils::list_tids(pid);
            const int resume_after_tid = cursor.pid == pid ? cursor.tid : 0;
            for (int tid : tids) {
                if (tid <= resume_after_tid || processed_tids.count(tid) > 0) {
                    continue;
                }
                if (budget.exhausted()) {
                    cursor = {pid, tid - 1};
                    return true;
                }

                // Do not pre-stat the thread path: opendir/open is authoritative and avoids
                // an extra syscall plus a TOCTOU race when a thread exits concurrently.
                const std::string thread_name = FileUtils::get_thread_name(pid, tid);
                budget.after_thread_read();
                if (thread_name == "[dead]") {
                    continue;
                }

                const uint64_t thread_start_time = FileUtils::get_thread_start_time(pid, tid);
                if (thread_start_time == 0) {
                    continue;
                }
                DispatchTask task{pid, tid, process_start_time, thread_start_time, thread_name,
                                  state, cpuset_base, proc_name, cmdline};
                size_t worker_hash = std::hash<int>{}(pid);
                worker_hash ^= std::hash<int>{}(tid) + 0x9e3779b9U
                    + (worker_hash << 6) + (worker_hash >> 2);
                worker_hash ^= std::hash<uint64_t>{}(process_start_time) + 0x9e3779b9U
                    + (worker_hash << 6) + (worker_hash >> 2);
                worker_hash ^= std::hash<uint64_t>{}(thread_start_time) + 0x9e3779b9U
                    + (worker_hash << 6) + (worker_hash >> 2);
                const size_t worker_idx = worker_hash % workers.size();
                const ScanWorker::EnqueueResult enqueue_result = workers[worker_idx]->enqueue(task);
                if (enqueue_result == ScanWorker::EnqueueResult::QueueFull
                    || enqueue_result == ScanWorker::EnqueueResult::Stopped) {
                    full_rescan_needed.store(true, std::memory_order_release);
                    cursor = {pid, tid - 1};
                    return true;
                }
                processed_tids.insert(tid);
            }
        }

        cursor = {pid, 0};
        ++pid_it;
    }

    cursor = {0, 0};
    return false;
}

inline bool dispatch_pinned_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                       const std::set<int>& pids, std::set<int>& processed_tids,
                                       ScanBudget& budget, ScanCursor& cursor) {
    static const std::string CPUSET_BASE = "/dev/cpuset";
    return dispatch_pids_to_workers(workers, pids, ProcessState::TOP, CPUSET_BASE,
                                    processed_tids, budget, cursor);
}

inline bool dispatch_topfore_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                        const std::set<int>& pids, std::set<int>& processed_tids,
                                        ScanBudget& budget, ScanCursor& cursor) {
    static const std::string CPUSET_BASE = "/dev/cpuset";
    return dispatch_pids_to_workers(workers, pids, ProcessState::TOP, CPUSET_BASE,
                                    processed_tids, budget, cursor);
}

inline bool dispatch_top_app_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                        const std::set<int>& pids, std::set<int>& processed_tids,
                                        ScanBudget& budget, ScanCursor& cursor) {
    static const std::string CPUSET_BASE = "/dev/cpuset";
    return dispatch_pids_to_workers(workers, pids, ProcessState::TOP, CPUSET_BASE,
                                    processed_tids, budget, cursor);
}

inline bool dispatch_foreground_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                           const std::set<int>& pids, std::set<int>& processed_tids,
                                           ScanBudget& budget, ScanCursor& cursor) {
    static const std::string CPUSET_BASE = "/dev/cpuset";
    return dispatch_pids_to_workers(workers, pids, ProcessState::FG, CPUSET_BASE,
                                    processed_tids, budget, cursor);
}

inline bool dispatch_background_to_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                                           const std::set<int>& pids, std::set<int>& processed_tids,
                                           ScanBudget& budget, ScanCursor& cursor) {
    static const std::string CPUSET_BASE = "/dev/cpuset";
    return dispatch_pids_to_workers(workers, pids, ProcessState::BG, CPUSET_BASE,
                                    processed_tids, budget, cursor);
}

void cleanup_dead_pids(ThreadCache& cache, PinnedCache& pinned_cache,
                       TopForeCache& topfore_cache, std::set<int>& dead_pids,
                       std::set<int>& pinned_pids, std::set<int>& topfore_pids) {
    for (int pid : dead_pids) {
        cache.reset_for_pid(pid);
        pinned_cache.remove_pid(pid);
        topfore_cache.remove_pid(pid);
        pinned_pids.erase(pid);
        topfore_pids.erase(pid);
    }
}

bool restore_all_thread_baselines(ThreadCache& cache, CpusetSetter& cpuset,
                                  PrioritySetter& prio, CpuctlSetter& cpuctl) {
    bool all_restored = true;
    for (const auto& entry : cache.get_all_baselines()) {
        const int pid = entry.pid;
        const int tid = entry.tid;
        const ThreadBaseline& baseline = entry.baseline;
        if (FileUtils::get_process_start_time(pid) != entry.process_start_time
            || FileUtils::get_thread_start_time(pid, tid) != entry.thread_start_time
            || !FileUtils::is_thread_in_process(pid, tid)) {
            LOG_D("Main", "Discarding baseline for stale pid/tid identity: "
                  + std::to_string(pid) + "/" + std::to_string(tid));
            cache.erase_thread_if_identity(pid, tid, entry.process_start_time,
                                           entry.thread_start_time);
            continue;
        }

        bool restored = true;
        if (baseline.has_cpuctl_path && !cpuctl.restore_cpuctl_cgroup(tid, baseline.cpuctl_path)) {
            LOG_W("Main", "Failed to restore cpuctl cgroup for tid " + std::to_string(tid));
            restored = false;
        }
        if (baseline.has_scheduler && !prio.restore_scheduler(
                tid, baseline.scheduler_policy, baseline.scheduler_priority, baseline.nice)) {
            LOG_W("Main", "Failed to restore scheduler for tid " + std::to_string(tid));
            restored = false;
        }
        if (baseline.has_cpuset_path && !cpuset.restore_cpuset_cgroup(tid, baseline.cpuset_path)) {
            LOG_W("Main", "Failed to restore cpuset cgroup for tid " + std::to_string(tid));
            restored = false;
        }
        if (baseline.has_affinity && !cpuset.restore_affinity(tid, baseline.affinity)) {
            LOG_W("Main", "Failed to restore affinity for tid " + std::to_string(tid));
            restored = false;
        }
        if (restored) {
            cache.clear_baseline(pid, tid);
            cache.clear_applied_result(pid, tid);
        } else {
            all_restored = false;
        }
    }
    return all_restored;
}

void stop_all_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                      const std::string& context) noexcept {
    for (auto& worker : workers) {
        try {
            worker->stop();
        } catch (const std::exception& e) {
            LOG_E("Main", context + ": worker stop failed: " + e.what());
        } catch (...) {
            LOG_E("Main", context + ": worker stop failed with an unknown exception");
        }
    }
}

bool start_all_workers(std::vector<std::unique_ptr<ScanWorker>>& workers,
                       const std::string& context) {
    try {
        for (auto& worker : workers) {
            if (!worker->start()) {
                LOG_E("Main", context + ": worker start returned false");
                stop_all_workers(workers, context);
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        LOG_E("Main", context + ": worker start failed: " + e.what());
    } catch (...) {
        LOG_E("Main", context + ": worker start failed with an unknown exception");
    }
    stop_all_workers(workers, context);
    return false;
}

bool resolve_allowed_config_path(const std::string& user_path, const std::string& allowed_dir,
                                 std::string& resolved_path) {
#if defined(__linux__)
    char resolved_dir[PATH_MAX];
    char resolved_file[PATH_MAX];
    if (realpath(allowed_dir.c_str(), resolved_dir) == nullptr
        || realpath(user_path.c_str(), resolved_file) == nullptr) {
        return false;
    }

    const std::string directory(resolved_dir);
    const std::string file(resolved_file);
    const std::string directory_prefix = directory + "/";
    if (file.compare(0, directory_prefix.size(), directory_prefix) != 0) {
        return false;
    }

    struct stat st {};
    if (stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    resolved_path = file;
    return true;
#else
    (void)user_path;
    (void)allowed_dir;
    (void)resolved_path;
    return false;
#endif
}

int main(int argc, char* argv[]) {
    std::string config_path = "/data/adb/ReUperf/ReUperf.json";
    const std::string allowed_dir = "/data/adb/ReUperf";
    
    if (argc > 1 && argv[1] != nullptr) {
        if (!resolve_allowed_config_path(argv[1], allowed_dir, config_path)) {
            std::cerr << "Security: Config path must resolve to a regular file under "
                      << allowed_dir << std::endl;
            return 1;
        }
    }
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
    
#if defined(__linux__)
    if (geteuid() != 0) {
        LOG_W("Main", "Not running as root; scheduler and cgroup operations may fail");
    }
#endif
    
    ensure_data_dir();
    
    const ConfigParseResult initial_parse = ConfigParser::parse(config_path);
    if (!initial_parse.success) {
        std::cerr << "Failed to parse config: " << config_path << std::endl;
        return 1;
    }
    Config config = initial_parse.config;
    
    std::string log_path = config.sched.log.output;
    if (log_path.empty() || log_path == "stdout" || log_path == "stderr") {
        log_path = "/data/adb/ReUperf/ReUperf.log";
    }
    
    LogLevel config_level = parse_log_level(config.sched.log.level);
    Logger::instance().init(config_level, log_path, true);
    
    LOG_I("Main", "ReUperf Thread Scheduler starting...");
    LOG_I("Main", "Config path: " + config_path);
    
    if (!config.sched.enable) {
        LOG_I("Main", "Sched module disabled, exiting");
        return 0;
    }
    
    LOG_I("Main", "Log level set to: " + config.sched.log.level);
    
    config.launcher_package = LauncherFinder::find();
    LOG_I("Main", "Launcher package: " + config.launcher_package);
    
    if (!CgroupInitializer::init(config)) {
        LOG_E("Main", "Failed to initialize cgroups");
        return 1;
    }
    
    FileUtils::set_cache_ttls(config.sched.timing.file_cache_ttl_ms,
                              config.sched.timing.cgroup_cache_ttl_ms);
    auto matcher_ptr = std::make_shared<ThreadMatcher>(config);
    auto cpuset_ptr = std::make_shared<CpusetSetter>(*matcher_ptr, config.sched.timing);
    auto prio_ptr = std::make_shared<PrioritySetter>(*matcher_ptr);
    auto cpuctl_ptr = std::make_shared<CpuctlSetter>();
    auto cache_ptr = std::make_shared<ThreadCache>();
    auto pinned_cache = std::make_unique<PinnedCache>();
    auto topfore_cache = std::make_unique<TopForeCache>();
    std::set<int> cached_top_app_pids;
    std::set<int> cached_foreground_pids;
    std::set<int> cached_background_pids;
    std::map<int, std::pair<uint64_t, ProcessState>> cached_process_states;

    LOG_I("Main", "Initial scan...");
    
    // Serializes access to worker ownership and the scheduler objects they reference.
    // EventRouter invokes its callback on a separate thread while the main thread
    // may rebuild these objects during a configuration reload.
    std::mutex scheduler_state_mutex;

    std::vector<std::unique_ptr<ScanWorker>> workers;
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker1", config.sched.timing));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker2", config.sched.timing));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker3", config.sched.timing));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker4", config.sched.timing));
    
    for (auto& worker : workers) {
        worker->set_configs(matcher_ptr, cpuset_ptr, prio_ptr, cpuctl_ptr, cache_ptr);
    }
    if (!start_all_workers(workers, "Starting initial workers")) {
        LOG_E("Main", "Failed to start scheduler workers");
        return 1;
    }
    
    // The initial scan uses the same resumable, budgeted state machine as periodic scans.
    full_rescan_needed.store(true, std::memory_order_release);

    
    g_event_router = std::make_shared<EventRouter>(config.sched.timing.event_throttle_ms);
    
    g_event_router->start([&](const std::set<int>& new_pids, const std::set<int>& dead_pids,
                              const std::set<int>& recycled_pids, bool events_dropped) {
        if (events_dropped) {
            LOG_W("Main", "Process events were dropped; requesting a compensating full scan");
            full_rescan_needed.store(true, std::memory_order_release);
        }
        // The callback runs on EventRouter's thread. Hold the same lock used by
        // configuration reload before accessing workers or scheduler objects.
        std::lock_guard<std::mutex> lock(scheduler_state_mutex);
        for (int pid : recycled_pids) {
            LOG_I("Main", "PID recycled, clearing cached thread state: " + std::to_string(pid));
            cache_ptr->reset_for_pid(pid);
            cached_process_states.erase(pid);
            FileUtils::invalidate_process_caches(pid);
        }
        for (int pid : new_pids) {
            if (pid > 0) {
                cache_ptr->reset_for_pid(pid);
                cached_process_states.erase(pid);
                FileUtils::invalidate_process_caches(pid);
                LOG_I("Main", "Incremental dispatch for new pid: " + std::to_string(pid));
                const std::set<int> single_pid = {pid};
                std::set<int> processed;
                ScanBudget budget(config.sched.top_scan_budget_us, config.sched.scan_batch_size,
                                  config.sched.scan_batch_yield_us);
                ScanCursor cursor;

                // Only inspect the affected PID. A process event must not trigger a global /proc scan.
                const std::string proc_name = FileUtils::get_process_name_from_status(pid);
                if (proc_name == "[dead]") continue;
                const std::string cmdline = FileUtils::get_process_cmdline(pid);
                const FileUtils::CgroupState cg_state = FileUtils::get_cgroup_state(pid);
                ProcessState actual_state = ProcessState::BG;
                bool initial_dispatch_incomplete = false;
                if (cg_state == FileUtils::CgroupState::TOP) {
                    actual_state = ProcessState::TOP;
                    initial_dispatch_incomplete = dispatch_top_app_to_workers(
                        workers, single_pid, processed, budget, cursor);
                } else if (cg_state == FileUtils::CgroupState::FG) {
                    actual_state = ProcessState::FG;
                }
                const MatchResult result = matcher_ptr->match_process_only(
                    proc_name, proc_name, actual_state, pid, cmdline);
                bool rule_dispatch_incomplete = false;
                if (result.matched) {
                    if (result.pinned) {
                        rule_dispatch_incomplete = dispatch_pinned_to_workers(
                            workers, single_pid, processed, budget, cursor);
                    } else if (result.topfore && actual_state == ProcessState::FG) {
                        rule_dispatch_incomplete = dispatch_topfore_to_workers(
                            workers, single_pid, processed, budget, cursor);
                    } else if (actual_state == ProcessState::FG) {
                        rule_dispatch_incomplete = dispatch_foreground_to_workers(
                            workers, single_pid, processed, budget, cursor);
                    } else if (actual_state == ProcessState::BG) {
                        rule_dispatch_incomplete = dispatch_background_to_workers(
                            workers, single_pid, processed, budget, cursor);
                    }
                }
                if (initial_dispatch_incomplete || rule_dispatch_incomplete) {
                    full_rescan_needed.store(true, std::memory_order_release);
                }
            }
        }
        for (int pid : dead_pids) {
            LOG_I("Main", "Cleaning up dead pid: " + std::to_string(pid));
            cache_ptr->reset_for_pid(pid);
            FileUtils::invalidate_process_caches(pid);
        }
    }, []() {
        full_rescan_needed.store(true, std::memory_order_release);
    });

    ProcMonitor monitor;
    const auto start_proc_monitor = [&]() {
        return monitor.start([&](int pid) {
            if (pid > 0) {
                LOG_D("Main", "Process created: " + std::to_string(pid));
                g_event_router->on_process_created(pid);
            } else if (pid < 0) {
                LOG_D("Main", "Process exited: " + std::to_string(-pid));
                g_event_router->on_process_exited(-pid);
            }
        }, []() {
            full_rescan_needed.store(true, std::memory_order_release);
        });
    };
    if (!start_proc_monitor()) {
        LOG_W("Main", "Incremental /proc monitoring unavailable; relying on periodic full scans");
    }
    // Cover the unavoidable gap before the /proc watch becomes active.
    full_rescan_needed.store(true, std::memory_order_release);
    auto next_monitor_restart = std::chrono::steady_clock::now()
        + std::chrono::seconds(config.sched.timing.monitor_initial_restart_delay_s);

    
    ConfigFileWatcher config_watcher(config_path);
    config_watcher.start();

    time_t last_mtime = get_file_mtime(config_path);
    uint64_t last_config_hash = compute_config_hash(config_path);
    uint64_t failed_config_hash = 0;
    int config_retry_delay_seconds = config.sched.timing.config_retry_initial_delay_s;
    auto next_config_retry = std::chrono::steady_clock::time_point::min();
    LOG_I("Main", "Initial config hash: " + std::to_string(last_config_hash));
    
    int highspeed_ms = std::max(config.sched.highspeed_sched_ms, 1);
    int refresh_ms = std::max(config.sched.refresh_interval_ms, highspeed_ms);
    auto next_full_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(refresh_ms);
    ScanCursor top_cursor;
    ScanCursor pinned_cursor;
    ScanCursor topfore_cursor;
    ScanCursor foreground_cursor;
    ScanCursor background_cursor;
    int full_scan_phase = 0;
    bool full_cycle_active = false;
    FullScanState full_scan_state;
    std::set<int> full_cycle_processed_tids;

    LOG_I("Main", "Scheduler: top=" + std::to_string(highspeed_ms)
          + "ms, full=" + std::to_string(refresh_ms)
          + "ms, top_budget=" + std::to_string(config.sched.top_scan_budget_us)
          + "us, full_budget=" + std::to_string(config.sched.full_scan_budget_us) + "us");
    
    while (running.load(std::memory_order_acquire)) {
        if (signal_requested != 0) {
            shutdown_requested.store(true, std::memory_order_release);
            running.store(false, std::memory_order_release);
            continue;
        }

        try {
        const auto monitor_now = std::chrono::steady_clock::now();
        if (!monitor.is_running() && monitor_now >= next_monitor_restart) {
            LOG_W("Main", "Incremental /proc monitor stopped; attempting restart");
            if (!start_proc_monitor()) {
                LOG_W("Main", "Incremental /proc monitor restart failed; continuing with periodic full scans");
            }
            full_rescan_needed.store(true, std::memory_order_release);
            next_monitor_restart = monitor_now
                + std::chrono::seconds(config.sched.timing.monitor_restart_retry_delay_s);
        }

        const bool config_changed_inotify = config_watcher.check_and_clear();
        const time_t current_mtime = get_file_mtime(config_path);
        const uint64_t current_hash = compute_config_hash(config_path);
        const bool config_changed_fallback =
            (current_mtime != last_mtime && current_mtime > 0)
            || (current_hash != last_config_hash && current_hash != 0);
        const auto config_check_now = std::chrono::steady_clock::now();
        const bool retry_deferred = current_hash != 0 && current_hash == failed_config_hash
            && config_check_now < next_config_retry;

        if ((config_changed_inotify || config_changed_fallback) && !retry_deferred) {
            LOG_I("Main", "Config file changed, reloading...");
            bool reload_succeeded = false;
            try {
                ConfigParseResult parsed_config = ConfigParser::parse(config_path);
                if (!parsed_config.success) {
                    LOG_E("Main", "Config reload failed, keeping old config");
                } else {
                    Config new_config = std::move(parsed_config.config);
                    if (!new_config.sched.enable) {
                        LOG_I("Main", "New config has sched disabled, exiting");
                        running.store(false, std::memory_order_release);
                        reload_succeeded = true;
                    } else {
                        new_config.launcher_package = LauncherFinder::find();
                        auto new_matcher = std::make_shared<ThreadMatcher>(new_config);
                        auto new_cpuset = std::make_shared<CpusetSetter>(
                            *new_matcher, new_config.sched.timing);
                        auto new_prio = std::make_shared<PrioritySetter>(*new_matcher);
                        auto new_cpuctl = std::make_shared<CpuctlSetter>();
                        std::vector<std::unique_ptr<ScanWorker>> new_workers;
                        for (int index = 1; index <= 4; ++index) {
                            auto worker = std::make_unique<ScanWorker>(
                                "ScanWorker" + std::to_string(index), new_config.sched.timing);
                            worker->set_configs(new_matcher, new_cpuset, new_prio,
                                                new_cpuctl, cache_ptr);
                            new_workers.push_back(std::move(worker));
                        }

                        std::lock_guard<std::mutex> scheduler_lock(scheduler_state_mutex);
                        auto old_workers = std::move(workers);
                        stop_all_workers(old_workers, "Stopping old workers for reload");

                        bool baselines_restored = false;
                        bool new_cgroups_ready = false;
                        bool new_workers_started = false;
                        try {
                            baselines_restored = restore_all_thread_baselines(
                                *cache_ptr, *cpuset_ptr, *prio_ptr, *cpuctl_ptr);
                            if (!baselines_restored) {
                                LOG_E("Main", "Baseline restoration failed; keeping old config");
                            } else {
                                new_cgroups_ready = CgroupInitializer::init(new_config);
                                if (!new_cgroups_ready) {
                                    LOG_E("Main", "Cgroup initialization failed; keeping old config");
                                } else {
                                    new_workers_started = start_all_workers(
                                        new_workers, "Starting replacement workers");
                                }
                            }
                        } catch (const std::exception& e) {
                            LOG_E("Main", "Replacement scheduler preparation failed: "
                                  + std::string(e.what()) + "; restoring old scheduler");
                        } catch (...) {
                            LOG_E("Main", "Replacement scheduler preparation failed with an unknown exception; restoring old scheduler");
                        }

                        if (!baselines_restored || !new_cgroups_ready || !new_workers_started) {
                            workers = std::move(old_workers);
                            bool old_cgroups_ready = !baselines_restored;
                            bool old_workers_started = false;
                            try {
                                if (!old_cgroups_ready) {
                                    old_cgroups_ready = CgroupInitializer::init(config);
                                }
                                if (old_cgroups_ready) {
                                    old_workers_started = start_all_workers(
                                        workers, "Restarting old workers");
                                }
                            } catch (const std::exception& e) {
                                LOG_E("Main", "Old scheduler recovery failed: "
                                      + std::string(e.what()));
                            } catch (...) {
                                LOG_E("Main", "Old scheduler recovery failed with an unknown exception");
                            }
                            if (!old_cgroups_ready) {
                                LOG_E("Main", "Failed to restore old cgroup configuration; stopping scheduler");
                                running.store(false, std::memory_order_release);
                            } else if (!old_workers_started) {
                                LOG_E("Main", "Old scheduler could not be fully restarted; stopping scheduler");
                                running.store(false, std::memory_order_release);
                            }
                            full_rescan_needed.store(true, std::memory_order_release);
                            next_full_scan = std::chrono::steady_clock::now();
                        } else {
                            workers = std::move(new_workers);
                            config = std::move(new_config);
                            matcher_ptr = std::move(new_matcher);
                            cpuset_ptr = std::move(new_cpuset);
                            prio_ptr = std::move(new_prio);
                            cpuctl_ptr = std::move(new_cpuctl);
                            cache_ptr->clear_scheduling_state_preserving_baselines();
                            pinned_cache->clear();
                            topfore_cache->clear();
                            cached_top_app_pids.clear();
                            cached_foreground_pids.clear();
                            cached_background_pids.clear();
                            cached_process_states.clear();
                            top_cursor = {};
                            pinned_cursor = {};
                            topfore_cursor = {};
                            foreground_cursor = {};
                            background_cursor = {};
                            full_scan_phase = 0;
                            full_cycle_active = false;
                            full_scan_state.reset();
                            full_cycle_processed_tids.clear();
                            highspeed_ms = std::max(config.sched.highspeed_sched_ms, 1);
                            refresh_ms = std::max(config.sched.refresh_interval_ms, highspeed_ms);
                            next_full_scan = std::chrono::steady_clock::now();
                            FileUtils::set_cache_ttls(config.sched.timing.file_cache_ttl_ms,
                                                      config.sched.timing.cgroup_cache_ttl_ms);
                            g_event_router->set_throttle_ms(
                                config.sched.timing.event_throttle_ms);
                            config_retry_delay_seconds =
                                config.sched.timing.config_retry_initial_delay_s;

                            std::string new_log_path = config.sched.log.output;
                            if (new_log_path.empty() || new_log_path == "stdout"
                                || new_log_path == "stderr") {
                                new_log_path = "/data/adb/ReUperf/ReUperf.log";
                            }
                            Logger::instance().init(parse_log_level(config.sched.log.level),
                                                    new_log_path, true);
                            LOG_I("Main", "Config reloaded - top="
                                  + std::to_string(highspeed_ms)
                                  + "ms, full=" + std::to_string(refresh_ms) + "ms");
                            reload_succeeded = true;
                        }
                    }
                }
            } catch (const std::exception& e) {
                LOG_E("Main", "Config reload failed: " + std::string(e.what())
                      + ", keeping old config");
            } catch (...) {
                LOG_E("Main", "Config reload failed with an unknown exception, keeping old config");
            }

            if (reload_succeeded) {
                last_mtime = current_mtime;
                last_config_hash = current_hash;
                failed_config_hash = 0;
                config_retry_delay_seconds = config.sched.timing.config_retry_initial_delay_s;
                next_config_retry = std::chrono::steady_clock::time_point::min();
                full_rescan_needed.store(true, std::memory_order_release);
            } else if (current_hash != 0) {
                if (failed_config_hash == current_hash) {
                    config_retry_delay_seconds = std::min(
                        config_retry_delay_seconds * 2,
                        config.sched.timing.config_retry_max_delay_s);
                } else {
                    failed_config_hash = current_hash;
                    config_retry_delay_seconds = config.sched.timing.config_retry_initial_delay_s;
                }
                next_config_retry = config_check_now
                    + std::chrono::seconds(config_retry_delay_seconds);
                LOG_W("Main", "Deferring retry for unchanged failed config by "
                      + std::to_string(config_retry_delay_seconds) + "s");
            }
        }
        const auto now = std::chrono::steady_clock::now();
        const bool run_full_scan = full_rescan_needed.exchange(false, std::memory_order_acq_rel)
            || now >= next_full_scan;
        std::set<int> processed;
        {
        std::lock_guard<std::mutex> scheduler_lock(scheduler_state_mutex);

        if (run_full_scan) {
            LOG_T("Main", "Full rescan cycle");
            ScanBudget budget(config.sched.full_scan_budget_us, config.sched.scan_batch_size,
                              config.sched.scan_batch_yield_us);
            if (!full_cycle_active) {
                full_cycle_active = true;
                full_scan_phase = 0;
                top_cursor = {};
                pinned_cursor = {};
                topfore_cursor = {};
                foreground_cursor = {};
                background_cursor = {};
                full_scan_state.begin(*cache_ptr, cached_process_states);
                full_cycle_processed_tids.clear();
            }

            if (full_scan_state.active) {
                if (!advance_full_scan(*matcher_ptr, *cache_ptr, full_scan_state, budget)) {
                    full_rescan_needed.store(true, std::memory_order_release);
                } else {
                    pinned_cache->update(full_scan_state.pinned_pids);
                    topfore_cache->update(full_scan_state.topfore_pids);
                    cached_top_app_pids = full_scan_state.top_app_pids;
                    cached_foreground_pids = full_scan_state.foreground_pids;
                    cached_background_pids = full_scan_state.background_pids;
                    cached_process_states = full_scan_state.observed_states;
                    cleanup_dead_pids(*cache_ptr, *pinned_cache, *topfore_cache,
                                      full_scan_state.dead_pids, full_scan_state.pinned_pids,
                                      full_scan_state.topfore_pids);
                    full_scan_state.active = false;
                }
            }

            bool scan_incomplete = false;
            if (!full_scan_state.active) {
                while (!scan_incomplete && !budget.exhausted() && full_scan_phase < 5) {
                    switch (full_scan_phase) {
                        case 0:
                            scan_incomplete = dispatch_top_app_to_workers(
                                workers, cached_top_app_pids, full_cycle_processed_tids, budget, top_cursor);
                            break;
                        case 1:
                            scan_incomplete = dispatch_pinned_to_workers(
                                workers, pinned_cache->get(), full_cycle_processed_tids, budget, pinned_cursor);
                            break;
                        case 2:
                            scan_incomplete = dispatch_topfore_to_workers(
                                workers, topfore_cache->get(), full_cycle_processed_tids, budget, topfore_cursor);
                            break;
                        case 3:
                            scan_incomplete = dispatch_foreground_to_workers(
                                workers, cached_foreground_pids, full_cycle_processed_tids, budget, foreground_cursor);
                            break;
                        case 4:
                            scan_incomplete = dispatch_background_to_workers(
                                workers, cached_background_pids, full_cycle_processed_tids, budget, background_cursor);
                            break;
                        default:
                            break;
                    }
                    if (!scan_incomplete) ++full_scan_phase;
                }
            }

            if (full_scan_state.active || scan_incomplete || budget.exhausted() || full_scan_phase < 5) {
                full_rescan_needed.store(true, std::memory_order_release);
            } else {
                full_cycle_active = false;
                full_scan_phase = 0;
                full_scan_state.reset();
                full_cycle_processed_tids.clear();
                next_full_scan = std::chrono::steady_clock::now()
                    + std::chrono::milliseconds(refresh_ms);
            }
        } else {
            // High-speed path intentionally touches only top-app and rules promoted to TOP.
            ScanBudget budget(config.sched.top_scan_budget_us, config.sched.scan_batch_size,
                              config.sched.scan_batch_yield_us);
            if (!dispatch_top_app_to_workers(workers, cached_top_app_pids, processed, budget, top_cursor)
                && !budget.exhausted()
                && !dispatch_pinned_to_workers(workers, pinned_cache->get(), processed, budget, pinned_cursor)
                && !budget.exhausted()) {
                dispatch_topfore_to_workers(workers, topfore_cache->get(), processed, budget, topfore_cursor);
            }
        }
        }

        const int current_sleep_ms = highspeed_ms;

        std::this_thread::sleep_for(std::chrono::milliseconds(current_sleep_ms));
        } catch (const std::exception& e) {
            LOG_E("Main", "Exception in main loop: " + std::string(e.what()));
        } catch (...) {
            LOG_E("Main", "Unknown exception in main loop");
        }
    }
    
    monitor.stop();
    g_event_router->stop();
    
    stop_all_workers(workers, "Stopping workers for shutdown");
    restore_all_thread_baselines(*cache_ptr, *cpuset_ptr, *prio_ptr, *cpuctl_ptr);
    cache_ptr->clear();
    
    if (shutdown_requested.load(std::memory_order_seq_cst)) {
        LOG_I("Main", "ReUperf Thread Scheduler stopped (signal)");
    } else {
        LOG_I("Main", "ReUperf Thread Scheduler stopped");
    }
    return 0;
}
