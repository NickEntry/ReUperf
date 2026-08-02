#ifndef CGROUP_INIT_HPP
#define CGROUP_INIT_HPP

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/cpu_mask.hpp"

class CgroupInitializer {
    class ControlFileTransaction {
    public:
        ~ControlFileTransaction() {
            if (finalized_) return;
            try {
                (void)rollback();
            } catch (...) {
                // Destructors must not throw while unwinding another initialization failure.
            }
        }

        bool write(const std::string& path, const std::string& value,
                   bool rollback_required = true) {
            if (rollback_required && original_values_.find(path) == original_values_.end()) {
                const std::string original = FileUtils::read_file(path);
                if (original.empty()) {
                    LOG_W("CgroupInit", "Cannot snapshot control file before update: " + path);
                    return false;
                }
                original_values_.emplace(path, original);
                write_order_.push_back(path);
            }
            return FileUtils::write_kernel_control_file(path, value);
        }

        bool rollback() {
            if (finalized_) return true;
            finalized_ = true;
            bool success = true;
            for (auto it = write_order_.rbegin(); it != write_order_.rend(); ++it) {
                const auto original = original_values_.find(*it);
                if (original == original_values_.end()) continue;
                if (!FileUtils::write_kernel_control_file(*it, original->second)) {
                    LOG_E("CgroupInit", "Failed to roll back control file: " + *it);
                    success = false;
                }
            }
            return success;
        }

        void commit() {
            finalized_ = true;
        }

    private:
        bool finalized_ = false;
        std::map<std::string, std::string> original_values_;
        std::vector<std::string> write_order_;
    };

public:
    static bool init(const Config& config) {
        ControlFileTransaction transaction;
        if (!init_cpuset(config, transaction) || !init_cpuctl(config, transaction)) {
            LOG_W("CgroupInit", "Cgroup initialization failed; rolling back control values");
            if (!transaction.rollback()) {
                LOG_E("CgroupInit", "Cgroup control value rollback was incomplete");
            }
            return false;
        }
        report_stale_groups(config);
        transaction.commit();
        return true;
    }

    static std::string resolve_limit_value(const std::optional<int>& configured,
                                           const std::string& inherited) {
        return configured ? std::to_string(*configured) : inherited;
    }

    static bool cpuset_group_requires_rollback(const std::string& cpus,
                                               const std::string& mems) {
        return !cpus.empty() && !mems.empty();
    }

    static std::string cpuset_parent_path() {
        return "/dev/cpuset/top-app";
    }

    static std::string cpuset_group_path(const std::string& mask_name) {
        return cpuset_parent_path() + "/ReUperf_" + mask_name;
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
        const std::string cpuset_parent = cpuset_parent_path();
        std::set<std::string> expected_cpuset;
        for (const auto& [name, cpus] : config.sched.cpumask) {
            (void)cpus;
            expected_cpuset.insert("ReUperf_" + name);
        }
        for (const auto& name : list_child_directories(cpuset_parent)) {
            if (name.rfind("ReUperf_", 0) == 0 && expected_cpuset.count(name) == 0) {
                LOG_W("CgroupInit", "Stale cpuset group retained for safety: "
                      + cpuset_parent + "/" + name);
            }
        }
        for (const auto& name : list_child_directories("/dev/cpuset")) {
            if (name.rfind("ReUperf_", 0) == 0) {
                LOG_W("CgroupInit", "Legacy root cpuset group retained for safety: /dev/cpuset/"
                      + name);
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

    static bool write_optional_limit(ControlFileTransaction& transaction,
                                     const std::string& child_path,
                                     const std::string& parent_path,
                                     const std::string& filename,
                                     const std::optional<int>& configured) {
        const std::string child_file = child_path + "/" + filename;
        if (!FileUtils::file_exists(child_file)) {
            if (configured) {
                LOG_W("CgroupInit", "Required control file not found: " + child_file);
                return false;
            }
            return true;
        }

        std::string inherited;
        if (!configured) {
            inherited = FileUtils::read_file(parent_path + "/" + filename);
            if (inherited.empty()) {
                LOG_W("CgroupInit", "Cannot read inherited value for " + child_file);
                return false;
            }
        }
        const std::string desired = resolve_limit_value(configured, inherited);
        if (!transaction.write(child_file, desired)) {
            LOG_W("CgroupInit", "Failed to configure " + child_file);
            return false;
        }
        return true;
    }

    static bool init_cpuset(const Config& config, ControlFileTransaction& transaction) {
        LOG_I("CgroupInit", "Initializing cpuset cgroups...");

        if (!FileUtils::dir_exists("/dev/cpuset")) {
            LOG_W("CgroupInit", "/dev/cpuset not found, skipping cpuset init");
            return true;
        }
        const std::string base_path = cpuset_parent_path();
        if (!FileUtils::dir_exists(base_path)
            || !FileUtils::file_exists(base_path + "/cpus")) {
            LOG_E("CgroupInit", "top-app cpuset is unavailable: " + base_path);
            return false;
        }

        const std::string parent_cpus = FileUtils::read_file(base_path + "/cpus");
        LOG_I("CgroupInit", "top-app parent CPUs: " + parent_cpus);
        if (parent_cpus.empty()) {
            LOG_E("CgroupInit", "top-app cpuset CPUs are empty or unreadable");
            return false;
        }

        const std::string parent_mems = FileUtils::read_file(base_path + "/mems");
        if (parent_mems.empty()) {
            LOG_E("CgroupInit", "top-app cpuset mems is empty or unreadable");
            return false;
        }
        int created = 0;
        int failed = 0;

        for (const auto& cpumask : config.sched.cpumask) {
            const std::string child_path = cpuset_group_path(cpumask.first);
            const bool group_was_ready = FileUtils::dir_exists(child_path)
                && cpuset_group_requires_rollback(
                    FileUtils::read_file(child_path + "/cpus"),
                    FileUtils::read_file(child_path + "/mems"));
            if (!FileUtils::mkdir_recursive(child_path)) {
                LOG_W("CgroupInit", "Failed to create cpuset: " + child_path);
                ++failed;
                continue;
            }
            if (!ensure_cgroup_file_exists(child_path + "/cpus")
                || !ensure_cgroup_file_exists(child_path + "/mems")
                || !ensure_cgroup_file_exists(child_path + "/tasks")) {
                LOG_W("CgroupInit", "Required cpuset control files missing for " + child_path);
                ++failed;
                continue;
            }

            const std::string cpus_str = CpuMask::to_string(cpumask.second);
            if (cpus_str.empty()) {
                LOG_W("CgroupInit", "Empty cpumask for " + cpumask.first);
                ++failed;
                continue;
            }
            if (!transaction.write(child_path + "/cpus", cpus_str, group_was_ready)) {
                LOG_W("CgroupInit", "Failed to set cpus for " + child_path
                      + " (cpus=" + cpus_str + ")");
                ++failed;
                continue;
            }
            if (!transaction.write(child_path + "/mems", parent_mems, group_was_ready)) {
                LOG_W("CgroupInit", "Failed to set inherited mems for " + child_path);
                ++failed;
                continue;
            }

            bool exclusive_ready = true;
            for (const char* filename : {"cpu_exclusive", "mem_exclusive"}) {
                const std::string control_file = child_path + "/" + filename;
                if (FileUtils::file_exists(control_file)
                    && !transaction.write(control_file, "0", group_was_ready)) {
                    LOG_W("CgroupInit", "Failed to disable " + std::string(filename)
                          + " for " + child_path);
                    exclusive_ready = false;
                }
            }
            if (!exclusive_ready) {
                ++failed;
                continue;
            }
            ++created;
        }

        LOG_I("CgroupInit", "top-app cpuset cgroups: " + std::to_string(created)
              + " groups created, " + std::to_string(failed) + " failed");
        return failed == 0;
    }

    static bool init_cpuctl(const Config& config, ControlFileTransaction& transaction) {
        LOG_I("CgroupInit", "Initializing cpuctl cgroups...");

        if (!FileUtils::dir_exists("/dev/cpuctl")) {
            LOG_W("CgroupInit", "/dev/cpuctl not found, skipping cpuctl init");
            return true;
        }

        const std::string reuperf_path = "/dev/cpuctl/ReUperf";
        if (!FileUtils::mkdir_recursive(reuperf_path)) {
            LOG_W("CgroupInit", "Failed to create " + reuperf_path);
            return false;
        }

        int failed = 0;
        for (const auto& rule : config.sched.rules) {
            const std::string rule_path = reuperf_path + "/" + rule.name;
            if (!FileUtils::mkdir_recursive(rule_path)) {
                LOG_W("CgroupInit", "Failed to create cpuctl: " + rule_path);
                ++failed;
                continue;
            }

            for (size_t i = 0; i < rule.thread_rules.size(); ++i) {
                const auto& thread_rule = rule.thread_rules[i];
                if (!thread_rule.enable_limit) continue;

                const std::string sub_path = rule_path + "/A" + std::to_string(i + 1);
                if (!FileUtils::mkdir_recursive(sub_path)) {
                    LOG_W("CgroupInit", "Failed to create cpuctl: " + sub_path);
                    ++failed;
                    continue;
                }

                bool configured = true;
                configured &= write_optional_limit(transaction, sub_path, rule_path,
                                                   "cpu.uclamp.max", thread_rule.uclamp_max);
                configured &= write_optional_limit(transaction, sub_path, rule_path,
                                                   "cpu.shares", thread_rule.cpu_share);
                if (!configured) {
                    ++failed;
                    continue;
                }
                LOG_D("CgroupInit", "Configured cpuctl sub-group: " + sub_path);
            }
        }

        LOG_I("CgroupInit", "cpuctl cgroups initialized, " + std::to_string(failed)
              + " failures");
        return failed == 0;
    }
};

#endif