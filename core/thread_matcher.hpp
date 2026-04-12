#ifndef THREAD_MATCHER_HPP
#define THREAD_MATCHER_HPP

#include <string>
#include <regex>
#include <vector>
#include <optional>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"

struct MatchResult {
    bool matched = false;
    std::string affinity_class;
    std::string prio_class;
    std::optional<int> uclamp_max;
    std::optional<int> cpu_share;
    bool enable_limit = false;
    ProcessState effective_state;
    std::string matched_rule_name;
    bool pinned = false;
    bool topfore = false;
};

inline bool is_result_equal(const MatchResult& a, const MatchResult& b) {
    return a.matched == b.matched &&
           a.affinity_class == b.affinity_class &&
           a.prio_class == b.prio_class &&
           a.effective_state == b.effective_state &&
           a.enable_limit == b.enable_limit &&
           a.uclamp_max == b.uclamp_max &&
           a.cpu_share == b.cpu_share;
}

struct CompiledThreadRule {
    ThreadRule rule;
    std::regex pattern;
    bool is_main_thread_rule = false;
};

struct CompiledProcessRule {
    ProcessRule rule;
    std::regex pattern;        // 匹配 cmdline
    std::regex comm_pattern;   // 匹配 comm
    std::vector<CompiledThreadRule> thread_rules;
};

static constexpr size_t kMaxRegexLength = 200;

class ThreadMatcher {
public:
    explicit ThreadMatcher(const Config& config) : config_(config), launcher_package_(config.launcher_package) {
        for (const auto& rule : config_.sched.rules) {
            CompiledProcessRule cr;
            cr.rule = rule;

            std::string expanded = expand_process_regex(rule.regex_str,
                launcher_package_, "");
            
            // Wrap with .* for contains-match semantics
            expanded = ".*" + expanded + ".*";
            
            // Security: limit regex length to prevent ReDoS
            if (expanded.length() > kMaxRegexLength) {
                LOG_W("ThreadMatcher", "Skip regex exceeding " + std::to_string(kMaxRegexLength) 
                      + " chars: " + rule.regex_str);
                continue;
            }
            
            try {
                cr.pattern = std::regex(expanded);
            } catch (const std::regex_error&) {
                LOG_W("ThreadMatcher", "Skip invalid process regex: " + rule.regex_str);
            }

            if (!rule.comm_regex_str.empty() && rule.comm_regex_str != "^$") {
                std::string comm_expanded = expand_process_regex(rule.comm_regex_str,
                    launcher_package_, "");
                
                // Wrap with .* for contains-match semantics
                comm_expanded = ".*" + comm_expanded + ".*";
                
                if (comm_expanded.length() > kMaxRegexLength) {
                    LOG_W("ThreadMatcher", "Skip comm_regex exceeding " + std::to_string(kMaxRegexLength) 
                          + " chars: " + rule.comm_regex_str);
                } else {
                    try {
                        cr.comm_pattern = std::regex(comm_expanded);
                    } catch (const std::regex_error&) {
                        LOG_W("ThreadMatcher", "Skip invalid comm process regex: " + rule.comm_regex_str);
                    }
                }
            }

            for (const auto& tr : rule.thread_rules) {
                CompiledThreadRule ctr;
                ctr.rule = tr;

                if (tr.keyword == "/MAIN_THREAD/") {
                    ctr.pattern = std::regex("^$");
                    ctr.is_main_thread_rule = true;
                } else {
                    std::string tk = expand_thread_regex(tr.keyword, "");
                    
                    // Wrap with .* for contains-match semantics
                    tk = ".*" + tk + ".*";
                    
                    // Security: limit regex length to prevent ReDoS
                    if (tk.length() > kMaxRegexLength) {
                        LOG_W("ThreadMatcher", "Skip thread regex exceeding " 
                              + std::to_string(kMaxRegexLength) + " chars: " + tr.keyword);
                        continue;
                    }
                    
                    try {
                        ctr.pattern = std::regex(tk);
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
        MatchResult result;
        result.effective_state = actual_state;

        std::string cache_key = proc_name + "#|#" + thread_name + "#|#" + cmdline;
        
        if (proc_name == "[dead]") {
            process_cache_.erase(cache_key);
            return result;
        }
        
        if (auto cached = get_cached_process_result(cache_key)) {
            result.matched = true;
            result.matched_rule_name = cached->matched_rule_name;
            result.affinity_class = cached->affinity_class;
            result.prio_class = cached->prio_class;
            result.uclamp_max = cached->uclamp_max;
            result.cpu_share = cached->cpu_share;
            result.enable_limit = cached->enable_limit;
            result.effective_state = cached->effective_state;
            LOG_T("ThreadMatcher", "Thread '" + thread_name + "' using cached rule '"
                  + cached->matched_rule_name + "'"
                  + ", ac=" + cached->affinity_class
                  + ", es=" + std::to_string((int)cached->effective_state));
            return result;
        }

        CompiledProcessRule* matched_rule = nullptr;

        for (auto& cr : compiled_rules_) {
            // regex 同时匹配 cmdline 和 proc_name（comm）
            bool cmdline_matched = !cmdline.empty() && std::regex_search(cmdline, cr.pattern);
            bool comm_matched = std::regex_search(proc_name, cr.pattern);
            if (cmdline_matched || comm_matched) {
                matched_rule = &cr;
                break;
            }

            // 单独的 comm_regex 字段（向后兼容）
            bool has_comm_pattern = false;
            try {
                has_comm_pattern = cr.comm_pattern.mark_count() >= 0;
            } catch (...) {
                has_comm_pattern = false;
            }
            if (has_comm_pattern && std::regex_search(proc_name, cr.comm_pattern)) {
                matched_rule = &cr;
                break;
            }

            // Priority 3: Wildcard rule ("." or "*") as fallback
            if (cr.rule.regex_str == "." || cr.rule.regex_str == "*" || cr.rule.regex_str.empty()) {
                matched_rule = &cr;
                break;
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

            for (const auto& ctr : matched_rule->thread_rules) {
                if (ctr.is_main_thread_rule) {
                    if (thread_name == proc_name) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Thread '" + thread_name + "' matched MAIN_THREAD rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                } else {
                    if (std::regex_search(thread_name, ctr.pattern)) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Thread '" + thread_name + "' matched rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                }
            }

            if (result.matched && !result.matched_rule_name.empty()) {
                ProcessCacheEntry cache_entry;
                cache_entry.matched_rule_name = result.matched_rule_name;
                cache_entry.affinity_class = result.affinity_class;
                cache_entry.prio_class = result.prio_class;
                cache_entry.uclamp_max = result.uclamp_max;
                cache_entry.cpu_share = result.cpu_share;
                cache_entry.enable_limit = result.enable_limit;
                cache_entry.effective_state = result.effective_state;
                cache_entry.pinned = result.pinned;
                cache_entry.topfore = result.topfore;
                cache_entry.timestamp = std::chrono::steady_clock::now();
                cache_process_result(cache_key, cache_entry);
            }
        }

        return result;
    }

    MatchResult match_process_only(const std::string& proc_name,
                                   const std::string& thread_name,
                                   ProcessState actual_state, int /*pid*/,
                                   const std::string& cmdline = "") {
        MatchResult result;
        result.effective_state = actual_state;

        std::string cache_key = proc_name + "#|#" + thread_name + "#|#" + cmdline;
        
        if (proc_name == "[dead]") {
            process_cache_.erase(cache_key);
            return result;
        }
        
        if (auto cached = get_cached_process_result(cache_key)) {
            result.matched = true;
            result.matched_rule_name = cached->matched_rule_name;
            result.affinity_class = cached->affinity_class;
            result.prio_class = cached->prio_class;
            result.uclamp_max = cached->uclamp_max;
            result.cpu_share = cached->cpu_share;
            result.enable_limit = cached->enable_limit;
            result.effective_state = cached->effective_state;
            result.pinned = cached->pinned;
            result.topfore = cached->topfore;
            LOG_T("ThreadMatcher", "Process '" + proc_name + "' using cached rule '"
                  + cached->matched_rule_name + "'"
                  + ", ac=" + cached->affinity_class
                  + ", es=" + std::to_string((int)cached->effective_state));
            return result;
        }

        CompiledProcessRule* matched_rule = nullptr;

        for (auto& cr : compiled_rules_) {
            bool cmdline_matched = !cmdline.empty() && std::regex_search(cmdline, cr.pattern);
            bool comm_matched = std::regex_search(proc_name, cr.pattern);
            if (cmdline_matched || comm_matched) {
                matched_rule = &cr;
                break;
            }

            bool has_comm_pattern = false;
            try {
                has_comm_pattern = cr.comm_pattern.mark_count() >= 0;
            } catch (...) {
                has_comm_pattern = false;
            }
            if (has_comm_pattern && std::regex_search(proc_name, cr.comm_pattern)) {
                matched_rule = &cr;
                break;
            }

            if (cr.rule.regex_str == "." || cr.rule.regex_str == "*" || cr.rule.regex_str.empty()) {
                matched_rule = &cr;
                break;
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

            for (const auto& ctr : matched_rule->thread_rules) {
                if (ctr.is_main_thread_rule) {
                    if (thread_name == proc_name) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Process '" + proc_name + "' matched MAIN_THREAD rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                } else {
                    if (std::regex_search(thread_name, ctr.pattern)) {
                        result.affinity_class = ctr.rule.affinity_class;
                        result.prio_class = ctr.rule.prio_class;
                        result.uclamp_max = ctr.rule.uclamp_max;
                        result.cpu_share = ctr.rule.cpu_share;
                        result.enable_limit = ctr.rule.enable_limit;
                        LOG_T("ThreadMatcher", "Process '" + proc_name + "' matched rule '"
                              + matched_rule->rule.name + "' -> ac=" + ctr.rule.affinity_class
                              + ", pc=" + ctr.rule.prio_class);
                        break;
                    }
                }
            }

            if (result.matched && !result.matched_rule_name.empty()) {
                ProcessCacheEntry cache_entry;
                cache_entry.matched_rule_name = result.matched_rule_name;
                cache_entry.affinity_class = result.affinity_class;
                cache_entry.prio_class = result.prio_class;
                cache_entry.uclamp_max = result.uclamp_max;
                cache_entry.cpu_share = result.cpu_share;
                cache_entry.enable_limit = result.enable_limit;
                cache_entry.effective_state = result.effective_state;
                cache_entry.pinned = result.pinned;
                cache_entry.topfore = result.topfore;
                cache_entry.timestamp = std::chrono::steady_clock::now();
                cache_process_result(cache_key, cache_entry);
            }
        }

        return result;
    }

    std::vector<int> get_cpus_for_affinity(const std::string& affinity_class,
                                           ProcessState effective_state) {
        if (affinity_class.empty()) {
            return {};
        }

        auto it = config_.sched.affinity.find(affinity_class);
        if (it == config_.sched.affinity.end()) {
            LOG_W("ThreadMatcher", "Unknown affinity class: " + affinity_class);
            return {};
        }

        std::string cpumask_name;
        switch (effective_state) {
            case ProcessState::BG: cpumask_name = it->second.bg; break;
            case ProcessState::FG: cpumask_name = it->second.fg; break;
            case ProcessState::TOP: cpumask_name = it->second.top; break;
        }

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

    int get_prio_value(const std::string& prio_class, ProcessState state) {
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
    Config config_;
    std::string launcher_package_;
    std::vector<CompiledProcessRule> compiled_rules_;

    std::string expand_process_regex(const std::string& regex_str,
                                     const std::string& launcher,
                                     const std::string& /*main_thread*/) {
        std::string result = regex_str;
        if (result == "/HOME_PACKAGE/") {
            result = "^" + launcher + "$";
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
        std::optional<int> uclamp_max;
        std::optional<int> cpu_share;
        bool enable_limit;
        ProcessState effective_state;
        bool pinned = false;
        bool topfore = false;
        std::chrono::steady_clock::time_point timestamp;
    };

    static constexpr int64_t kProcessCacheTTLMs = 100;
    std::unordered_map<std::string, ProcessCacheEntry> process_cache_;
    mutable std::mutex process_cache_mutex_;

    void clear_process_cache() {
        std::lock_guard<std::mutex> lock(process_cache_mutex_);
        process_cache_.clear();
    }

    std::optional<ProcessCacheEntry> get_cached_process_result(const std::string& proc_name) {
        std::lock_guard<std::mutex> lock(process_cache_mutex_);
        auto it = process_cache_.find(proc_name);
        if (it != process_cache_.end()) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - it->second.timestamp).count();
            if (elapsed_ms < kProcessCacheTTLMs) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    void cache_process_result(const std::string& proc_name, const ProcessCacheEntry& entry) {
        std::lock_guard<std::mutex> lock(process_cache_mutex_);
        process_cache_[proc_name] = entry;
    }
};

#endif
