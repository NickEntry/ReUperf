#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "config/config_parser.hpp"
#include "core/cgroup_init.hpp"
#include "core/event_router.hpp"
#include "core/restoration_retry.hpp"
#include "core/scan_worker.hpp"
#include "core/scan_cursor.hpp"
#include "core/thread_cache.hpp"
#include "core/thread_matcher.hpp"
#include "scheduler/cpuset_setter.hpp"
#include "scheduler/priority_setter.hpp"
#include "utils/cpu_mask.hpp"
#include "utils/file_utils.hpp"

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

std::filesystem::path write_config(const std::string& name, const std::string& rule_name,
                                   bool duplicate = false) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream output(path);
    output << "{\"modules\":{\"sched\":{\"rules\":[{\"name\":\"" << rule_name
           << "\",\"regex\":\"app\",\"rules\":[]}";
    if (duplicate) {
        output << ",{\"name\":\"" << rule_name
               << "\",\"regex\":\"other\",\"rules\":[]}";
    }
    output << "]}}}";
    return path;
}

void test_cgroup_and_stat_parsing() {
    using FileUtils::CgroupState;
    require(FileUtils::cgroup_path_to_state("/top-app") == CgroupState::TOP,
            "cpu top-app must map to TOP");
    require(FileUtils::cgroup_path_to_state("/foreground") == CgroupState::FG,
            "cpu foreground must map to FG");
    require(FileUtils::cgroup_path_to_state("/background") == CgroupState::BG,
            "cpu background must map to BG");
    require(FileUtils::cgroup_path_to_state("/ReUperf_0-6") == CgroupState::OTHER,
            "custom cpuset must not masquerade as Android state");
    require(FileUtils::cgroup_paths_to_state("/", "/foreground") == CgroupState::FG,
            "state-neutral cpu path must fall back to foreground cpuset");
    require(FileUtils::cgroup_paths_to_state("/uid_1000", "/top-app") == CgroupState::TOP,
            "unrecognized cpu path must fall back to top-app cpuset");
    require(FileUtils::cgroup_paths_to_state("/background", "/foreground") == CgroupState::BG,
            "recognized cpu state must remain authoritative");

    const std::string stat =
        "123 (name with ) parenthesis) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 424242 20";
    require(FileUtils::parse_start_time_from_stat(stat) == 424242,
            "stat field 22 starttime parsing failed");
    require(FileUtils::parse_start_time_from_stat("broken") == 0,
            "malformed stat must fail closed");
}

void test_config_validation() {
    const auto valid = write_config("reuperf-valid.json", "规则一");
    const auto invalid_space = write_config("reuperf-space.json", " bad");
    const auto invalid_colon = write_config("reuperf-colon.json", "bad:name");
    const auto duplicate = write_config("reuperf-duplicate.json", "same", true);

    const auto valid_result = ConfigParser::parse(valid.string());
    require(valid_result.success && valid_result.config.sched.rules.size() == 1,
            "valid UTF-8 cgroup component rejected");
    require(ConfigParser::parse(invalid_space.string()).config.sched.rules.empty(),
            "leading whitespace in cgroup component accepted");
    require(ConfigParser::parse(invalid_colon.string()).config.sched.rules.empty(),
            "colon in cgroup component accepted");
    require(ConfigParser::parse(duplicate.string()).config.sched.rules.size() == 1,
            "duplicate rule name was not rejected");

    std::filesystem::remove(valid);
    std::filesystem::remove(invalid_space);
    std::filesystem::remove(invalid_colon);
    std::filesystem::remove(duplicate);
}

void test_timing_config() {
    const auto path = std::filesystem::temp_directory_path() / "reuperf-timing.json";
    {
        std::ofstream output(path);
        output << R"({"modules":{"sched":{"timing":{)"
               << R"("event_throttle_ms":0,)"
               << R"("min_schedule_interval_ms":25,)"
               << R"("cgroup_check_interval_ms":0,)"
               << R"("cpuset_retry_count":5,)"
               << R"("cpuset_retry_interval_ms":2,)"
               << R"("process_cache_ttl_ms":20,)"
               << R"("file_cache_ttl_ms":30,)"
               << R"("cgroup_cache_ttl_ms":40,)"
               << R"("config_retry_initial_delay_s":8,)"
               << R"("config_retry_max_delay_s":3)"
               << R"(}}}})";
    }
    const auto result = ConfigParser::parse(path.string());
    require(result.success, "timing config failed to parse");
    const auto& timing = result.config.sched.timing;
    require(timing.event_throttle_ms == 0, "zero event throttle was rejected");
    require(timing.min_schedule_interval_ms == 25, "schedule interval was not parsed");
    require(timing.cgroup_check_interval_ms == 0, "zero cgroup interval was rejected");
    require(timing.cpuset_retry_count == 5 && timing.cpuset_retry_interval_ms == 2,
            "cpuset retry timing was not parsed");
    require(timing.process_cache_ttl_ms == 20 && timing.file_cache_ttl_ms == 30
                && timing.cgroup_cache_ttl_ms == 40,
            "cache TTL timing was not parsed");
    require(timing.config_retry_initial_delay_s == 8
                && timing.config_retry_max_delay_s == 8,
            "config retry maximum was not clamped to the initial delay");
    std::filesystem::remove(path);

    const auto invalid_path = std::filesystem::temp_directory_path() / "reuperf-timing-invalid.json";
    {
        std::ofstream output(invalid_path);
        output << R"({"modules":{"sched":{"timing":{"cpuset_retry_count":0,"file_cache_ttl_ms":60001}}}})";
    }
    const auto invalid = ConfigParser::parse(invalid_path.string());
    require(invalid.config.sched.timing.cpuset_retry_count == 3,
            "invalid retry count did not fall back to default");
    require(invalid.config.sched.timing.file_cache_ttl_ms == 100,
            "invalid file cache TTL did not fall back to default");
    std::filesystem::remove(invalid_path);
}

void test_cpu_mask_formatting() {
    require(CpuMask::to_string(std::vector<int>{0, 1, 2, 4, 6, 7}) == "0-2,4,6-7",
            "CPU mask range formatting failed");
    require(CpuMask::to_string(std::vector<int>{3}) == "3",
            "single CPU mask formatting failed");
}

void test_main_thread_identity() {
    Config config;
    ProcessRule process_rule;
    process_rule.name = "app";
    process_rule.regex_str = "^app$";
    process_rule.thread_rules.push_back(ThreadRule{"/MAIN_THREAD/", "main", "auto",
                                                   std::nullopt, std::nullopt, false});
    config.sched.rules.push_back(process_rule);

    ThreadMatcher matcher(config);
    const auto leader = matcher.match("app", "shared-name", ProcessState::FG,
                                      100, 100, "app");
    const auto same_name_worker = matcher.match("app", "shared-name", ProcessState::FG,
                                                100, 101, "app");
    require(leader.thread_rule_index == 0 && leader.affinity_class == "main",
            "thread-group leader did not match MAIN_THREAD");
    require(same_name_worker.thread_rule_index == -1,
            "same-name worker incorrectly matched MAIN_THREAD");
}

void test_home_package_regex_is_literal() {
    Config config;
    config.launcher_package = "com.example.home";
    ProcessRule process_rule;
    process_rule.name = "launcher";
    process_rule.regex_str = "/HOME_PACKAGE/";
    process_rule.thread_rules.push_back(ThreadRule{".", "auto", "auto", std::nullopt,
                                                   std::nullopt, false});
    config.sched.rules.push_back(process_rule);

    ThreadMatcher matcher(config);
    const auto exact = matcher.match_process_only(
        "com.example.home", "com.example.home", ProcessState::FG, 1, "com.example.home");
    const auto wildcard = matcher.match_process_only(
        "comXexampleYhome", "comXexampleYhome", ProcessState::FG, 2, "comXexampleYhome");
    require(exact.matched, "HOME_PACKAGE failed to match the exact launcher package");
    require(!wildcard.matched, "HOME_PACKAGE treated dots as regex wildcards");
}

void test_reuperf_cgroup_state_info() {
    using FileUtils::CgroupState;
    const auto owned = FileUtils::cgroup_paths_to_state_info(
        "/ReUperf/game/A1", "/ReUperf_big");
    require(owned.state == CgroupState::OTHER && owned.reuperf_owned,
            "ReUperf-owned controller paths were not identified");
    const auto external = FileUtils::cgroup_paths_to_state_info("/", "/unknown");
    require(external.state == CgroupState::OTHER && !external.reuperf_owned,
            "unrecognized external paths were treated as ReUperf-owned");
    const auto top = FileUtils::cgroup_paths_to_state_info(
        "/top-app", "/top-app/ReUperf_big");
    require(top.state == CgroupState::TOP,
            "recognized Android cpu state was hidden by a ReUperf cpuset");
    const auto background = FileUtils::cgroup_paths_to_state_info(
        "/background", "/top-app/ReUperf_big");
    require(background.state == CgroupState::BG && background.reuperf_owned,
            "top-app-nested ReUperf cpuset overrode the cpu-controller BG state");
    const auto unknown_owned = FileUtils::cgroup_paths_to_state_info(
        "/", "/top-app/ReUperf_big");
    require(unknown_owned.state == CgroupState::OTHER && unknown_owned.reuperf_owned,
            "ReUperf cpuset path incorrectly supplied a TOP fallback state");
}

void test_managed_component_tracking() {
    ThreadCache cache;
    MatchResult result;
    cache.update(10, 11, 100, 101, "worker", "proc", ProcessState::TOP,
                 result, "/dev/cpuset", "/dev/cpuctl");

    ManagedComponents managed;
    managed.affinity = true;
    managed.priority = true;
    cache.set_managed_components(10, 11, managed);

    const ManagedComponents stored = cache.get_managed_components(10, 11);
    require(stored.affinity && stored.priority && !stored.cpuctl,
            "independent managed components were not retained");
    require(!cache.get_applied_result(10, 11),
            "partial component tracking incorrectly claimed full application");
    require(should_restore_managed_component(stored.affinity, false),
            "partially applied affinity would not be restored when removed");

    cache.clear_managed_components(10, 11);
    const ManagedComponents cleared = cache.get_managed_components(10, 11);
    require(!cleared.affinity && !cleared.priority && !cleared.cpuctl,
            "managed component state was not cleared after restoration");
}

void test_thread_cache_identity() {
    ThreadCache cache;
    MatchResult result;
    result.matched = true;
    result.effective_state = ProcessState::FG;
    cache.update(10, 11, 100, 200, "thread", "cmd", ProcessState::FG,
                 result, "/dev/cpuset", "/dev/cpuctl");

    require(cache.lookup(10, 11, 100, 200, "thread", "cmd", ProcessState::FG).has_value(),
            "cache lookup rejected matching identity");
    require(!cache.lookup(10, 11, 101, 200, "thread", "cmd", ProcessState::FG).has_value(),
            "cache accepted recycled process identity");
    require(!cache.lookup(10, 11, 100, 201, "thread", "cmd", ProcessState::FG).has_value(),
            "cache accepted recycled thread identity");

    ThreadBaseline baseline;
    baseline.affinity = {0, 1};
    baseline.has_affinity = true;
    cache.set_baseline(10, 11, baseline);
    require(cache.get_baseline(10, 11).has_value(), "cache failed to retain baseline");
    cache.erase_thread_if_identity(10, 11, 100, 201);
    require(cache.get_baseline(10, 11).has_value(), "wrong identity erased a baseline");
    cache.erase_thread_if_identity(10, 11, 100, 200);
    require(!cache.get_baseline(10, 11).has_value(), "matching identity failed to erase cache entry");

    cache.update(20, 21, 300, 400, "old", "cmd", ProcessState::FG,
                 result, "/dev/cpuset", "/dev/cpuctl");
    const auto scan_start = cache.identity_snapshot();
    cache.reset_for_pid(20);
    cache.update(20, 21, 301, 401, "new", "cmd", ProcessState::TOP,
                 result, "/dev/cpuset", "/dev/cpuctl");
    ThreadBaseline new_baseline;
    new_baseline.affinity = {2};
    new_baseline.has_affinity = true;
    cache.set_baseline(20, 21, new_baseline);
    cache.retain_live_threads({{20, 300}}, {{{20, 21}, 400}}, scan_start);
    require(cache.get_baseline(20, 21).has_value(),
            "stale full-scan snapshot erased a newer recycled identity");
}

void test_priority_policy_mapping() {
    require(!PrioritySetter::expected_policy_for_priority(0).has_value(),
            "priority zero must not require a scheduler policy");
    require(PrioritySetter::expected_policy_for_priority(1) == SCHED_FIFO,
            "real-time priority must map to SCHED_FIFO");
    require(PrioritySetter::expected_policy_for_priority(120) == SCHED_OTHER,
            "nice priority must map to the normal policy");
    require(PrioritySetter::expected_policy_for_priority(-1) == SCHED_OTHER,
            "normal sentinel must map to the normal policy");
    require(!PrioritySetter::expected_policy_for_priority(99).has_value(),
            "invalid priority unexpectedly mapped to a policy");
}

void test_event_router_throttle() {
    using namespace std::chrono_literals;
    EventRouter router(60);
    std::mutex mutex;
    std::condition_variable cv;
    int callback_count = 0;
    std::set<int> received;

    router.start([&](const std::set<int>& created, const std::set<int>&,
                     const std::set<int>&, bool) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            ++callback_count;
            received.insert(created.begin(), created.end());
        }
        cv.notify_one();
    });

    router.on_process_created(1001);
    std::this_thread::sleep_for(10ms);
    router.on_process_created(1002);

    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 1s, [&] { return received.size() == 2; }),
                "EventRouter did not flush the aggregated event batch");
        require(callback_count == 1, "EventRouter did not aggregate events within throttle window");
    }
    router.stop();
}

void test_background_dispatch_eligibility() {
    MatchResult default_rule;
    default_rule.matched = true;
    default_rule.matched_rule_name = "Default rule";
    require(should_dispatch_background_process(default_rule, false),
            "a matched default rule was excluded from background dispatch");

    MatchResult unmatched;
    require(!should_dispatch_background_process(unmatched, false),
            "an unmanaged background process was dispatched without a baseline");
    require(should_dispatch_background_process(unmatched, true),
            "an unmatched process with a baseline was not dispatched for restoration");
    require(!should_dispatch_background_process(default_rule, false, true),
            "a kernel thread was dispatched through Default rule");
    require(should_dispatch_background_process(default_rule, true, true),
            "a kernel thread with an old baseline was not dispatched for cleanup");
}

void test_inherited_limit_resolution() {
    require(CgroupInitializer::resolve_limit_value(75, "100") == "75",
            "configured cgroup limit did not override the inherited value");
    require(CgroupInitializer::resolve_limit_value(std::nullopt, "100") == "100",
            "omitted cgroup limit did not reset to the inherited value");
    require(CgroupInitializer::cpuset_group_requires_rollback("0-7", "0"),
            "complete existing cpuset was not protected by rollback");
    require(!CgroupInitializer::cpuset_group_requires_rollback("0-7", ""),
            "half-initialized cpuset was treated as a valid rollback baseline");
    require(CgroupInitializer::cpuset_parent_path() == "/dev/cpuset/top-app",
            "ReUperf cpuset parent is not top-app");
    require(CgroupInitializer::cpuset_group_path("c2")
                == "/dev/cpuset/top-app/ReUperf_c2",
            "ReUperf cpuset group path was constructed incorrectly");
}

void test_resolved_affinity_detection() {
    MatchResult result;
    result.matched = true;
    result.affinity_class = "u7";
    result.cpumask_name = "c2";
    require(has_resolved_affinity(result), "resolved affinity was not detected");

    const MatchResult previous = result;
    result.cpumask_name.clear();
    require(!has_resolved_affinity(result),
            "affinity with an empty state mask was treated as active");
    require(should_restore_managed_affinity(previous, result),
            "managed affinity removal did not request baseline restoration");
    require(!should_restore_managed_affinity(std::nullopt, result),
            "missing previous result requested affinity restoration");

    MatchResult still_managed = previous;
    still_managed.cpumask_name = "c1";
    require(!should_restore_managed_affinity(previous, still_managed),
            "transition between managed masks requested baseline restoration");
    MatchResult limited = previous;
    limited.enable_limit = true;
    MatchResult unlimited = limited;
    unlimited.enable_limit = false;
    require(should_restore_managed_limit(limited, unlimited),
            "disabled cpuctl limit did not request baseline restoration");
    require(!should_restore_managed_limit(limited, limited),
            "active cpuctl limit requested baseline restoration");
    require(should_restore_managed_priority(120, 0),
            "disabled priority did not request scheduler restoration");
    require(!should_restore_managed_priority(120, 98),
            "transition between managed priorities requested restoration");
    require(!should_restore_managed_priority(0, 120),
            "priority activation requested scheduler restoration");
    require(should_restore_managed_component(true, false),
            "managed component removal was not detected");
    require(!should_restore_managed_component(false, true),
            "component activation requested baseline restoration");

    result.affinity_class = "auto";
    result.cpumask_name = "c2";
    require(!has_resolved_affinity(result), "auto affinity was treated as active");
}

void test_affinity_takeover_sequence() {
    std::vector<std::string> calls;
    int affinity_calls = 0;
    int verify_calls = 0;
    const auto result = CpusetSetter::run_takeover_sequence(
        100, 101, "c2", {7},
        [&](int tid, const std::vector<int>& cpus) {
            require(tid == 101 && cpus == std::vector<int>{7},
                    "takeover passed incorrect affinity arguments");
            calls.push_back("affinity");
            return ++affinity_calls != 1;
        },
        [&](int tid, const std::string& mask_name) {
            require(tid == 101 && mask_name == "c2",
                    "takeover passed incorrect cpuset arguments");
            calls.push_back("cpuset");
            return true;
        },
        [&](int tid) {
            require(tid == 101, "takeover passed incorrect parent cpuset arguments");
            calls.push_back("parent");
            return true;
        },
        [&](int pid, int tid, const std::string& mask_name) {
            require(pid == 100 && tid == 101 && mask_name == "c2",
                    "takeover passed incorrect cpuset verification arguments");
            calls.push_back("verify_cpuset");
            return ++verify_calls >= 2;
        },
        [&](int tid, const std::vector<int>& cpus) {
            require(tid == 101 && cpus == std::vector<int>{7},
                    "takeover passed incorrect affinity verification arguments");
            calls.push_back("verify_affinity");
            return verify_calls >= 2;
        },
        [] { return true; });

    require(result.success() && result.attempts == 2,
            "takeover did not retry after final verification failed");
    const std::vector<std::string> expected = {
        "affinity", "cpuset", "affinity", "verify_cpuset", "verify_affinity",
        "affinity", "cpuset", "affinity", "verify_cpuset", "verify_affinity"};
    require(calls == expected, "takeover operations were skipped or executed out of order");
    require(result.pre_affinity_succeeded && result.cpuset_succeeded
                && result.post_affinity_succeeded,
            "successful retry did not retain the final operation results");
}

void test_affinity_takeover_failure_does_not_short_circuit() {
    std::vector<std::string> calls;
    const auto result = CpusetSetter::run_takeover_sequence(
        1, 2, "c2", {7},
        [&](int, const std::vector<int>&) {
            calls.push_back("affinity");
            return false;
        },
        [&](int, const std::string&) {
            calls.push_back("cpuset");
            return false;
        },
        [&](int) {
            calls.push_back("parent");
            return false;
        },
        [&](int, int, const std::string&) {
            calls.push_back("verify_cpuset");
            return false;
        },
        [&](int, const std::vector<int>&) {
            calls.push_back("verify_affinity");
            return false;
        },
        [] { return true; }, 1);
    require(!result.success(), "failed takeover was reported as successful");
    const std::vector<std::string> expected = {
        "affinity", "cpuset", "affinity", "parent", "affinity", "cpuset",
        "affinity", "verify_cpuset", "verify_affinity"};
    require(calls == expected,
            "a failed takeover step suppressed or reordered enhanced recovery");
}


void test_restoration_retry() {
    int attempts = 0;
    std::vector<int> delays;
    require(run_restoration_with_retry(
                3,
                [&] { return ++attempts == 3; },
                [&](int attempt) { delays.push_back(attempt); }),
            "restoration retry did not recover on the final attempt");
    require(attempts == 3 && delays == std::vector<int>({1, 2}),
            "restoration retry used incorrect attempt or delay semantics");

    attempts = 0;
    delays.clear();
    require(!run_restoration_with_retry(
                2,
                [&] { ++attempts; return false; },
                [&](int attempt) { delays.push_back(attempt); }),
            "permanent restoration failure was reported as success");
    require(attempts == 2 && delays == std::vector<int>({1}),
            "permanent restoration failure exceeded its bound");
    require(!run_restoration_with_retry(0, [] { return true; }, [](int) {}),
            "zero restoration attempts unexpectedly ran");
}

void test_controller_requirements() {
    Config config;
    require(!CgroupInitializer::requires_cpuset(config),
            "empty configuration unexpectedly requires cpuset");
    require(!CgroupInitializer::requires_cpuctl(config),
            "empty configuration unexpectedly requires cpuctl");

    config.sched.cpumask["c0"] = {0};
    config.sched.affinity["little"] = AffinityScene{"c0", "c0", "c0"};
    ProcessRule rule;
    rule.name = "app";
    ThreadRule thread_rule;
    thread_rule.affinity_class = "little";
    rule.thread_rules.push_back(thread_rule);
    config.sched.rules.push_back(rule);
    require(CgroupInitializer::requires_cpuset(config),
            "configured affinity did not require cpuset");
    require(!CgroupInitializer::requires_cpuctl(config),
            "affinity-only configuration unexpectedly requires cpuctl");

    config.sched.rules[0].thread_rules[0].enable_limit = true;
    require(CgroupInitializer::requires_cpuctl(config),
            "configured limit did not require cpuctl");
}

void test_affinity_takeover_stops_on_identity_change() {
    std::vector<std::string> calls;
    int identity_checks = 0;
    const auto result = CpusetSetter::run_takeover_sequence(
        1, 2, "c2", {7},
        [&](int, const std::vector<int>&) {
            calls.push_back("affinity");
            return true;
        },
        [&](int, const std::string&) {
            calls.push_back("cpuset");
            return true;
        },
        [&](int) {
            calls.push_back("parent");
            return true;
        },
        [&](int, int, const std::string&) {
            calls.push_back("verify_cpuset");
            return true;
        },
        [&](int, const std::vector<int>&) {
            calls.push_back("verify_affinity");
            return true;
        },
        [&] { return ++identity_checks < 2; });
    require(!result.success() && result.attempts == 1,
            "identity change during takeover was reported as success");
    require(calls == std::vector<std::string>({"affinity"}),
            "takeover continued mutating a recycled TID");
}


void test_scan_cursor_lifecycle() {
    ScanCursor cursor;
    cursor.begin_pid(static_cast<int>(getpid()));
    ScanBudget budget(1'000'000, 32, 0);
    require(cursor.enumerate_tids(budget),
            "scan cursor failed to enumerate the current process");
    require(std::find(cursor.tids.begin(), cursor.tids.end(), static_cast<int>(getpid()))
                != cursor.tids.end(),
            "scan cursor omitted the current process leader");
    require(cursor.task_dir == nullptr && cursor.tid_enumeration_complete,
            "scan cursor retained a directory after enumeration completed");

    cursor.process_name_loaded = true;
    cursor.process_name = "cached";
    cursor.process_start_time_loaded = true;
    cursor.process_start_time = 42;
    cursor.cmdline_loaded = true;
    cursor.cmdline = "cached";
    cursor.reset();
    require(cursor.pid == 0 && cursor.task_dir == nullptr && cursor.tids.empty()
                && cursor.tid_index == 0 && !cursor.process_name_loaded
                && !cursor.process_start_time_loaded && !cursor.cmdline_loaded,
            "scan cursor reset left resumable state behind");

    cursor.begin_pid(static_cast<int>(getpid()));
    ScanBudget exhausted_budget(0, 32, 0);
    require(!cursor.enumerate_tids(exhausted_budget),
            "exhausted scan budget unexpectedly completed enumeration");
    require(cursor.task_dir != nullptr,
            "scan cursor did not preserve its directory for budgeted continuation");
    cursor.reset();
    require(cursor.task_dir == nullptr,
            "scan cursor reset leaked its resumable directory");
}

void test_scan_cadence_policy() {
    require(should_throttle_same_state_schedule(true, 199, 200),
            "same-state TOP scheduling was not throttled within the configured interval");
    require(!should_throttle_same_state_schedule(true, 200, 200),
            "same-state scheduling was throttled after the configured interval");
    require(!should_throttle_same_state_schedule(false, 0, 200),
            "state transition was incorrectly throttled");
    require(!should_throttle_same_state_schedule(true, 0, 0),
            "zero scheduling interval did not disable throttling");

    require(!should_run_managed_cgroup_check(true, false, false),
            "cpuset cgroup check bypassed its configured cadence");
    require(!should_run_managed_cgroup_check(false, true, false),
            "cpuctl cgroup check bypassed its configured cadence");
    require(should_run_managed_cgroup_check(true, false, true),
            "due cpuset cgroup check was suppressed");
    require(should_run_managed_cgroup_check(false, true, true),
            "due cpuctl cgroup check was suppressed");
    require(!should_run_managed_cgroup_check(false, false, true),
            "unmanaged task requested a cgroup drift check");
}

void test_match_result_equality() {
    MatchResult left;
    left.matched = true;
    left.affinity_class = "performance";
    left.prio_class = "normal";
    left.cpumask_name = "big";
    left.uclamp_max = 90;
    left.cpu_share = 512;
    left.enable_limit = true;
    left.effective_state = ProcessState::TOP;
    left.matched_rule_name = "app";
    left.thread_rule_index = 2;
    left.pinned = true;
    left.topfore = false;

    MatchResult right = left;
    require(is_result_equal(left, right), "equal match results compared unequal");
    right.thread_rule_index = 3;
    require(!is_result_equal(left, right), "different rule indices compared equal");
    right = left;
    right.cpu_share = 256;
    require(!is_result_equal(left, right), "different CPU shares compared equal");
}

void test_event_router_recycle() {
    using namespace std::chrono_literals;
    EventRouter router(40);
    std::mutex mutex;
    std::condition_variable cv;
    std::set<int> created;
    std::set<int> recycled;

    router.start([&](const std::set<int>& new_pids, const std::set<int>&,
                     const std::set<int>& recycled_pids, bool) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            created.insert(new_pids.begin(), new_pids.end());
            recycled.insert(recycled_pids.begin(), recycled_pids.end());
        }
        cv.notify_one();
    });

    router.on_process_exited(2001);
    router.on_process_created(2001);
    {
        std::unique_lock<std::mutex> lock(mutex);
        require(cv.wait_for(lock, 1s, [&] { return recycled.count(2001) != 0; }),
                "EventRouter failed to classify delete-create as PID recycling");
        require(created.count(2001) != 0, "recycled PID was not dispatched as newly created");
    }
    router.stop();
}

}  // namespace

int main() {
    Logger::instance().init(LogLevel::ERR, "", false);
    test_cgroup_and_stat_parsing();
    test_config_validation();
    test_timing_config();
    test_cpu_mask_formatting();
    test_main_thread_identity();
    test_home_package_regex_is_literal();
    test_reuperf_cgroup_state_info();
    test_managed_component_tracking();
    test_thread_cache_identity();
    test_background_dispatch_eligibility();
    test_inherited_limit_resolution();
    test_resolved_affinity_detection();
    test_affinity_takeover_sequence();
    test_affinity_takeover_failure_does_not_short_circuit();
    test_affinity_takeover_stops_on_identity_change();
    test_restoration_retry();
    test_controller_requirements();
    test_scan_cursor_lifecycle();
    test_scan_cadence_policy();
    test_match_result_equality();
    test_priority_policy_mapping();
    test_event_router_throttle();
    test_event_router_recycle();
    std::cout << "ReUperf tests passed\n";
    return 0;
}
