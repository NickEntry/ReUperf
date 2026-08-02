#ifndef SCAN_CURSOR_HPP
#define SCAN_CURSOR_HPP

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <dirent.h>
#include <string>
#include <thread>
#include <vector>
#include "../utils/file_utils.hpp"
#include "../utils/logger.hpp"

struct ScanBudget {
    explicit ScanBudget(int budget_us, int configured_batch_size,
                        int configured_batch_yield_us)
        : deadline(std::chrono::steady_clock::now() + std::chrono::microseconds(budget_us)),
          batch_size(std::max(configured_batch_size, 1)),
          batch_yield_us(std::max(configured_batch_yield_us, 0)) {}

    bool exhausted() const {
        return std::chrono::steady_clock::now() >= deadline;
    }

    void after_thread_read() {
        ++thread_reads;
        if (batch_yield_us > 0 && thread_reads % static_cast<size_t>(batch_size) == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(batch_yield_us));
        }
    }

    std::chrono::steady_clock::time_point deadline;
    int batch_size;
    int batch_yield_us;
    size_t thread_reads = 0;
};

struct ScanCursor {
    int pid = 0;
    DIR* task_dir = nullptr;
    bool tid_enumeration_complete = false;
    std::vector<int> tids;
    size_t tid_index = 0;
    bool process_name_loaded = false;
    std::string process_name;
    bool process_start_time_loaded = false;
    uint64_t process_start_time = 0;
    bool cmdline_loaded = false;
    std::string cmdline;

    ScanCursor() = default;
    ScanCursor(const ScanCursor&) = delete;
    ScanCursor& operator=(const ScanCursor&) = delete;

    ~ScanCursor() {
        reset();
    }

    void reset() {
        if (task_dir != nullptr) {
            closedir(task_dir);
            task_dir = nullptr;
        }
        pid = 0;
        tid_enumeration_complete = false;
        tids.clear();
        tid_index = 0;
        process_name_loaded = false;
        process_name.clear();
        process_start_time_loaded = false;
        process_start_time = 0;
        cmdline_loaded = false;
        cmdline.clear();
    }

    void begin_pid(int next_pid) {
        reset();
        pid = next_pid;
    }

    bool enumerate_tids(ScanBudget& budget) {
        if (tid_enumeration_complete) return true;
        if (pid <= 0) return false;
        if (task_dir == nullptr) {
            const std::string task_path = "/proc/" + std::to_string(pid) + "/task";
            task_dir = opendir(task_path.c_str());
            if (task_dir == nullptr) {
                tid_enumeration_complete = true;
                return true;
            }
            tids.clear();
        }

        while (!budget.exhausted()) {
            errno = 0;
            const dirent* entry = readdir(task_dir);
            if (entry == nullptr) {
                const int read_error = errno;
                closedir(task_dir);
                task_dir = nullptr;
                if (read_error != 0) {
                    LOG_W("ScanCursor", "Failed while enumerating tasks for pid "
                          + std::to_string(pid) + ": " + std::string(strerror(read_error)));
                    tids.clear();
                }
                std::sort(tids.begin(), tids.end());
                tids.erase(std::unique(tids.begin(), tids.end()), tids.end());
                tid_enumeration_complete = true;
                tid_index = 0;
                return true;
            }

            budget.after_thread_read();
            if (!FileUtils::is_all_digits(entry->d_name)) continue;
            errno = 0;
            char* end = nullptr;
            const long parsed_tid = strtol(entry->d_name, &end, 10);
            if (errno == 0 && end != entry->d_name && *end == '\0'
                && FileUtils::is_valid_tid(parsed_tid)) {
                tids.push_back(static_cast<int>(parsed_tid));
            }
        }
        return false;
    }
};

#endif
