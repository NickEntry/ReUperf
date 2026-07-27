#ifndef CPUCTL_SETTER_HPP
#define CPUCTL_SETTER_HPP

#include <string>
#include <optional>
#include "../config/config_types.hpp"
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"

class CpuctlSetter {
public:
    CpuctlSetter() = default;

    // Returns the base cpuctl path (kept for ThreadCache compatibility)
    std::string get_cpuctl_base(ProcessState /*state*/) {
        return "/dev/cpuctl";
    }
    
    bool apply_with_result(int /*pid*/, int tid, const MatchResult& result) {
        if (!result.matched || !result.enable_limit) {
            return true;
        }
        
        if (result.thread_rule_index < 0) {
            LOG_W("CpuctlSetter", "enable_limit set but thread_rule_index is -1 for tid "
                  + std::to_string(tid) + ", rule=" + result.matched_rule_name);
            return false;
        }

        // Path: /dev/cpuctl/ReUperf/{rule_name}/A{thread_rule_index+1}
        std::string group_path = "/dev/cpuctl/ReUperf/" + result.matched_rule_name
                               + "/A" + std::to_string(result.thread_rule_index + 1);
        
        return migrate_thread(tid, group_path);
    }

private:
    static bool migrate_thread(int tid, const std::string& group_path) {
        if (!FileUtils::dir_exists(group_path)) {
            LOG_W("CpuctlSetter", "cpuctl group not exists: " + group_path);
            return false;
        }
        FileUtils::write_cgroup_procs("/dev/cpuctl/ReUperf", tid);
        if (!FileUtils::write_cgroup_procs(group_path, tid)) {
            LOG_W("CpuctlSetter", "Failed to move tid " + std::to_string(tid) + " to " + group_path);
            return false;
        }
        return true;
    }
};

#endif