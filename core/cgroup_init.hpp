#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP
#include <string>
#include <vector>
#include <map>
#ifdef __ANDROID__
#include <unistd.h>
#endif
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
public:
    // 全局标志：记录内核是否支持 uclamp
    static inline bool uclamp_supported = true;

    static bool init(const Config& config) {
        LOG_I("CgroupInit", "Starting cgroup initialization...");

        // 1. Cpuset 是核心功能，必须成功
        if (!init_cpuset(config)) {
            LOG_E("CgroupInit", "Cpuset initialization FAILED. ReUperf cannot start.");
            return false;
        }

        // 2. Cpuctl 初始化（仅用于探测 uclamp 支持情况）
        // 即使 cpuctl 路径不存在或失败，也不阻断启动
        if (FileUtils::dir_exists("/dev/cpuctl")) {
            init_cpuctl(config);
        } else {
            LOG_W("CgroupInit", "/dev/cpuctl not found, skipping uclamp.");
            uclamp_supported = false;
        }

        // 3. Schedtune 回退（uclamp 不支持时的替代方案）
        // 注意：/dev/stune 经常受 SELinux 限制导致不可写。
        // 这里即使 init_schedtune 返回 false，我们也不应该阻断程序启动，
        // 因为 Cpuset + Priority 已经足够工作。
        if (!uclamp_supported) {
            LOG_W("CgroupInit", "Uclamp NOT supported. Attempting schedtune fallback...");
            if (!init_schedtune(config)) {
                LOG_W("CgroupInit", "Schedtune fallback FAILED. "
                      "This is normal due to SELinux restrictions on /dev/stune. "
                      "Continuing with Cpuset + Priority features.");
            }
        }

        LOG_I("CgroupInit", "Cgroup initialization completed. (Mode: Cpuset + Priority" +               std::string(!uclamp_supported ? " + (Schedtune attempted)" : " + Uclamp") + ")");
        return true;
    }

private:
    static bool ensure_cgroup_file_exists(const std::string& path, int max_retries = 5) {
        for (int i = 0; i < max_retries; ++i) {
            if (FileUtils::file_exists(path)) return true;
#ifdef __ANDROID__
            usleep(5000);
#endif
        }
        return false;
    }

    static bool init_cpuset(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuset cgroups...");
        if (!FileUtils::dir_exists("/dev/cpuset")) {
            LOG_W("CgroupInit", "/dev/cpuset not found, skipping cpuset init");
            return true;
        }
        if (!FileUtils::file_exists("/dev/cpuset/cpus")) {
            LOG_W("CgroupInit", "cpuset not properly mounted (missing /dev/cpuset/cpus), skipping");
            return true;
        }

        std::string all_cpus = CpuMask::get_all_cpus_string();
        if (all_cpus.empty()) return false;

        std::string base_path = "/dev/cpuset";
        std::string root_mems = FileUtils::read_file(base_path + "/mems");
        int created = 0, failed = 0;

        for (const auto& cpumask : config.sched.cpumask) {
            std::string child_path = base_path + "/ReUperf_" + cpumask.first;
            if (!FileUtils::mkdir_recursive(child_path)) { failed++; continue; }
            if (!ensure_cgroup_file_exists(child_path + "/cpus")) { failed++; continue; }

            std::string cpus_str = CpuMask::to_string(cpumask.second);
            if (cpus_str.empty()) continue;

            // ✅ 关键修复：先写 mems，再写 cpus（Android 内核要求）
            if (!root_mems.empty() && ensure_cgroup_file_exists(child_path + "/mems")) {
                if (!FileUtils::write_file(child_path + "/mems", root_mems)) { 
                    LOG_W("CgroupInit", "Failed to write mems for " + child_path);
                }
            }
#ifdef __ANDROID__
            usleep(10000); // 延迟
#endif
            if (!FileUtils::write_file(child_path + "/cpus", cpus_str)) {
                LOG_W("CgroupInit", "Failed to set cpus for " + child_path);
                failed++; continue;
            }
            created++;

            // ✅ 移除 cpu_exclusive 写入：Android 上通常不支持且一直报 "Invalid argument"，属于无效日志噪音
        }
        LOG_I("CgroupInit", "cpuset cgroups: " + std::to_string(created) + " created, " + std::to_string(failed) + " failed");
        return true;
    }

    static bool init_cpuctl(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuctl cgroups (probing uclamp)...");
        if (!FileUtils::dir_exists("/dev/cpuctl")) return true;

        std::string base_path = "/dev/cpuctl";
        std::string reuperf_path = base_path + "/ReUperf";
        if (!FileUtils::mkdir_recursive(reuperf_path)) return false;

        bool first_attempt = true;
        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(path)) continue;

            for (const auto& tr : rule.thread_rules) {
                if (tr.enable_limit && tr.uclamp_max.has_value()) {
                    std::string uclamp_path = path + "/cpu.uclamp.max";
                    if (FileUtils::file_exists(uclamp_path)) {
                        if (!FileUtils::write_file(uclamp_path, std::to_string(tr.uclamp_max.value()))) {
                            if (first_attempt) {
                                LOG_W("CgroupInit", "Uclamp write failed, marking unsupported.");
                                uclamp_supported = false;
                                first_attempt = false;
                            }
                        }
                    } else {
                        if (first_attempt) {
                            LOG_W("CgroupInit", "Uclamp file not found, marking unsupported.");
                            uclamp_supported = false;
                            first_attempt = false;
                        }
                    }
                }
            }
        }
        LOG_I("CgroupInit", "cpuctl init done (Uclamp: " + std::string(uclamp_supported ? "Yes" : "No") + ")");
        return true;
    }
    static bool init_schedtune(const Config& config) {
        std::string stune_base = "/dev/stune";
        if (!FileUtils::dir_exists(stune_base)) return false;

        std::string reuperf_stune_path = stune_base + "/ReUperf";
        // 尝试创建 ReUperf 根目录
        if (!FileUtils::mkdir_recursive(reuperf_stune_path)) {
            LOG_E("CgroupInit", "Failed to create schedtune base: " + reuperf_stune_path + 
                  ". SELinux likely blocking writes to /dev/stune.");
            return false;
        }

        bool any_applied = false;
        for (const auto& rule : config.sched.rules) {
            std::string path = reuperf_stune_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(path)) {
                // 单个组创建失败不阻断，仅警告
                LOG_W("CgroupInit", "Failed to create schedtune group: " + path);
                continue;
            }

            int max_uclamp = -1;
            for (const auto& tr : rule.thread_rules) {
                if (tr.uclamp_max.has_value() && tr.uclamp_max.value() > max_uclamp) {
                    max_uclamp = tr.uclamp_max.value();
                }
            }

            if (max_uclamp >= 0) {
                int boost = (max_uclamp * 100) / 1024;
                std::string boost_path = path + "/schedtune.boost";
                
                if (FileUtils::file_exists(boost_path) && FileUtils::write_file(boost_path, std::to_string(boost))) {
                    LOG_I("CgroupInit", "Applied schedtune: " + rule.name + " boost=" + std::to_string(boost));
                    any_applied = true;
                    
                    if (boost > 50) {
                        std::string idle_path = path + "/schedtune.prefer_idle";
                        if (FileUtils::file_exists(idle_path)) FileUtils::write_file(idle_path, "1");
                    }
                }
            }
        }
        return any_applied;
    }
};
#endif