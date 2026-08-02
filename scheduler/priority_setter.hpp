#ifndef PRIORITY_SETTER_HPP
#define PRIORITY_SETTER_HPP

#include <string>
#include <sstream>
#include <cerrno>
#include <sched.h>
#include <cstring>
#include <sys/resource.h>
#include <optional>
#include "../config/config_types.hpp"
#include "../utils/logger.hpp"
#include "../core/thread_matcher.hpp"
#include "../utils/file_utils.hpp"

struct CurrentSched {
    int policy;
    int prio;
};

class PrioritySetter {
public:
    PrioritySetter(ThreadMatcher& matcher)
        : matcher_(matcher) {}

    bool capture_scheduler_baseline(int tid, int& policy, int& priority, int& nice) {
        policy = sched_getscheduler(tid);
        priority = 0;
        nice = 0;
        if (policy < 0) {
            return false;
        }

        sched_param param{};
        if (sched_getparam(tid, &param) != 0) {
            return false;
        }
        priority = param.sched_priority;
        if (policy == normal_policy() || policy == batch_policy() || policy == idle_policy()) {
            errno = 0;
            nice = getpriority(PRIO_PROCESS, tid);
            if (errno != 0) {
                return false;
            }
        }
        return true;
    }

    CurrentSched get_current_sched(int tid) {
        CurrentSched curr;
        curr.policy = sched_getscheduler(tid);
        curr.prio = 0;

        if (curr.policy < 0) {
            return curr;
        }

        sched_param param{};
        if (sched_getparam(tid, &param) == 0) {
            curr.prio = param.sched_priority;
        }

        if (curr.policy == normal_policy() || curr.policy == batch_policy() || curr.policy == idle_policy()) {
            errno = 0;
            int nice = getpriority(PRIO_PROCESS, tid);
            if (errno == 0) {
                curr.prio = nice + 120;
            }
        }

        return curr;
    }

    static std::optional<int> expected_policy_for_priority(int expected_prio) {
        if (expected_prio == 0) {
            return std::nullopt;
        }
        if (expected_prio >= 1 && expected_prio <= 98) {
            return SCHED_FIFO;
        }
        if (expected_prio >= 100 && expected_prio <= 139) {
            return normal_policy();
        }
        if (expected_prio == -1) {
            return normal_policy();
        }
        if (expected_prio == -2) {
            return batch_policy();
        }
        if (expected_prio == -3) {
            return idle_policy();
        }
        return std::nullopt;
    }

    bool is_sched_changed(int tid, int expected_prio) {
        auto curr = get_current_sched(tid);
        if (curr.policy < 0) {
            return true;
        }

        const auto expected_policy = expected_policy_for_priority(expected_prio);
        if (expected_policy && curr.policy != *expected_policy) {
            return true;
        }

        if (expected_prio >= 1 && expected_prio <= 98) {
            return curr.prio != expected_prio;
        }
        if (expected_prio >= 100 && expected_prio <= 139) {
            return curr.prio != expected_prio;
        }
        return false;
    }

    bool set_priority(int tid, int prio_value) {
        if (tid <= 0) {
            LOG_W("PrioritySetter", "Invalid tid: " + std::to_string(tid));
            return false;
        }
        
        if (prio_value == 0) {
            return true;
        }
        
        struct sched_param param;
        
        if (prio_value >= 1 && prio_value <= 98) {
            param.sched_priority = prio_value;
            if (sched_setscheduler(tid, SCHED_FIFO, &param) != 0) {
                int err = errno;
                if (err == EPERM || err == EACCES) {
                    LOG_W("PrioritySetter", "Permission denied to set SCHED_FIFO for tid " 
                          + std::to_string(tid) + " (need CAP_SYS_NICE)");
                } else {
                    LOG_W("PrioritySetter", "Failed to set SCHED_FIFO for tid " 
                          + std::to_string(tid) + " prio=" + std::to_string(prio_value)
                          + ": " + std::string(strerror(err)));
                }
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) 
                  + " to SCHED_FIFO prio=" + std::to_string(prio_value));
        } else if (prio_value >= 100 && prio_value <= 139) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, normal_policy(), &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_NORMAL for tid " 
                      + std::to_string(tid));
                return false;
            }
            if (!set_nice_value(tid, prio_value - 120)) {
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid)
                  + " to SCHED_NORMAL nice=" + std::to_string(prio_value - 120));
        } else if (prio_value == -1) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, normal_policy(), &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_NORMAL for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_NORMAL");
        } else if (prio_value == -2) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, batch_policy(), &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_BATCH for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_BATCH");
        } else if (prio_value == -3) {
            param.sched_priority = 0;
            if (sched_setscheduler(tid, idle_policy(), &param) != 0) {
                LOG_W("PrioritySetter", "Failed to set SCHED_IDLE for tid " 
                      + std::to_string(tid));
                return false;
            }
            LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " to SCHED_IDLE");
        } else {
            LOG_W("PrioritySetter", "Unknown prio_value " + std::to_string(prio_value) 
                  + " for tid " + std::to_string(tid) + ", skipping");
            return false;
        }
        
        return true;
    }
    
    bool restore_scheduler(int tid, int policy, int priority, int nice) {
        if (tid <= 0 || policy < 0) {
            return false;
        }

        sched_param param{};
        param.sched_priority = priority;
        if (sched_setscheduler(tid, policy, &param) != 0) {
            const int error = errno;
            LOG_W("PrioritySetter", "Failed to restore scheduler for tid " + std::to_string(tid)
                  + " (errno=" + std::to_string(error) + ", "
                  + std::string(strerror(error)) + ")");
            return false;
        }

        if (policy == normal_policy() || policy == batch_policy() || policy == idle_policy()) {
            if (!set_nice_value(tid, nice)) {
                return false;
            }
        }
        return true;
    }

    bool apply_with_result(int pid, int tid, const MatchResult& result) {
        (void)pid;
        if (!result.matched) {
            return true;
        }
        
        int prio = matcher_.get_prio_value(result.prio_class, result.effective_state);
        
        if (prio == 0) {
            return true;
        }
        
        return set_priority(tid, prio);
    }

private:
    ThreadMatcher& matcher_;

    static int normal_policy() {
#ifdef SCHED_NORMAL
        return SCHED_NORMAL;
#else
        return SCHED_OTHER;
#endif
    }

    static int batch_policy() {
#ifdef SCHED_BATCH
        return SCHED_BATCH;
#else
        LOG_W("PrioritySetter", "SCHED_BATCH not available on this platform, falling back to SCHED_OTHER");
        return normal_policy();
#endif
    }

    static int idle_policy() {
#ifdef SCHED_IDLE
        return SCHED_IDLE;
#else
        LOG_W("PrioritySetter", "SCHED_IDLE not available on this platform, falling back to SCHED_OTHER");
        return normal_policy();
#endif
    }
    
    bool set_nice_value(int tid, int nice) {
        if (setpriority(PRIO_PROCESS, tid, nice) != 0) {
            LOG_W("PrioritySetter", "Failed to set nice " + std::to_string(nice) 
                  + " for tid " + std::to_string(tid));
            return false;
        }
        LOG_T("PrioritySetter", "Set tid " + std::to_string(tid) + " nice=" + std::to_string(nice));
        return true;
    }
};

#endif
