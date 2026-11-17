/* SPDX-License-Identifier: Apache-2.0 */
/* Logging shim: slog2 on target (W2), stderr on the host. Never used on the call path.
 * Each line is formatted into one stack buffer and written with a single write() so lines
 * from different threads cannot interleave (stderr FILE buffering does not guarantee
 * atomicity across the trailing newline). */
#ifndef QMP_CORE_LOG_H
#define QMP_CORE_LOG_H

#include <stdio.h>
#include <unistd.h>

#if defined(QMP_NO_LOG)
#define qmp_log(...) ((void)0)
#else
/* VERIFY(qnx8): V-QNX-020 — switch to slog2f() once verified on target. */
#define qmp_log(...)                                                                               \
    do {                                                                                           \
        char qmp_log_buf_[256];                                                                    \
        int qmp_log_n_ = snprintf(qmp_log_buf_, sizeof(qmp_log_buf_) - 1U, "qmp: " __VA_ARGS__);   \
        if (qmp_log_n_ > 0) {                                                                      \
            if ((size_t)qmp_log_n_ > sizeof(qmp_log_buf_) - 1U) {                                  \
                qmp_log_n_ = (int)sizeof(qmp_log_buf_) - 1;                                        \
            }                                                                                      \
            qmp_log_buf_[qmp_log_n_] = '\n';                                                       \
            (void)write(STDERR_FILENO, qmp_log_buf_, (size_t)qmp_log_n_ + 1U);                     \
        }                                                                                          \
    } while (0)
#endif

#endif /* QMP_CORE_LOG_H */
