#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
public:
    static bool init(const Config& config) {
        bool success = true;
        
        success &= init_cpuset(config);
        success &= init_cpuctl(config);
        report_stale_groups(config);

        return success;
    }

private:
    static std::set<std::string> list_child_directories(const std::string& path) {
        std::set<std::string> names;
        DIR* dir = opendir(path.c_str());
        if (dir == nullptr) return names;
        while (const dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (entry->d_type == DT_DIR) {
                names.insert(name);
                continue;
            }
            if (entry->d_type == DT_UNKNOWN) {
                struct stat st {};
                if (fstatat(dirfd(dir), name.c_str(), &st, 0) == 0 && S_ISDIR(st.st_mode)) {
                    names.insert(name);
                }
            }
        }
        closedir(dir);
        return names;
    }

    static void report_stale_groups(const Config& config) {
        std::set<std::string> expected_cpuset;
        for (const auto& [name, cpus] : config.sched.cpumask) {
            (void)cpus;
            expected_cpuset.insert("ReUperf_" + name);
        }
        for (const auto& name : list_child_directories("/dev/cpuset")) {
            if (name.rfind("ReUperf_", 0) == 0 && expected_cpuset.count(name) == 0) {
                LOG_W("CgroupInit", "Stale cpuset group retained for safety: /dev/cpuset/" + name);
            }
        }

        std::set<std::string> expected_cpuctl;
        for (const auto& rule : config.sched.rules) expected_cpuctl.insert(rule.name);
        for (const auto& name : list_child_directories("/dev/cpuctl/ReUperf")) {
            if (expected_cpuctl.count(name) == 0) {
                LOG_W("CgroupInit", "Stale cpuctl group retained for safety: /dev/cpuctl/ReUperf/" + name);
            }
        }
    }

    static bool ensure_cgroup_file_exists(const std::string& path, int max_retries = 5) {
        for (int i = 0; i < max_retries; ++i) {
            if (FileUtils::file_exists(path)) {
                return true;
            }
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
        
        // 检测所有物理 CPU 核心
        std::string all_cpus = CpuMask::get_all_cpus_string();
        LOG_I("CgroupInit", "Detected all CPUs: " + all_cpus);
        if (all_cpus.empty()) {
            LOG_E("CgroupInit", "No CPUs detected, cannot initialize cpuset");
            return false;
        }
        
        std::string base_path = "/dev/cpuset";
        std::string root_mems = FileUtils::read_file(base_path + "/mems");
        if (root_mems.empty()) {
            LOG_E("CgroupInit", "Root cpuset mems is empty or unreadable");
            return false;
        }
        int created = 0, failed = 0;
        
        // 创建各规则子组，命名格式：ReUperf_<cpumask_name>（例如 ReUperf_all）
        for (const auto& cpumask : config.sched.cpumask) {
            std::string child_path = base_path + "/ReUperf_" + cpumask.first;
            
            if (!FileUtils::mkdir_recursive(child_path)) {
                LOG_W("CgroupInit", "Failed to create cpuset: " + child_path);
                failed++;
                continue;
            }
            
            if (!ensure_cgroup_file_exists(child_path + "/cpus")) {
                LOG_W("CgroupInit", "cpuset control files not created for " + child_path);
                failed++;
                continue;
            }
            
            std::string cpus_str = CpuMask::to_string(cpumask.second);
            if (cpus_str.empty()) {
                LOG_W("CgroupInit", "Empty cpumask for " + cpumask.first + ", skipping");
                continue;
            }
            
            if (!FileUtils::write_kernel_control_file(child_path + "/cpus", cpus_str)) {
                LOG_W("CgroupInit", "Failed to set cpus for " + child_path 
                      + " (cpus=" + cpus_str + ")");
                failed++;
                continue;
            }
            
            LOG_D("CgroupInit", "Set " + child_path + "/cpus = " + cpus_str);
            
            if (!ensure_cgroup_file_exists(child_path + "/mems")
                || !ensure_cgroup_file_exists(child_path + "/tasks")) {
                LOG_W("CgroupInit", "Required cpuset control files missing for " + child_path);
                failed++;
                continue;
            }
            if (!FileUtils::write_kernel_control_file(child_path + "/mems", root_mems)) {
                LOG_W("CgroupInit", "Failed to set mems for " + child_path);
                failed++;
                continue;
            }
            LOG_D("CgroupInit", "Set " + child_path + "/mems = " + root_mems);
            
            created++;
            
            // ReUperf masks intentionally overlap. Keep these groups non-exclusive so
            // their CPU and memory-node sets remain valid on cpuset v1 kernels.
            if (FileUtils::file_exists(child_path + "/cpu_exclusive")) {
                int fd = open((child_path + "/cpu_exclusive").c_str(), O_WRONLY);
                if (fd >= 0) {
                    const char* v = "0";
                    ssize_t written = write(fd, v, 1);
                    int err = errno;
                    close(fd);
                    if (written != 1) {
                        LOG_W("CgroupInit", "Failed to disable cpu_exclusive: " + std::string(strerror(err)));
                    }
                }
            }
            if (FileUtils::file_exists(child_path + "/mem_exclusive")) {
                int fd = open((child_path + "/mem_exclusive").c_str(), O_WRONLY);
                if (fd >= 0) {
                    const char* v = "0";
                    ssize_t written = write(fd, v, 1);
                    int err = errno;
                    close(fd);
                    if (written != 1) {
                        LOG_W("CgroupInit", "Failed to disable mem_exclusive: " + std::string(strerror(err)));
                    }
                }
            }
        }
        
        LOG_I("CgroupInit", "cpuset cgroups: " + std::to_string(created)
              + " groups created, " + std::to_string(failed) + " failed");
        return failed == 0;
    }

    static bool init_cpuctl(const Config& config) {
        LOG_I("CgroupInit", "Initializing cpuctl cgroups...");
        
        if (!FileUtils::dir_exists("/dev/cpuctl")) {
            LOG_W("CgroupInit", "/dev/cpuctl not found, skipping cpuctl init");
            return true;
        }
        
        std::string base_path = "/dev/cpuctl";
        
        // 在根目录创建 ReUperf 组（不受子集 cpus 限制）
        std::string reuperf_path = base_path + "/ReUperf";
        if (!FileUtils::mkdir_recursive(reuperf_path)) {
            LOG_W("CgroupInit", "Failed to create " + reuperf_path);
            return false;
        }
        
        int failed = 0;

        // 创建各规则子组，以及每个线程规则的 A{index} 子组
        for (const auto& rule : config.sched.rules) {
            std::string rule_path = reuperf_path + "/" + rule.name;
            
            if (!FileUtils::mkdir_recursive(rule_path)) {
                LOG_W("CgroupInit", "Failed to create cpuctl: " + rule_path);
                failed++;
                continue;
            }
            
            LOG_D("CgroupInit", "Created cpuctl group: " + rule_path);

            // Create A{index} subdirectories for each thread rule with enable_limit
            for (size_t i = 0; i < rule.thread_rules.size(); ++i) {
                const auto& tr = rule.thread_rules[i];
                if (!tr.enable_limit) continue;

                std::string sub_path = rule_path + "/A" + std::to_string(i + 1);
                if (!FileUtils::mkdir_recursive(sub_path)) {
                    LOG_W("CgroupInit", "Failed to create cpuctl: " + sub_path);
                    failed++;
                    continue;
                }

                if (tr.uclamp_max.has_value()) {
                    std::string uclamp_path = sub_path + "/cpu.uclamp.max";
                    if (!FileUtils::file_exists(uclamp_path)
                        || !FileUtils::write_kernel_control_file(uclamp_path, std::to_string(tr.uclamp_max.value()))) {
                        LOG_W("CgroupInit", "Failed to configure " + uclamp_path);
                        failed++;
                    }
                }
                if (tr.cpu_share.has_value()) {
                    std::string shares_path = sub_path + "/cpu.shares";
                    if (!FileUtils::file_exists(shares_path)
                        || !FileUtils::write_kernel_control_file(shares_path, std::to_string(tr.cpu_share.value()))) {
                        LOG_W("CgroupInit", "Failed to configure " + shares_path);
                        failed++;
                    }
                }

                LOG_D("CgroupInit", "Created cpuctl sub-group: " + sub_path);
            }
        }
        
        LOG_I("CgroupInit", "cpuctl cgroups initialized, " + std::to_string(failed) + " failures");
        return failed == 0;
    }
};

#endif