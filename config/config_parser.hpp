#ifndef CONFIG_PARSER_HPP
#define CONFIG_PARSER_HPP

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <string>
#include <vector>
#include "config_types.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

using json = nlohmann::json;

struct ConfigParseResult {
    bool success = false;
    Config config;
};

class ConfigParser {
public:
    static ConfigParseResult parse(const std::string& path) {
        ConfigParseResult result;

        std::ifstream ifs(path);
        if (!ifs.is_open()) {
            LOG_W("ConfigParser", "Config file not found: " + path);
            return result;
        }

        try {
            json j = json::parse(ifs);
            if (!j.is_object()) {
                LOG_E("ConfigParser", "Config root must be an object");
                return result;
            }

            parse_meta(j, result.config);
            parse_sched(j, result.config);
            result.success = true;
        } catch (const json::exception& e) {
            LOG_E("ConfigParser", "Invalid config: " + std::string(e.what()));
            return result;
        } catch (const std::exception& e) {
            LOG_E("ConfigParser", "Failed to parse config: " + std::string(e.what()));
            return result;
        }

        LOG_I("ConfigParser", "Parsed config: " + result.config.meta_name + " by "
              + result.config.meta_author);
        return result;
    }

private:
    static bool is_safe_cgroup_component(const std::string& name) {
        if (name.empty() || name == "." || name == "..") {
            return false;
        }

        for (unsigned char c : name) {
            // This is intentionally a blacklist for cgroup path components, not an
            // ASCII-only whitelist: UTF-8 names (including Chinese and other languages)
            // remain valid. Reject only path syntax and control characters.
            if (c < 0x20 || c == 0x7f || c == '/' || c == '\\') {
                return false;
            }
        }
        return true;
    }

    static void parse_meta(const json& j, Config& config) {
        const auto meta_it = j.find("meta");
        if (meta_it == j.end()) {
            return;
        }
        if (!meta_it->is_object()) {
            LOG_W("ConfigParser", "meta must be an object");
            return;
        }

        const json& meta = *meta_it;
        if (meta.contains("name") && meta["name"].is_string()) {
            config.meta_name = meta["name"].get<std::string>();
        }
        if (meta.contains("author") && meta["author"].is_string()) {
            config.meta_author = meta["author"].get<std::string>();
        }
    }

    static void parse_sched(const json& j, Config& config) {
        const auto modules_it = j.find("modules");
        if (modules_it == j.end() || !modules_it->is_object()) {
            LOG_W("ConfigParser", "No valid modules object found, disabling scheduler");
            config.sched.enable = false;
            return;
        }

        const auto sched_it = modules_it->find("sched");
        if (sched_it == modules_it->end() || !sched_it->is_object()) {
            LOG_W("ConfigParser", "No valid sched module found, disabling scheduler");
            config.sched.enable = false;
            return;
        }

        const json& sched = *sched_it;
        auto& cfg = config.sched;

        if (sched.contains("enable") && sched["enable"].is_boolean()) {
            cfg.enable = sched["enable"].get<bool>();
        }
        if (sched.contains("case_insensitive") && sched["case_insensitive"].is_boolean()) {
            cfg.case_insensitive = sched["case_insensitive"].get<bool>();
        }

        int refresh_interval = 2000;
        if (sched.contains("refresh_interval_ms") && sched["refresh_interval_ms"].is_number_integer()) {
            refresh_interval = sched["refresh_interval_ms"].get<int>();
        }
        if (refresh_interval <= 0) {
            LOG_W("ConfigParser", "Invalid refresh_interval_ms " + std::to_string(refresh_interval) + ", using default 2000");
            refresh_interval = 2000;
        }
        cfg.refresh_interval_ms = refresh_interval;

        int highspeed = 300;
        if (sched.contains("highspeed_sched_ms") && sched["highspeed_sched_ms"].is_number_integer()) {
            highspeed = sched["highspeed_sched_ms"].get<int>();
        }
        if (highspeed <= 0) {
            LOG_W("ConfigParser", "Invalid highspeed_sched_ms " + std::to_string(highspeed) + ", using default 300");
            highspeed = 300;
        }
        cfg.highspeed_sched_ms = highspeed;

        auto parse_bounded_int = [&sched](const char* name, int default_value, int min_value, int max_value) {
            int value = default_value;
            if (sched.contains(name) && sched[name].is_number_integer()) {
                value = sched[name].get<int>();
            }
            if (value < min_value || value > max_value) {
                LOG_W("ConfigParser", "Invalid " + std::string(name) + " " + std::to_string(value)
                      + ", using default " + std::to_string(default_value));
                return default_value;
            }
            return value;
        };
        cfg.top_scan_budget_us = parse_bounded_int("top_scan_budget_us", 4000, 500, 20000);
        cfg.full_scan_budget_us = parse_bounded_int("full_scan_budget_us", 12000, 1000, 50000);
        cfg.scan_batch_size = parse_bounded_int("scan_batch_size", 32, 1, 256);
        cfg.scan_batch_yield_us = parse_bounded_int("scan_batch_yield_us", 0, 0, 1000);

        if (sched.contains("log")) {
            const json& log = sched["log"];
            if (!log.is_object()) {
                LOG_W("ConfigParser", "log must be an object");
            } else {
                if (log.contains("level") && log["level"].is_string()) {
                    cfg.log.level = log["level"].get<std::string>();
                }
                if (log.contains("output") && log["output"].is_string()) {
                    cfg.log.output = log["output"].get<std::string>();
                }
            }
        }

        parse_cpumask(sched, cfg);
        parse_affinity(sched, cfg);
        parse_prio(sched, cfg);
        parse_rules(sched, cfg);
        validate_references(cfg);
    }

    static void parse_cpumask(const json& sched, SchedConfig& cfg) {
        const auto masks_it = sched.find("cpumask");
        if (masks_it == sched.end()) {
            return;
        }
        if (!masks_it->is_object()) {
            LOG_W("ConfigParser", "cpumask must be an object");
            return;
        }

        for (auto it = masks_it->begin(); it != masks_it->end(); ++it) {
            const std::string name = it.key();
            if (!is_safe_cgroup_component(name)) {
                LOG_W("ConfigParser", "Invalid cpumask name: " + name);
                continue;
            }
            if (!it.value().is_array()) {
                LOG_W("ConfigParser", "cpumask[" + name + "] must be an array");
                continue;
            }

            std::vector<int> cpus;
            for (const auto& cpu : it.value()) {
                if (!cpu.is_number_integer()) {
                    LOG_W("ConfigParser", "Invalid CPU value in cpumask[" + name + "]");
                    continue;
                }

                const int value = cpu.get<int>();
                if (value < 0 || value >= CPU_SETSIZE) {
                    LOG_W("ConfigParser", "CPU index out of range in cpumask[" + name + "]: "
                          + std::to_string(value));
                    continue;
                }
                cpus.push_back(value);
            }

            std::sort(cpus.begin(), cpus.end());
            cpus.erase(std::unique(cpus.begin(), cpus.end()), cpus.end());
            if (cpus.empty()) {
                LOG_W("ConfigParser", "Ignoring empty cpumask[" + name + "]");
                continue;
            }

            // Keep cross-device configs usable; do not require every configured CPU to be online here.
            cfg.cpumask[name] = cpus;
            LOG_D("ConfigParser", "cpumask[" + name + "] = " + CpuMask::to_string(cpus));
        }
    }

    static void parse_affinity(const json& sched, SchedConfig& cfg) {
        const auto affinity_it = sched.find("affinity");
        if (affinity_it == sched.end()) {
            return;
        }
        if (!affinity_it->is_object()) {
            LOG_W("ConfigParser", "affinity must be an object");
            return;
        }

        for (auto it = affinity_it->begin(); it != affinity_it->end(); ++it) {
            const std::string name = it.key();
            const json& val = it.value();
            if (!val.is_object()) {
                LOG_W("ConfigParser", "affinity[" + name + "] must be an object");
                continue;
            }

            AffinityScene scene;
            if (val.contains("bg") && val["bg"].is_string()) {
                scene.bg = val["bg"].get<std::string>();
            }
            if (val.contains("fg") && val["fg"].is_string()) {
                scene.fg = val["fg"].get<std::string>();
            }
            if (val.contains("touch") && val["touch"].is_string()) {
                scene.top = val["touch"].get<std::string>();
            } else if (val.contains("top") && val["top"].is_string()) {
                scene.top = val["top"].get<std::string>();
            }

            cfg.affinity[name] = scene;
            LOG_D("ConfigParser", "affinity[" + name + "]: bg=" + scene.bg
                  + ", fg=" + scene.fg + ", top=" + scene.top);
        }
    }

    static bool is_valid_prio_value(int value) {
        return value == 0 || (value >= 1 && value <= 98)
            || (value >= 100 && value <= 139)
            || value == -1 || value == -2 || value == -3;
    }

    static int parse_prio_value(const json& val, const char* state_name,
                                const std::string& scene_name, int default_value) {
        if (!val.contains(state_name) || !val[state_name].is_number_integer()) {
            return default_value;
        }
        const int value = val[state_name].get<int>();
        if (is_valid_prio_value(value)) {
            return value;
        }
        LOG_W("ConfigParser", "Invalid prio[" + scene_name + "]." + state_name
              + " value " + std::to_string(value) + ", using " + std::to_string(default_value));
        return default_value;
    }

    static void parse_prio(const json& sched, SchedConfig& cfg) {
        const auto prio_it = sched.find("prio");
        if (prio_it == sched.end()) {
            return;
        }
        if (!prio_it->is_object()) {
            LOG_W("ConfigParser", "prio must be an object");
            return;
        }

        for (auto it = prio_it->begin(); it != prio_it->end(); ++it) {
            const std::string name = it.key();
            const json& val = it.value();
            if (!val.is_object()) {
                LOG_W("ConfigParser", "prio[" + name + "] must be an object");
                continue;
            }

            PrioScene scene;
            scene.bg = parse_prio_value(val, "bg", name, scene.bg);
            scene.fg = parse_prio_value(val, "fg", name, scene.fg);
            if (val.contains("touch")) {
                scene.top = parse_prio_value(val, "touch", name, scene.top);
            } else {
                scene.top = parse_prio_value(val, "top", name, scene.top);
            }

            cfg.prio[name] = scene;
            LOG_D("ConfigParser", "prio[" + name + "]: bg=" + std::to_string(scene.bg)
                  + ", fg=" + std::to_string(scene.fg) + ", top=" + std::to_string(scene.top));
        }
    }

    static void parse_rules(const json& sched, SchedConfig& cfg) {
        const auto rules_it = sched.find("rules");
        if (rules_it == sched.end()) {
            return;
        }
        if (!rules_it->is_array()) {
            LOG_W("ConfigParser", "rules must be an array");
            return;
        }

        for (const auto& rule : *rules_it) {
            if (!rule.is_object()) {
                LOG_W("ConfigParser", "Ignoring non-object process rule");
                continue;
            }

            ProcessRule pr;
            if (rule.contains("name") && rule["name"].is_string()) {
                pr.name = rule["name"].get<std::string>();
            }
            if (!is_safe_cgroup_component(pr.name)) {
                LOG_W("ConfigParser", "Ignoring process rule with unsafe name: " + pr.name);
                continue;
            }
            if (!rule.contains("regex") || !rule["regex"].is_string()) {
                LOG_W("ConfigParser", "Ignoring process rule '" + pr.name
                      + "': regex must be a non-empty string");
                continue;
            }
            pr.regex_str = rule["regex"].get<std::string>();
            if (rule.contains("comm_regex")) {
                LOG_W("ConfigParser", "Ignoring removed comm_regex in process rule '" + pr.name + "'");
            }
            if (pr.regex_str.empty()) {
                LOG_W("ConfigParser", "Ignoring process rule '" + pr.name + "': regex must not be empty");
                continue;
            }
            if (rule.contains("pinned") && rule["pinned"].is_boolean()) {
                pr.pinned = rule["pinned"].get<bool>();
            }
            if (rule.contains("topfore") && rule["topfore"].is_boolean()) {
                pr.topfore = rule["topfore"].get<bool>();
            }

            const auto thread_rules_it = rule.find("rules");
            if (thread_rules_it != rule.end()) {
                if (!thread_rules_it->is_array()) {
                    LOG_W("ConfigParser", "rules for process " + pr.name + " must be an array");
                } else {
                    for (const auto& tr : *thread_rules_it) {
                        if (!tr.is_object()) {
                            LOG_W("ConfigParser", "Ignoring non-object thread rule for " + pr.name);
                            continue;
                        }

                        ThreadRule t;
                        if (tr.contains("k") && tr["k"].is_string()) {
                            t.keyword = tr["k"].get<std::string>();
                        } else {
                            t.keyword = ".";
                        }
                        if (tr.contains("ac") && tr["ac"].is_string()) {
                            t.affinity_class = tr["ac"].get<std::string>();
                        } else {
                            t.affinity_class = "auto";
                        }
                        if (tr.contains("pc") && tr["pc"].is_string()) {
                            t.prio_class = tr["pc"].get<std::string>();
                        } else {
                            t.prio_class = "auto";
                        }

                        if (tr.contains("uclamp_max") && tr["uclamp_max"].is_number_integer()) {
                            const int value = tr["uclamp_max"].get<int>();
                            if (value >= 0 && value <= 100) {
                                t.uclamp_max = value;
                            } else {
                                LOG_W("ConfigParser", "Invalid uclamp_max " + std::to_string(value)
                                      + " for rule[" + pr.name + "], expected 0-100");
                            }
                        }
                        if (tr.contains("cpu_share") && tr["cpu_share"].is_number_integer()) {
                            const int value = tr["cpu_share"].get<int>();
                            if (value >= 0 && value <= 1024) {
                                t.cpu_share = value;
                            } else {
                                LOG_W("ConfigParser", "Invalid cpu_share " + std::to_string(value)
                                      + " for rule[" + pr.name + "], expected 0-1024 (Android-specific)");
                            }
                        }
                        if (tr.contains("enable_limit") && tr["enable_limit"].is_boolean()) {
                            t.enable_limit = tr["enable_limit"].get<bool>();
                        }

                        pr.thread_rules.push_back(std::move(t));
                    }
                }
            }

            cfg.rules.push_back(std::move(pr));
            const ProcessRule& parsed = cfg.rules.back();
            LOG_D("ConfigParser", "Added rule: " + parsed.name + " (pinned="
                  + std::to_string(parsed.pinned) + ", topfore=" + std::to_string(parsed.topfore)
                  + ", threads=" + std::to_string(parsed.thread_rules.size()));
        }
    }

    static void validate_affinity_mask(const std::string& affinity_name,
                                       const char* state_name,
                                       std::string& mask_name,
                                       const SchedConfig& cfg) {
        if (!mask_name.empty() && cfg.cpumask.find(mask_name) == cfg.cpumask.end()) {
            LOG_W("ConfigParser", "Unknown cpumask '" + mask_name + "' in affinity["
                  + affinity_name + "]." + state_name + ", disabling this state");
            mask_name.clear();
        }
    }

    static void validate_references(SchedConfig& cfg) {
        for (auto& [name, scene] : cfg.affinity) {
            validate_affinity_mask(name, "bg", scene.bg, cfg);
            validate_affinity_mask(name, "fg", scene.fg, cfg);
            validate_affinity_mask(name, "top", scene.top, cfg);
        }

        for (auto& rule : cfg.rules) {
            for (auto& thread_rule : rule.thread_rules) {
                if (!thread_rule.affinity_class.empty() && thread_rule.affinity_class != "auto"
                    && cfg.affinity.find(thread_rule.affinity_class) == cfg.affinity.end()) {
                    LOG_W("ConfigParser", "Unknown affinity class '" + thread_rule.affinity_class
                          + "' in rule[" + rule.name + "], using auto");
                    thread_rule.affinity_class = "auto";
                }
                if (!thread_rule.prio_class.empty() && thread_rule.prio_class != "auto"
                    && cfg.prio.find(thread_rule.prio_class) == cfg.prio.end()) {
                    LOG_W("ConfigParser", "Unknown prio class '" + thread_rule.prio_class
                          + "' in rule[" + rule.name + "], using auto");
                    thread_rule.prio_class = "auto";
                }
            }
        }
    }
};

#endif
