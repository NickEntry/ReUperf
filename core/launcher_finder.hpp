#ifndef LAUNCHER_FINDER_HPP
#define LAUNCHER_FINDER_HPP

#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include "../utils/logger.hpp"

class LauncherFinder {
private:
    static bool is_home_package(const std::string& pkg) {
        std::string lower;
        lower.reserve(pkg.size());
        for (char c : pkg) {
            lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower.find("home") != std::string::npos || 
               lower.find("launcher") != std::string::npos;
    }

    static bool is_valid_package(const std::string& pkg) {
        if (pkg.empty() || pkg.length() > 128 || pkg.front() == '.' || pkg.back() == '.') {
            return false;
        }
        bool segment_start = true;
        bool has_separator = false;
        for (const char raw_character : pkg) {
            const unsigned char character = static_cast<unsigned char>(raw_character);
            if (character == '.') {
                if (segment_start) return false;
                segment_start = true;
                has_separator = true;
                continue;
            }
            if (segment_start) {
                if (!std::isalpha(character)) return false;
                segment_start = false;
            } else if (!std::isalnum(character) && character != '_') {
                return false;
            }
        }
        return has_separator && !segment_start;
    }

public:
    static std::string find() {
        LOG_I("LauncherFinder", "Finding launcher package...");
        
        std::string result = execute_command(
            "cmd package resolve-activity -a android.intent.action.MAIN "
            "-c android.intent.category.HOME 2>/dev/null | grep 'packageName='",
            5000);
        
        std::vector<std::string> candidates;
        if (!result.empty()) {
            std::istringstream ss(result);
            std::string line;
            while (std::getline(ss, line)) {
                size_t pos = line.find("packageName=");
                if (pos != std::string::npos) {
                    std::string pkg = line.substr(pos + 12);
                    while (!pkg.empty() && (pkg.back() == '\n' || pkg.back() == '\r')) {
                        pkg.pop_back();
                    }
                    if (is_valid_package(pkg)) {
                        candidates.push_back(pkg);
                    }
                }
            }
        }
        
        if (candidates.empty()) {
            result = execute_command(
                "cmd package query-activities -a android.intent.action.MAIN "
                "-c android.intent.category.HOME 2>/dev/null | grep 'packageName='",
                5000);
            
            if (!result.empty()) {
                std::istringstream ss(result);
                std::string line;
                while (std::getline(ss, line)) {
                    size_t pos = line.find("packageName=");
                    if (pos != std::string::npos) {
                        std::string pkg = line.substr(pos + 12);
                        while (!pkg.empty() && (pkg.back() == '\n' || pkg.back() == '\r')) {
                            pkg.pop_back();
                        }
                        if (is_valid_package(pkg)) {
                            candidates.push_back(pkg);
                        }
                    }
                }
            }
        }
        
        std::string non_com_android_home;
        std::string com_android_home;
        std::string other_pkg;
        
        for (const auto& pkg : candidates) {
            bool is_home = is_home_package(pkg);
            bool starts_com_android = (pkg.find("com.android") == 0);
            
            if (is_home && !starts_com_android) {
                if (non_com_android_home.empty()) {
                    non_com_android_home = pkg;
                }
            } else if (is_home && starts_com_android) {
                if (com_android_home.empty()) {
                    com_android_home = pkg;
                }
            } else if (other_pkg.empty()) {
                other_pkg = pkg;
            }
        }
        
        if (!non_com_android_home.empty()) {
            LOG_I("LauncherFinder", "Found launcher (non-com.android home): " + non_com_android_home);
            return non_com_android_home;
        }
        if (!com_android_home.empty()) {
            LOG_I("LauncherFinder", "Found launcher (com.android home): " + com_android_home);
            return com_android_home;
        }
        if (!other_pkg.empty()) {
            LOG_I("LauncherFinder", "Found launcher (other): " + other_pkg);
            return other_pkg;
        }
        
        result = "com.miui.home";
        LOG_W("LauncherFinder", "Could not find launcher, using default: " + result);
        return result;
    }

private:
    static constexpr size_t kMaxOutputSize = 64 * 1024;  // 64KB max output

    static std::string execute_command(const std::string& cmd, int timeout_ms) {
        int pipe_fds[2];
        if (pipe(pipe_fds) != 0) {
            LOG_E("LauncherFinder", "pipe failed: " + std::string(strerror(errno)));
            return "";
        }

        pid_t child_pid = fork();
        if (child_pid < 0) {
            LOG_E("LauncherFinder", "fork failed: " + std::string(strerror(errno)));
            close(pipe_fds[0]);
            close(pipe_fds[1]);
            return "";
        }

        if (child_pid == 0) {
            // Isolate the shell and its pipeline children for timeout cleanup.
            if (setpgid(0, 0) != 0) {
                _exit(127);
            }
            close(pipe_fds[0]);
            if (dup2(pipe_fds[1], STDOUT_FILENO) < 0) {
                _exit(127);
            }
            close(pipe_fds[1]);
            execl("/system/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }

        // Ensure the child has its own process group before timeout handling.
        // The child also calls setpgid(), so EACCES/ESRCH here can be ignored.
        if (setpgid(child_pid, child_pid) != 0 && errno != EACCES && errno != ESRCH) {
            LOG_W("LauncherFinder", "setpgid failed: " + std::string(strerror(errno)));
        }

        close(pipe_fds[1]);
        const int read_fd = pipe_fds[0];
        const int old_flags = fcntl(read_fd, F_GETFL, 0);
        if (old_flags < 0 || fcntl(read_fd, F_SETFL, old_flags | O_NONBLOCK) < 0) {
            LOG_E("LauncherFinder", "fcntl failed: " + std::string(strerror(errno)));
            close(read_fd);
            terminate_child(child_pid);
            return "";
        }

        std::string result;
        bool timed_out = false;
        bool output_too_large = false;
        bool io_failed = false;
        bool pipe_closed = false;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        char buffer[4096];

        while (!pipe_closed) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                timed_out = true;
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            struct pollfd pfd{read_fd, POLLIN | POLLHUP, 0};
            const int poll_result = poll(&pfd, 1, static_cast<int>(remaining));
            if (poll_result == 0) {
                timed_out = true;
                break;
            }
            if (poll_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_E("LauncherFinder", "poll failed: " + std::string(strerror(errno)));
                io_failed = true;
                break;
            }

            while (true) {
                const ssize_t bytes_read = read(read_fd, buffer, sizeof(buffer));
                if (bytes_read > 0) {
                    if (result.size() + static_cast<size_t>(bytes_read) > kMaxOutputSize) {
                        output_too_large = true;
                        break;
                    }
                    result.append(buffer, static_cast<size_t>(bytes_read));
                    continue;
                }
                if (bytes_read == 0) {
                    pipe_closed = true;
                    break;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                if (errno == EINTR) {
                    continue;
                }
                LOG_E("LauncherFinder", "read failed: " + std::string(strerror(errno)));
                pipe_closed = true;
                break;
            }
            if (output_too_large) {
                break;
            }
        }

        close(read_fd);
        if (timed_out || output_too_large || io_failed) {
            if (timed_out) {
                LOG_W("LauncherFinder", "Command timed out after "
                      + std::to_string(timeout_ms) + "ms, killing");
            } else if (output_too_large) {
                LOG_W("LauncherFinder", "Command output exceeds "
                      + std::to_string(kMaxOutputSize) + " bytes, terminating");
            } else {
                LOG_W("LauncherFinder", "Command I/O failed, terminating");
            }
            terminate_child(child_pid);
            return "";
        }

        int exit_status = 0;
        while (true) {
            const pid_t wait_result = waitpid(child_pid, &exit_status, WNOHANG);
            if (wait_result == child_pid) {
                break;
            }
            if (wait_result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                LOG_E("LauncherFinder", "waitpid failed: " + std::string(strerror(errno)));
                return "";
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                LOG_W("LauncherFinder", "Command timed out after "
                      + std::to_string(timeout_ms) + "ms while waiting, killing");
                terminate_child(child_pid);
                return "";
            }
            usleep(10'000);
        }
        if (!WIFEXITED(exit_status) || WEXITSTATUS(exit_status) != 0) {
            LOG_W("LauncherFinder", "Command exited with status: " + std::to_string(exit_status));
        }

        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        return result;
    }

    static void terminate_child(pid_t child_pid) {
        if (kill(-child_pid, SIGTERM) != 0 && errno != ESRCH) {
            LOG_W("LauncherFinder", "kill SIGTERM failed: " + std::string(strerror(errno)));
        }

        for (int attempt = 0; attempt < 10; ++attempt) {
            const pid_t wait_result = waitpid(child_pid, nullptr, WNOHANG);
            if (wait_result == child_pid || (wait_result < 0 && errno == ECHILD)) {
                return;
            }
            if (wait_result < 0 && errno != EINTR) {
                LOG_W("LauncherFinder", "waitpid failed: " + std::string(strerror(errno)));
                return;
            }
            usleep(10'000);
        }

        if (kill(-child_pid, SIGKILL) != 0 && errno != ESRCH) {
            LOG_W("LauncherFinder", "kill SIGKILL failed: " + std::string(strerror(errno)));
        }
        while (waitpid(child_pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
};

#endif
