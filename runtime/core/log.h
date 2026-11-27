/* SPDX-License-Identifier: Apache-2.0 */
/* Logging shim: slog2 on target (W2), stderr on the host. Never used on the call path.
 * Each line is formatted into one stack buffer and written with a single write() so lines
 * from different threads cannot interleave (stderr FILE buffering does not guarantee
 * atomicity across the trailing newline).
 *
 * Every line starts with the CLOCK_MONOTONIC time in seconds, in the same
 * "[<s>.<us>] <who>: " shape as the example programs, so runtime and application lines
 * from one run sort and correlate on a single clock. */
#ifndef QMP_CORE_LOG_H
#define QMP_CORE_LOG_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#if defined(QMP_NO_LOG)
#define qmp_log(...) ((void)0)
#else
/* Writes "[<s>.<us>] qmp: " into buf; returns its length (0 on error). */
static inline size_t qmp_log_prefix_(char *buf, size_t cap) {
    struct timespec ts = {0, 0};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    int n = snprintf(buf, cap, "[%" PRIu64 ".%06" PRIu64 "] qmp: ", (uint64_t)ts.tv_sec,
                     (uint64_t)ts.tv_nsec / 1000U);
    if (n < 0) {
        return 0;
    }
    return (size_t)n < cap ? (size_t)n : cap - 1U;
}

/* VERIFY(qnx8): V-QNX-020 — switch to slog2f() once verified on target. */
#define qmp_log(...)                                                                               \
    do {                                                                                           \
        char qmp_log_buf_[256];                                                                    \
        size_t qmp_log_off_ = qmp_log_prefix_(qmp_log_buf_, sizeof(qmp_log_buf_) - 1U);            \
        int qmp_log_n_ = snprintf(qmp_log_buf_ + qmp_log_off_,                                     \
                                  sizeof(qmp_log_buf_) - 1U - qmp_log_off_, __VA_ARGS__);          \
        if (qmp_log_n_ >= 0) {                                                                     \
            qmp_log_off_ += (size_t)qmp_log_n_;                                                    \
            if (qmp_log_off_ > sizeof(qmp_log_buf_) - 2U) {                                        \
                qmp_log_off_ = sizeof(qmp_log_buf_) - 2U; /* truncated */                          \
            }                                                                                      \
            qmp_log_buf_[qmp_log_off_] = '\n';                                                     \
            /* Best effort; a (void) cast does not silence glibc's warn_unused_result. */          \
            ssize_t qmp_log_w_ = write(STDERR_FILENO, qmp_log_buf_, qmp_log_off_ + 1U);            \
            (void)qmp_log_w_;                                                                      \
        }                                                                                          \
    } while (0)
#endif

#endif /* QMP_CORE_LOG_H */
