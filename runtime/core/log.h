/* SPDX-License-Identifier: Apache-2.0 */
/* Logging shim: slog2 on target (W2), stderr on the host. Never used on the call path. */
#ifndef QMP_CORE_LOG_H
#define QMP_CORE_LOG_H

#include <stdio.h>

#if defined(QMP_NO_LOG)
#define qmp_log(...) ((void)0)
#else
/* VERIFY(qnx8): V-QNX-020 — switch to slog2f() once the qnx transport lands (W2). */
#define qmp_log(...)                                                                               \
    do {                                                                                           \
        (void)fprintf(stderr, "qmp: " __VA_ARGS__);                                                \
        (void)fputc('\n', stderr);                                                                 \
    } while (0)
#endif

#endif /* QMP_CORE_LOG_H */
