#ifndef THREAD_MATCHER_HPP
#define THREAD_MATCHER_HPP

#include <algorithm>
#include <string>
#include <regex>
#include <vector>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <mutex>
#include <limits>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"

struct MatchResult {
    bool matched = false;
    std::string affinity_class;
    std::string prio_class;
    std::string cpumask_name;       // resolved cpumask name for cpuset cgroup path
    std::optional<int> uclamp_max;
    std::optional<int> cpu_share;
    bool enable_limit = false;
    ProcessState effective_state;
    std::string matched_rule_name;
    int thread_rule_index = -1;     // index into ProcessRule::thread_rules (0-based), -1 = no match
    bool pinned = false;
    bool topfore = false;
};

inline bool has_resolved_affinity(const MatchResult& result) {
    return !result.affinity_class.empty() && result.affinity_class != "auto"
        && !result.cpumask_name.empty();
}

inline bool should_restore_managed_component(bool previously_managed,
                                             bool currently_managed) {
    return previously_managed && !currently_managed;
}

inline bool should_restore_managed_affinity(const std::optional<MatchResult>& previous,
                                            const MatchResult& current) {
    return previous && should_restore_managed_component(
        has_resolved_affinity(*previous), has_resolved_affinity(current));
}

inline bool should_restore_managed_priority(int previous_priority,
                                            int current_priority) {
    return should_restore_managed_component(
        previous_priority != 0, current_priority != 0);
}

inline bool should_restore_managed_limit(const std::optional<MatchResult>& previous,
                                         const MatchResult& current) {
    return previous && should_restore_managed_component(
        previous->enable_limit, current.enable_limit);
}

inline bool is_result_equal(const MatchResult& a, const MatchResult& b) {
    return a.matched == b.matched &&
           a.affinity_class == b.affinity_class &&
           a.prio_class == b.prio_class &&
           a.cpumask_name == b.cpumask_name &&
           a.effective_state == b.effective_state &&
           a.enable_limit == b.enable_limit &&
           a.uclamp_max == b.uclamp_max &&
           a.cpu_share == b.cpu_share &&
           a.thread_rule_index == b.thread_rule_index &&
           a.matched_rule_name == b.matched_rule_name &&
           a.pinned == b.pinned &&
           a.topfore == b.topfore;
}

struct CompiledThreadRule {
    ThreadRule rule;
    std::regex pattern;
    size_t source_rule_index = 0;
    bool is_main_thread_rule = false;
};

struct CompiledProcessRule {
    ProcessRule rule;
    std::regex pattern;  // 匹配进程 cmdline 或进程名
    std::vector<CompiledThreadRule> thread_rules;
};

// Regex patterns are user-maintained, trusted configuration. Deliberately do not
// add ReDoS syntax restrictions or other defensive filtering; users retain full
// regex control. This limit only rejects anomalously large configuration entries.
static constexpr size_t kMaxRegexLength = 500;

class ThreadMatcher {
public:
    explicit ThreadMatcher(const Config& config) : config_(config), launcher_package_(config.launcher_package) {
        auto regex_flags = std::regex::ECMAScript;
        if (config_.sched.case_insensitive) regex_flags |= std::regex::icase;

        for (const auto& rule : config_.sched.rules) {
            CompiledProcessRule cr;
            cr.rule = rule;

            RegexPattern process_regex = parse_regex_pattern(rule.regex_str);
            std::string expanded = expand_process_regex(process_regex.pattern,
                launcher_package_, "");
            
            // Keep only the configured maximum entry size; ReDoS filtering is intentionally omitted.
            if (expanded.length() > kMaxRegexLength) {
                LOG_W("ThreadMatcher", "Skip regex exceeding " + std::to_string(kMaxRegexLength) 
                      + " chars: " + rule.regex_str);
                continue;
            }
            
            try {
                cr.pattern = std::regex(expanded, regex_flags_for(process_regex, regex_flags));
            } catch (const std::regex_error&) {
                LOG_W("ThreadMatcher", "Skip invalid process regex: " + rule.regex_str);
                continue;
            }

            for (size_t source_rule_index = 0; source_rule_index < rule.thread_rules.size(); ++source_rule_index) {
                const auto& tr = rule.thread_rules[source_rule_index];
                CompiledThreadRule ctr;
                ctr.rule = tr;
                ctr.source_rule_index = source_rule_index;
                RegexPattern thread_regex = parse_regex_pattern(tr.keyword);

                if (thread_regex.pattern == "/MAIN_THREAD/") {
                    ctr.pattern = std::regex("^$");
                    ctr.is_main_thread_rule = true;
                } else {
                    std::string tk = expand_thread_regex(thread_regex.pattern, "");
                    
                    // Keep only the configured maximum entry size; ReDoS filtering is intentionally omitted.
                    if (tk.length() > kMaxRegexLength) {
                        LOG_W("ThreadMatcher", "Skip thread regex exceeding " 
                              + std::to_string(kMaxRegexLength) + " chars: " + tr.keyword);
                        continue;
                    }
                    
                    try {
                        ctr.pattern = std::regex(tk, regex_flags_for(thread_regex, regex_flags));
                    } catch (const std::regex_error&) {
                        LOG_W("ThreadMatcher", "Skip invalid thread regex: " + tr.keyword);
                        continue;
                    }
                    ctr.is_main_thread_rule = false;
                }

                cr.thread_rules.push_back(std::move(ctr));
            }

            compiled_rules_.push_back(std::move(cr));
        }
    }

    MatchResult match(const std::string& proc_name, const std::string& thread_name,
                      ProcessState actual_state, int /*pid*/, const std::string& cmdline = "") {
        return match_impl(proc_name, thread_name, actual_state, cmdline, "Thread");
    }

    MatchResult match_process_only(const std::string& proc_name,
                                   const std::string& thread_name,
                                   ProcessState actual_state, int /*pid*/,
                                   const std::string& cmdline = "") {
        return match_impl(proc_name, thread_name, actual_state, cmdline, "Process");
    }

    std::vector<int> get_cpus_for_affinity(const std::string& affinity_class,
                                           ProcessState effective_state) const {
        std::string cpumask_name = get_cpumask_name_for_affinity(affinity_class, effective_state);
        if (cpumask_name.empty()) {
            return {};
        }

        auto mask_it = config_.sched.cpumask.find(cpumask_name);
        if (mask_it == config_.sched.cpumask.end()) {
            LOG_W("ThreadMatcher", "Unknown cpumask: " + cpumask_name);
            return {};
        }

        return mask_it->second;
    }

    std::string get_cpumask_name_for_affinity(const std::string& affinity_class,
                                              ProcessState effective_state) const {
        if (affinity_class.empty() || affinity_class == "auto") {
            return "";
        }

        auto it = config_.sched.affinity.find(affinity_class);
        if (it == config_.sched.affinity.end()) {
            LOG_W("ThreadMatcher", "Unknown affinity class: " + affinity_class);
            return "";
        }

        switch (effective_state) {
            case ProcessState::BG: return it->second.bg;
            case ProcessState::FG: return it->second.fg;
            case ProcessState::TOP: return it->second.top;
        }
        return "";
    }

    int get_prio_value(const std::string& prio_class, ProcessState state) const {
        if (prio_class.empty() || prio_class == "auto") {
            return 0;
        }

        auto it = config_.sched.prio.find(prio_class);
        if (it == config_.sched.prio.end()) {
            LOG_W("ThreadMatcher", "Unknown prio class: " + prio_class);
            return 0;
        }

        int prio = 0;
        switch (state) {
            case ProcessState::BG: prio = it->second.bg; break;
            case ProcessState::FG: prio = it->second.fg; break;
            case ProcessState::TOP: prio = it->second.top; break;
        }

        return prio;
    }

    const std::vector<CompiledProcessRule>& compiled_rules() const {
        return compiled_rules_;
    }

    const SchedConfig& sched_config() const {
        return config_.sched;
    }

    const Config& config() const {
        return config_;
    }

    const std::string& launcher_package() const {
        return launcher_package_;
    }

private:
    struct RegexPattern {
        std::string pattern;
        bool case_insensitive = false;
    };

    static RegexPattern parse_regex_pattern(const std::string& pattern) {
        constexpr const char* kCaseInsensitivePrefix = "(?i)";
        constexpr size_t kCaseInsensitivePrefixLength = 4;

        if (pattern.compare(0, kCaseInsensitivePrefixLength, kCaseInsensitivePrefix) == 0) {
            return {pattern.substr(kCaseInsensitivePrefixLength), true};
        }
        return {pattern, false};
    }

    static std::regex::flag_type regex_flags_for(
        const RegexPattern& pattern, std::regex::flag_type base_flags) {
        return pattern.case_insensitive ? base_flags | std::regex::icase : base_flags;
    }

    Config config_;
    std::string launcher_package_;
    std::vector<CompiledProcessRule> compiled_rules_;

    MatchResult match_impl(const std::string& proc_name, const std::string& thread_name,
                           ProcessState actual_state, const std::string& cmdline,
                           const char* log_label) {
        MatchResult result;
        result.effective_state = actual_state;

        std::string cache_key = proc_name + "#|#" + thread_name + "#|#" + cmdline
            + "#|#" + std::to_string(static_cast<int>(actual_state));
        
        if (proc_name == "[dead]") {
            size_t bucket = get_cache_bucket(cache_key);
            std::lock_guard<std::mutex> lock(process_cache_mutex_[bucket]);
            process_cache_[bucket].erase(cache_key);
            return result;
        }
        
        if (auto cached = get_cached_process_result(cache_key)) {
            result.matched = true;
            result.matched_rule_name = cached->matched_rule_name;
            result.affinity_class = cached->affinity_class;
            result.prio_class = cached->prio_class;
            result.cpumask_name = cached->cpumask_name;
            result.uclamp_max = cached->uclamp_max;
            result.cpu_share = cached->cpu_share;
            result.enable_limit = cached->enable_limit;
            result.effective_state = cached->effective_state;
            result.thread_rule_index = cached->thread_rule_index;
            result.pinned = cached->pinned;
            result.topfore = cached->topfore;
            LOG_T("ThreadMatcher", std::string(log_label) + " '" + thread_name + "' using cached rule '"
                  + cached->matched_rule_name + "'"
                  + ", ac=" + cached->affinity_class
                  + ", es=" + std::to_string((int)cached->effective_state));
            return result;
        }

        CompiledProcessRule* matched_rule = nullptr;

        for (auto& cr : compiled_rules_) {
            try {
                const bool cmdline_matched = !cmdline.empty()
                    && std::regex_search(cmdline, cr.pattern);
                const bool process_name_matched = std::regex_search(proc_name, cr.pattern);
                if (cmdline_matched || process_name_matched) {
                    matched_rule = &cr;
                    break;
                }
            } catch (const std::regex_error& e) {
                LOG_W("ThreadMatcher", "Process regex match failed for rule '" + cr.rule.name
                      + "': " + e.what());
            }
        }

        if (matched_rule) {
            result.matched = true;
            result.matched_rule_name = matched_rule->rule.name;

            ProcessState effective_state = actual_state;
            if (matched_rule->rule.pinned) {
                effective_state = ProcessState::TOP;
            } else if (matched_rule->rule.topfore && actual_state == ProcessState::FG) {
                effective_state = ProcessState::TOP;
            }
            result.effective_state = effective_state;
            result.pinned = matched_rule->rule.pinned;
            result.topfore = matched_rule->rule.topfore;

            for (size_t i = 0; i < matched_rule->thread_rules.size(); ++i) {
                const auto& ctr = matched_rule->thread_rules[i];
                if (ctr.is_main_thread_rule) {
                    if (thread_name == proc_name) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        result.thread_rule_index = static_cast<int>(ctr.source_rule_index);
                        LOG_T("ThreadMatcher", std::string(log_label) + " '" + thread_name + "' matched MAIN_THREAD rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                } else {
                    bool thread_matched = false;
                    try {
                        thread_matched = std::regex_search(thread_name, ctr.pattern);
                    } catch (const std::regex_error& e) {
                        LOG_W("ThreadMatcher", "Thread regex match failed for rule '"
                              + matched_rule->rule.name + "': " + e.what());
                    }
                    if (thread_matched) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        result.thread_rule_index = static_cast<int>(ctr.source_rule_index);
                        LOG_T("ThreadMatcher", std::string(log_label) + " '" + thread_name + "' matched rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                }
            }

            // Resolve cpumask_name from affinity_class + effective_state
            result.cpumask_name = get_cpumask_name_for_affinity(result.affinity_class, result.effective_state);

            if (result.matched && !result.matched_rule_name.empty()) {
                ProcessCacheEntry cache_entry;
                cache_entry.matched_rule_name = result.matched_rule_name;
                cache_entry.affinity_class = result.affinity_class;
                cache_entry.prio_class = result.prio_class;
                cache_entry.cpumask_name = result.cpumask_name;
                cache_entry.uclamp_max = result.uclamp_max;
                cache_entry.cpu_share = result.cpu_share;
                cache_entry.enable_limit = result.enable_limit;
                cache_entry.effective_state = result.effective_state;
                cache_entry.thread_rule_index = result.thread_rule_index;
                cache_entry.pinned = result.pinned;
                cache_entry.topfore = result.topfore;
                cache_entry.timestamp = std::chrono::steady_clock::now();
                cache_process_result(cache_key, cache_entry);
            }
        }

        return result;
    }

    // Design: /HOME_PACKAGE/ and /MAIN_THREAD/ macros use exact-match replacement only.
    // Partial/substring macro expansion is intentionally not supported to keep regex
    // semantics predictable and avoid ambiguity with user-supplied regex patterns.
    static std::string escape_regex_literal(const std::string& value) {
        static constexpr const char* kMetacharacters = R"(\^$.|?*+()[]{} )";
        std::string escaped;
        escaped.reserve(value.size() * 2);
        for (char character : value) {
            if (character != ' ' && std::strchr(kMetacharacters, character) != nullptr) {
                escaped.push_back('\\');
            }
            escaped.push_back(character);
        }
        return escaped;
    }

    std::string expand_process_regex(const std::string& regex_str,
                                     const std::string& launcher,
                                     const std::string& /*main_thread*/) {
        std::string result = regex_str;
        if (result == "/HOME_PACKAGE/") {
            result = "^" + escape_regex_literal(launcher) + "$";
        }
        return result;
    }

    std::string expand_thread_regex(const std::string& keyword,
                                    const std::string& process_name) {
        std::string result = keyword;
        if (result == "/MAIN_THREAD/") {
            result = process_name;
        }
        return result;
    }

    struct ProcessCacheEntry {
        std::string matched_rule_name;
        std::string affinity_class;
        std::string prio_class;
        std::string cpumask_name;
        std::optional<int> uclamp_max;
        std::optional<int> cpu_share;
        bool enable_limit;
        ProcessState effective_state;
        int thread_rule_index = -1;
        bool pinned = false;
        bool topfore = false;
        std::chrono::steady_clock::time_point timestamp;
    };

    static constexpr size_t kMaxProcessCacheEntriesPerBucket = 256;

    // 细粒度锁：分桶策略
    static constexpr size_t kCacheBuckets = 16;
    std::unordered_map<std::string, ProcessCacheEntry> process_cache_[kCacheBuckets];
    mutable std::mutex process_cache_mutex_[kCacheBuckets];

    inline size_t get_cache_bucket(const std::string& key) const {
        return std::hash<std::string>{}(key) % kCacheBuckets;
    }

    std::optional<ProcessCacheEntry> get_cached_process_result(const std::string& proc_name) {
        size_t bucket = get_cache_bucket(proc_name);
        std::lock_guard<std::mutex> lock(process_cache_mutex_[bucket]);
        auto it = process_cache_[bucket].find(proc_name);
        if (it != process_cache_[bucket].end()) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - it->second.timestamp).count();
            if (elapsed_ms < config_.sched.timing.process_cache_ttl_ms) {
                return it->second;
            }
            process_cache_[bucket].erase(it);
        }
        return std::nullopt;
    }

    void cache_process_result(const std::string& proc_name, const ProcessCacheEntry& entry) {
        const size_t bucket = get_cache_bucket(proc_name);
        std::lock_guard<std::mutex> lock(process_cache_mutex_[bucket]);
        auto& cache = process_cache_[bucket];
        const auto now = std::chrono::steady_clock::now();

        for (auto it = cache.begin(); it != cache.end();) {
            const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.timestamp).count();
            if (elapsed_ms >= config_.sched.timing.process_cache_ttl_ms) {
                it = cache.erase(it);
            } else {
                ++it;
            }
        }

        if (cache.find(proc_name) == cache.end()
            && cache.size() >= kMaxProcessCacheEntriesPerBucket) {
            const auto oldest = std::min_element(cache.begin(), cache.end(),
                [](const auto& left, const auto& right) {
                    return left.second.timestamp < right.second.timestamp;
                });
            if (oldest != cache.end()) {
                cache.erase(oldest);
            }
        }
        cache[proc_name] = entry;
    }
};

#endif
