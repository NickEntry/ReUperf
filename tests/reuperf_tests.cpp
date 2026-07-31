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
#include "core/event_router.hpp"
#include "core/thread_cache.hpp"
#include "core/thread_matcher.hpp"
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
    test_home_package_regex_is_literal();
    test_thread_cache_identity();
    test_match_result_equality();
    test_priority_policy_mapping();
    test_event_router_throttle();
    test_event_router_recycle();
    std::cout << "ReUperf tests passed\n";
    return 0;
}
