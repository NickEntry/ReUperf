#ifndef RESTORATION_RETRY_HPP
#define RESTORATION_RETRY_HPP

#include <chrono>
#include <thread>
#include <utility>

template <typename RestoreOperation, typename DelayOperation>
bool run_restoration_with_retry(int max_attempts, RestoreOperation&& restore_operation,
                                DelayOperation&& delay_operation) {
    if (max_attempts <= 0) {
        return false;
    }
    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        if (restore_operation()) {
            return true;
        }
        if (attempt < max_attempts) {
            delay_operation(attempt);
        }
    }
    return false;
}

template <typename RestoreOperation>
bool restore_with_bounded_retry(int max_attempts, int retry_delay_ms,
                                RestoreOperation&& restore_operation) {
    return run_restoration_with_retry(
        max_attempts, std::forward<RestoreOperation>(restore_operation),
        [retry_delay_ms](int) {
            if (retry_delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(retry_delay_ms));
            }
        });
}

#endif
