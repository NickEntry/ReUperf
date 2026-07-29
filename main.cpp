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
#if defined(__linux__)
    #include <fcntl.h>
    #include <unistd.h>
    #include <sys/stat.h>
    #include <sys/types.h>
    #include <sys/inotify.h>
    #include <poll.h>
#endif
#if defined(__linux__) && !defined(USE_CAPABILITY_STUB)
    #include <sys/capability.h>
#else
    // Non-Linux platforms or cross-compile without libcap: provide stub for capability functions
    #define CAP_SET 1
    #define CAP_CLEAR 0
    #define CAP_EFFECTIVE 1
    #define CAP_SYS_NICE 23
    #define CAP_SYS_ADMIN 21
    typedef int cap_flag_value_t;
    struct cap_t { void* data; };
    inline cap_t* cap_get_proc() { return nullptr; }
    inline int cap_free(cap_t*) { return 0; }
    inline int cap_get_flag(cap_t*, int, cap_flag_value_t, cap_flag_value_t*) { return 0; }
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
    explicit ScanBudget(int budget_us, int batch_size, int batch_yield_us)
        : deadline(std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us)),
          batch_size(std::max(batch_size, 1)), batch_yield_us(std::max(batch_yield_us, 0)) {}

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

void scan_and_update_rule_cache(ThreadMatcher& matcher, ThreadCache& cache,
                                std::set<int>& pinned_pids,
                                std::set<int>& topfore_pids,
                                std::set<int>& top_app_pids,
                                std::set<int>& foreground_pids,
                                std::set<int>& dead_pids) {
    const auto all_pids = FileUtils::list_pids();
    std::map<int, uint64_t> live_processes;
    std::set<std::pair<int, int>> live_threads;

    for (int pid : all_pids) {
        const uint64_t process_start_time = FileUtils::get_process_start_time(pid);
        if (process_start_time == 0) {
            continue;
        }
        const std::string proc_name = FileUtils::get_process_name_from_status(pid);
        if (proc_name == "[dead]") {
            dead_pids.insert(pid);
            continue;
        }
        live_processes.emplace(pid, process_start_time);
        for (int tid : FileUtils::list_tids(pid)) {
            live_threads.emplace(pid, tid);
        }

        const std::string cmdline = FileUtils::get_process_cmdline(pid);
        const FileUtils::CgroupState cg_state = FileUtils::get_cgroup_state(pid);
        ProcessState actual_state = ProcessState::BG;
        if (cg_state == FileUtils::CgroupState::TOP) {
            actual_state = ProcessState::TOP;
            top_app_pids.insert(pid);
        } else if (cg_state == FileUtils::CgroupState::FG) {
            actual_state = ProcessState::FG;
            foreground_pids.insert(pid);
        }

        const MatchResult result = matcher.match_process_only(
            proc_name, proc_name, actual_state, pid, cmdline);
        if (!result.matched || result.matched_rule_name == "Default rule") {
            continue;
        }
        if (result.pinned) {
            pinned_pids.insert(pid);
        }
        if (result.topfore && result.effective_state == ProcessState::TOP) {
            topfore_pids.insert(pid);
        }
    }
    cache.retain_live_threads(live_processes, live_threads);
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
        pid_it = pids.begin();
    }
    const int first_pid = *pid_it;

    do {
        const int pid = *pid_it;
        const int worker_idx = pid % static_cast<int>(workers.size());
        const std::string proc_name = FileUtils::get_process_name_from_status(pid);
        if (proc_name != "[dead]") {
            const uint64_t process_start_time = FileUtils::get_process_start_time(pid);
            if (process_start_time == 0) {
                cursor = {pid, 0};
                ++pid_it;
                if (pid_it == pids.end()) pid_it = pids.begin();
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

                DispatchTask task{pid, tid, process_start_time, thread_name, state, cpuset_base, proc_name, cmdline};
                workers[worker_idx]->enqueue(task);
                processed_tids.insert(tid);
            }
        }

        cursor = {pid, 0};
        ++pid_it;
        if (pid_it == pids.end()) {
            pid_it = pids.begin();
        }
    } while (*pid_it != first_pid);

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

void restore_all_thread_baselines(ThreadCache& cache, CpusetSetter& cpuset,
                                  PrioritySetter& prio, CpuctlSetter& cpuctl) {
    for (const auto& entry : cache.get_all_baselines()) {
        const int pid = entry.pid;
        const int tid = entry.tid;
        const ThreadBaseline& baseline = entry.baseline;
        if (FileUtils::get_process_start_time(pid) != entry.process_start_time
            || !FileUtils::is_thread_in_process(pid, tid)) {
            LOG_D("Main", "Discarding baseline for stale pid/tid identity: "
                  + std::to_string(pid) + "/" + std::to_string(tid));
            cache.erase_thread(pid, tid);
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
        }
    }
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
    
    // 权限检查：检查是否具备必要的 capabilities
    #ifdef __linux__
    {
        cap_t* caps = cap_get_proc();
        if (caps != nullptr) {
            cap_flag_value_t sys_nice = CAP_CLEAR;
            cap_flag_value_t sys_admin = CAP_CLEAR;
            cap_get_flag(caps, CAP_SYS_NICE, CAP_EFFECTIVE, &sys_nice);
            cap_get_flag(caps, CAP_SYS_ADMIN, CAP_EFFECTIVE, &sys_admin);
            cap_free(caps);
            
            if (sys_nice != CAP_SET) {
                LOG_W("Main", "Missing CAP_SYS_NICE - priority scheduling may not work");
            }
            if (sys_admin != CAP_SET) {
                LOG_W("Main", "Missing CAP_SYS_ADMIN - cgroup operations may not work");
            }
        }
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
    
    auto matcher_ptr = std::make_shared<ThreadMatcher>(config);
    auto cpuset_ptr = std::make_shared<CpusetSetter>(*matcher_ptr);
    auto prio_ptr = std::make_shared<PrioritySetter>(*matcher_ptr);
    auto cpuctl_ptr = std::make_shared<CpuctlSetter>();
    auto cache_ptr = std::make_shared<ThreadCache>();
    auto pinned_cache = std::make_unique<PinnedCache>();
    auto topfore_cache = std::make_unique<TopForeCache>();
    std::set<int> cached_top_app_pids;
    std::set<int> cached_foreground_pids;

    LOG_I("Main", "Initial scan...");
    
    // Serializes access to worker ownership and the scheduler objects they reference.
    // EventRouter invokes its callback on a separate thread while the main thread
    // may rebuild these objects during a configuration reload.
    std::mutex scheduler_state_mutex;

    std::vector<std::unique_ptr<ScanWorker>> workers;
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker1"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker2"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker3"));
    workers.push_back(std::make_unique<ScanWorker>("ScanWorker4"));
    
    for (auto& w : workers) {
        w->set_configs(matcher_ptr, cpuset_ptr, prio_ptr, cpuctl_ptr, cache_ptr);
        w->start();
    }
    
    {
        std::set<int> dead_pids;
        std::set<int> pinned_pids;
        std::set<int> topfore_pids;
        std::set<int> top_app_pids;
        std::set<int> foreground_pids;

        scan_and_update_rule_cache(*matcher_ptr, *cache_ptr, pinned_pids, topfore_pids, top_app_pids,
                                   foreground_pids, dead_pids);
        pinned_cache->update(pinned_pids);
        topfore_cache->update(topfore_pids);
        cached_top_app_pids = top_app_pids;
        cached_foreground_pids = foreground_pids;
        cleanup_dead_pids(*cache_ptr, *pinned_cache, *topfore_cache, dead_pids, pinned_pids, topfore_pids);

        std::set<int> processed;
        ScanBudget initial_budget(config.sched.full_scan_budget_us, config.sched.scan_batch_size,
                                  config.sched.scan_batch_yield_us);
        ScanCursor cursor;
        if (!dispatch_top_app_to_workers(workers, top_app_pids, processed, initial_budget, cursor)
            && !initial_budget.exhausted()
            && !dispatch_pinned_to_workers(workers, pinned_pids, processed, initial_budget, cursor)
            && !initial_budget.exhausted()
            && !dispatch_topfore_to_workers(workers, topfore_pids, processed, initial_budget, cursor)
            && !initial_budget.exhausted()) {
            dispatch_foreground_to_workers(workers, foreground_pids, processed, initial_budget, cursor);
        }
        // The initial scan is already a complete calibration; avoid repeating it in the first loop.
        full_rescan_needed.store(false, std::memory_order_release);

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    g_event_router = std::make_shared<EventRouter>(50);
    
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
        });
    };
    if (!start_proc_monitor()) {
        LOG_W("Main", "Incremental /proc monitoring unavailable; relying on periodic full scans");
        full_rescan_needed.store(true, std::memory_order_release);
    }
    auto next_monitor_restart = std::chrono::steady_clock::now() + std::chrono::seconds(1);

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
            FileUtils::invalidate_process_caches(pid);
        }
        for (int pid : new_pids) {
            if (pid > 0) {
                cache_ptr->reset_for_pid(pid);
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
                if (cg_state == FileUtils::CgroupState::TOP) {
                    actual_state = ProcessState::TOP;
                    dispatch_top_app_to_workers(workers, single_pid, processed, budget, cursor);
                } else if (cg_state == FileUtils::CgroupState::FG) {
                    actual_state = ProcessState::FG;
                }
                const MatchResult result = matcher_ptr->match_process_only(
                    proc_name, proc_name, actual_state, pid, cmdline);
                if (result.matched && result.matched_rule_name != "Default rule") {
                    if (result.pinned) {
                        dispatch_pinned_to_workers(workers, single_pid, processed, budget, cursor);
                    } else if (result.topfore && actual_state == ProcessState::FG) {
                        dispatch_topfore_to_workers(workers, single_pid, processed, budget, cursor);
                    }
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
    
    ConfigFileWatcher config_watcher(config_path);
    config_watcher.start();

    time_t last_mtime = get_file_mtime(config_path);
    uint64_t last_config_hash = compute_config_hash(config_path);
    uint64_t failed_config_hash = 0;
    int config_retry_delay_seconds = 1;
    auto next_config_retry = std::chrono::steady_clock::time_point::min();
    LOG_I("Main", "Initial config hash: " + std::to_string(last_config_hash));
    
    int highspeed_ms = std::max(config.sched.highspeed_sched_ms, 1);
    int refresh_ms = std::max(config.sched.refresh_interval_ms, highspeed_ms);
    auto next_full_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(refresh_ms);
    ScanCursor top_cursor;
    ScanCursor pinned_cursor;
    ScanCursor topfore_cursor;
    ScanCursor foreground_cursor;

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
            next_monitor_restart = monitor_now + std::chrono::seconds(5);
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
                        if (!CgroupInitializer::init(new_config)) {
                            LOG_E("Main", "Cgroup initialization failed; keeping old config");
                        } else {
                            // Build every replacement object before stopping the active scheduler.
                            auto new_matcher = std::make_shared<ThreadMatcher>(new_config);
                            auto new_cpuset = std::make_shared<CpusetSetter>(*new_matcher);
                            auto new_prio = std::make_shared<PrioritySetter>(*new_matcher);
                            auto new_cpuctl = std::make_shared<CpuctlSetter>();
                            std::vector<std::unique_ptr<ScanWorker>> new_workers;
                            for (int index = 1; index <= 4; ++index) {
                                auto worker = std::make_unique<ScanWorker>(
                                    "ScanWorker" + std::to_string(index));
                                worker->set_configs(new_matcher, new_cpuset, new_prio, new_cpuctl, cache_ptr);
                                new_workers.push_back(std::move(worker));
                            }

                            std::lock_guard<std::mutex> scheduler_lock(scheduler_state_mutex);
                            Config old_config = config;
                            auto old_matcher = matcher_ptr;
                            auto old_cpuset = cpuset_ptr;
                            auto old_prio = prio_ptr;
                            auto old_cpuctl = cpuctl_ptr;
                            auto old_workers = std::move(workers);

                            for (auto& worker : old_workers) {
                                worker->stop();
                            }
                            restore_all_thread_baselines(*cache_ptr, *old_cpuset, *old_prio, *old_cpuctl);

                            config = std::move(new_config);
                            matcher_ptr = std::move(new_matcher);
                            cpuset_ptr = std::move(new_cpuset);
                            prio_ptr = std::move(new_prio);
                            cpuctl_ptr = std::move(new_cpuctl);
                            workers = std::move(new_workers);

                            bool workers_started = true;
                            for (auto& worker : workers) {
                                if (!worker->start()) {
                                    workers_started = false;
                                    break;
                                }
                            }
                            if (!workers_started) {
                                LOG_E("Main", "Failed to start replacement workers; restoring old scheduler");
                                for (auto& worker : workers) {
                                    worker->stop();
                                }
                                workers = std::move(old_workers);
                                config = std::move(old_config);
                                matcher_ptr = std::move(old_matcher);
                                cpuset_ptr = std::move(old_cpuset);
                                prio_ptr = std::move(old_prio);
                                cpuctl_ptr = std::move(old_cpuctl);
                                for (auto& worker : workers) {
                                    worker->set_configs(matcher_ptr, cpuset_ptr, prio_ptr, cpuctl_ptr, cache_ptr);
                                    if (!worker->start()) {
                                        LOG_E("Main", "Failed to restart old worker after reload rollback");
                                    }
                                }
                            } else {
                                cache_ptr->clear_scheduling_state_preserving_baselines();
                                pinned_cache->clear();
                                topfore_cache->clear();
                                cached_top_app_pids.clear();
                                cached_foreground_pids.clear();
                                top_cursor = {};
                                pinned_cursor = {};
                                topfore_cursor = {};
                                foreground_cursor = {};
                                highspeed_ms = std::max(config.sched.highspeed_sched_ms, 1);
                                refresh_ms = std::max(config.sched.refresh_interval_ms, highspeed_ms);
                                next_full_scan = std::chrono::steady_clock::now();

                                std::string new_log_path = config.sched.log.output;
                                if (new_log_path.empty() || new_log_path == "stdout" || new_log_path == "stderr") {
                                    new_log_path = "/data/adb/ReUperf/ReUperf.log";
                                }
                                Logger::instance().init(parse_log_level(config.sched.log.level), new_log_path, true);
                                LOG_I("Main", "Config reloaded - top=" + std::to_string(highspeed_ms)
                                      + "ms, full=" + std::to_string(refresh_ms) + "ms");
                                reload_succeeded = true;
                            }
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
                config_retry_delay_seconds = 1;
                next_config_retry = std::chrono::steady_clock::time_point::min();
                full_rescan_needed.store(true, std::memory_order_release);
            } else if (current_hash != 0) {
                if (failed_config_hash == current_hash) {
                    config_retry_delay_seconds = std::min(config_retry_delay_seconds * 2, 5);
                } else {
                    failed_config_hash = current_hash;
                    config_retry_delay_seconds = 1;
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

        if (run_full_scan) {
            LOG_T("Main", "Full rescan cycle");
            std::set<int> dead_pids;
            std::set<int> pinned_pids;
            std::set<int> topfore_pids;
            std::set<int> top_app_pids;
            std::set<int> foreground_pids;

            scan_and_update_rule_cache(*matcher_ptr, *cache_ptr, pinned_pids, topfore_pids, top_app_pids,
                                       foreground_pids, dead_pids);
            pinned_cache->update(pinned_pids);
            topfore_cache->update(topfore_pids);
            cached_top_app_pids = std::move(top_app_pids);
            cached_foreground_pids = std::move(foreground_pids);
            cleanup_dead_pids(*cache_ptr, *pinned_cache, *topfore_cache, dead_pids,
                               pinned_pids, topfore_pids);

            ScanBudget budget(config.sched.full_scan_budget_us, config.sched.scan_batch_size,
                              config.sched.scan_batch_yield_us);
            if (!dispatch_top_app_to_workers(workers, cached_top_app_pids, processed, budget, top_cursor)
                && !budget.exhausted()
                && !dispatch_pinned_to_workers(workers, pinned_pids, processed, budget, pinned_cursor)
                && !budget.exhausted()
                && !dispatch_topfore_to_workers(workers, topfore_pids, processed, budget, topfore_cursor)
                && !budget.exhausted()) {
                dispatch_foreground_to_workers(workers, cached_foreground_pids, processed, budget,
                                               foreground_cursor);
            }
            // Schedule from completion so a full scan is never immediately followed by another full scan.
            next_full_scan = std::chrono::steady_clock::now() + std::chrono::milliseconds(refresh_ms);
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
    
    for (auto& w : workers) {
        w->stop();
    }
    restore_all_thread_baselines(*cache_ptr, *cpuset_ptr, *prio_ptr, *cpuctl_ptr);
    cache_ptr->clear();
    
    if (shutdown_requested.load(std::memory_order_seq_cst)) {
        LOG_I("Main", "ReUperf Thread Scheduler stopped (signal)");
    } else {
        LOG_I("Main", "ReUperf Thread Scheduler stopped");
    }
    return 0;
}
