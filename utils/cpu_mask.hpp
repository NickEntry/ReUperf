#ifndef CPU_MASK_HPP
#define CPU_MASK_HPP

#include <vector>
#include <string>
#include <sstream>
#include <algorithm>
#include <sched.h>
#include <cstring>
#include <dirent.h>
#include <cctype>
#include "logger.hpp"
#include "file_utils.hpp"

class CpuMask {
public:
    static cpu_set_t from_vector(const std::vector<int>& cpus) {
        cpu_set_t set;
        CPU_ZERO(&set);
        for (int cpu : cpus) {
            if (cpu >= 0 && cpu < CPU_SETSIZE) {
                CPU_SET(cpu, &set);
            }
        }
        return set;
    }

    static std::vector<int> to_vector(const cpu_set_t& set) {
        std::vector<int> cpus;
        for (int i = 0; i < CPU_SETSIZE; ++i) {
            if (CPU_ISSET(i, &set)) {
                cpus.push_back(i);
            }
        }
        return cpus;
    }

    static std::string to_string(const std::vector<int>& cpus) {
        if (cpus.empty()) return "";
        
        std::vector<int> sorted = cpus;
        std::sort(sorted.begin(), sorted.end());
        
        std::ostringstream oss;
        for (size_t i = 0; i < sorted.size(); ++i) {
            if (i > 0) {
                if (sorted[i] == sorted[i - 1] + 1) {
                    if (i + 1 < sorted.size() && sorted[i + 1] == sorted[i] + 1) {
                        continue;
                    }
                    oss << "-" << sorted[i];
                } else {
                    oss << "," << sorted[i];
                }
            } else {
                oss << sorted[i];
            }
        }
        return oss.str();
    }

    static std::string to_string(const cpu_set_t& set) {
        return to_string(to_vector(set));
    }

    static bool set_affinity(int tid, const cpu_set_t& set) {
        if (sched_setaffinity(tid, sizeof(cpu_set_t), &set) != 0) {
            LOG_E("CpuMask", "sched_setaffinity failed for tid " + std::to_string(tid) 
                  + ": " + std::string(strerror(errno)));
            return false;
        }
        return true;
    }

    static bool set_affinity(int tid, const std::vector<int>& cpus) {
        if (cpus.empty()) return true;
        return set_affinity(tid, from_vector(cpus));
    }

    static std::vector<int> get_affinity_from_status(int tid) {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(tid, sizeof(cpu_set_t), &set) == 0) {
            return to_vector(set);
        }

        std::string status_path = "/proc/" + std::to_string(tid) + "/status";
        std::string content = FileUtils::read_file(status_path);
        if (content.empty()) {
            LOG_W("CpuMask", "Cannot read status for tid " + std::to_string(tid));
            return {};
        }
        
        std::istringstream iss(content);
        std::string line;
        
        while (std::getline(iss, line)) {
            if (line.compare(0, 17, "Cpus_allowed_list") == 0) {
                size_t pos = line.find(':');
                if (pos == std::string::npos) continue;
                
                std::string value = line.substr(pos + 1);
                size_t start = 0;
                while (start < value.size() && (value[start] == ' ' || value[start] == '\t')) {
                    start++;
                }
                
                std::vector<int> cpus;
                size_t i = 0;
                while (i < value.size()) {
                    while (i < value.size() && (value[i] == ' ' || value[i] == '\t')) {
                        i++;
                    }
                    if (i >= value.size()) break;
                    
                    size_t dash_pos = value.find('-', i);
                    size_t comma_pos = value.find(',', i);
                    size_t end_pos = (comma_pos == std::string::npos) ? value.size() : comma_pos;
                    
                    if (dash_pos != std::string::npos && dash_pos > i && dash_pos < end_pos) {
                        int start_cpu = std::stoi(value.substr(i, dash_pos - i));
                        int end_cpu = std::stoi(value.substr(dash_pos + 1, end_pos - dash_pos - 1));
                        for (int cpu = start_cpu; cpu <= end_cpu; cpu++) {
                            cpus.push_back(cpu);
                        }
                        i = end_pos;
                    } else {
                        int cpu = std::stoi(value.substr(i, end_pos - i));
                        cpus.push_back(cpu);
                        i = end_pos;
                    }
                    
                    if (comma_pos != std::string::npos) {
                        i = comma_pos + 1;
                    }
                }
                return cpus;
            }
        }
        
        LOG_W("CpuMask", "Cpus_allowed_list not found in status for tid " + std::to_string(tid));
        return {};
    }
    
    static bool is_affinity_changed_from_status(int tid, const std::vector<int>& expected) {
        auto current = get_affinity_from_status(tid);
        if (current.empty()) return true;
        std::vector<int> sorted_current = current;
        std::vector<int> sorted_expected = expected;
        std::sort(sorted_current.begin(), sorted_current.end());
        std::sort(sorted_expected.begin(), sorted_expected.end());
        return sorted_current != sorted_expected;
    }
    
    static int get_cpu_count() {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof(cpu_set_t), &set) != 0) {
            LOG_W("CpuMask", "sched_getaffinity failed");
            return 0;
        }
        return CPU_COUNT(&set);
    }

    static std::vector<int> detect_all_cpus() {
        std::vector<int> cpus;
        const char* sysfs_path = "/sys/devices/system/cpu";
        DIR* dir = opendir(sysfs_path);
        if (!dir) {
            LOG_W("CpuMask", "Cannot open " + std::string(sysfs_path));
            return cpus;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type != DT_DIR) continue;

            const char* name = entry->d_name;
            size_t len = strlen(name);

            if (len < 4 || strncmp(name, "cpu", 3) != 0) continue;

            bool all_digits = true;
            for (size_t i = 3; i < len; ++i) {
                if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
                    all_digits = false;
                    break;
                }
            }
            if (!all_digits) continue;

            int cpu_num = atoi(name + 3);
            if (cpu_num >= 0) {
                cpus.push_back(cpu_num);
            }
        }
        closedir(dir);

        std::sort(cpus.begin(), cpus.end());
        return cpus;
    }

    static std::string get_all_cpus_string() {
        auto cpus = detect_all_cpus();
        if (cpus.empty()) {
            LOG_W("CpuMask", "No CPUs detected, falling back to sched_getaffinity count");
            int count = get_cpu_count();
            for (int i = 0; i < count; i++) {
                cpus.push_back(i);
            }
        }
        return to_string(cpus);
    }
};

#endif
