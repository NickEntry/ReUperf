#ifndef CPUCTL_SETTER_HPP
#define CPUCTL_SETTER_HPP

#include <string>
#include <optional>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"
// ✅ 引入 CgroupInitializer 以获取 uclamp 支持状态
#include "../core/cgroup_init.hpp"

class CpuctlSetter {
public:
    CpuctlSetter() = default;

    // ✅ P1 改进：添加统计信息
    struct Stats {
        int uclamp_success_count = 0;
        int uclamp_failure_count = 0;
        int cpuctl_write_count = 0;
    };
    
    const Stats& get_stats() const { return stats_; }
    
    void reset_stats() { stats_ = Stats(); }
    
    std::string print_stats() const {
        return "CpuctlSetter Stats: uclamp_success=" + std::to_string(stats_.uclamp_success_count) +
               ", uclamp_failure=" + std::to_string(stats_.uclamp_failure_count) +
               ", cpuctl_write=" + std::to_string(stats_.cpuctl_write_count);
    }

private:
    mutable Stats stats_;
    
public:

    bool set_uclamp_max(int tid, int uclamp_max, const std::string& /*cpuctl_base*/,
                        const std::string& rule_name) {
        if (tid <= 0) {
            LOG_W("CpuctlSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        // ✅ 零开销防护：内核不支持 uclamp 时直接返回 true，跳过无效 IO 与日志
        if (!CgroupInitializer::uclamp_supported) {
            return true;
        }
        
        if (uclamp_max < 0 || uclamp_max > 100) {
            LOG_W("CpuctlSetter", "Invalid uclamp_max: " + std::to_string(uclamp_max)
                  + " (valid range: 0-100)");
            return false;
        }
        
        std::string path = "/dev/cpuctl/ReUperf/" + rule_name;
        if (!FileUtils::dir_exists(path)) {
            LOG_W("CpuctlSetter", "cpuctl group not exists: " + path);
            return false;
        }
        
        if (!FileUtils::write_file(path + "/cpu.uclamp.max", std::to_string(uclamp_max))) {
            LOG_W("CpuctlSetter", "Failed to write cpu.uclamp.max for " + path);
            stats_.uclamp_failure_count++;
            return false;
        }
        
        LOG_T("CpuctlSetter", "Set cpu.uclamp.max=" + std::to_string(uclamp_max) + " for " + path);
        stats_.uclamp_success_count++;
        stats_.cpuctl_write_count++;
        
        // 先写入父组 /dev/cpuctl/ReUperf/tasks
        FileUtils::write_cgroup_procs("/dev/cpuctl/ReUperf", tid);
        if (!FileUtils::write_cgroup_procs(path, tid)) {
            LOG_W("CpuctlSetter", "Failed to move tid " + std::to_string(tid) + " to " + path);
            return false;
        }
        return true;
    }

    bool set_cpu_share(int tid, int cpu_share, const std::string& /*cpuctl_base*/,
                       const std::string& rule_name) {
        if (tid <= 0) {
            LOG_W("CpuctlSetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        // Android-specific: uses 0-1024 instead of standard cgroup 2-262144
        if (cpu_share < 0 || cpu_share > 1024) {
            LOG_W("CpuctlSetter", "Invalid cpu_share: " + std::to_string(cpu_share)
                  + " (valid range: 0-1024, Android-specific)");
            return false;
        }
        
        std::string path = "/dev/cpuctl/ReUperf/" + rule_name;
        if (!FileUtils::dir_exists(path)) {
            LOG_W("CpuctlSetter", "cpuctl group not exists: " + path);
            return false;
        }
        
        if (!FileUtils::write_file(path + "/cpu.shares", std::to_string(cpu_share))) {
            LOG_W("CpuctlSetter", "Failed to write cpu.shares for " + path);
            return false;
        }
        
        LOG_T("CpuctlSetter", "Set cpu.shares=" + std::to_string(cpu_share) + " for " + path);
        stats_.cpuctl_write_count++;
        
        FileUtils::write_cgroup_procs("/dev/cpuctl/ReUperf", tid);
        if (!FileUtils::write_cgroup_procs(path, tid)) {
            LOG_W("CpuctlSetter", "Failed to move tid " + std::to_string(tid) + " to " + path);
            return false;
        }
        return true;
    }

    std::string get_cpuctl_base(ProcessState /*state*/) {
        return "/dev/cpuctl";
    }

    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result) {
        if (!result.matched || !result.enable_limit) {
            return true;
        }
        
        std::string base = get_cpuctl_base(result.effective_state);
        // uclamp 写入已内置支持检测，不支持时自动静默跳过
        if (result.uclamp_max.has_value()) {
            if (!set_uclamp_max(tid, result.uclamp_max.value(),
                                base, result.matched_rule_name)) {
                return false;
            }
        }
        
        // cpu.shares 始终有效（cgroup v1 标准控制器）
        if (result.cpu_share.has_value()) {
            if (!set_cpu_share(tid, result.cpu_share.value(),
                               base, result.matched_rule_name)) {
                return false;
            }
        }
        return true;
    }

private:
};

#endif // CPUCTL_SETTER_HPP