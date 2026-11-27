/* SPDX-License-Identifier: Apache-2.0 */
/* Transport selection: the platform default and lookup by name. */
#include <string.h>

#include <qmp/qmp_transport.h>

const qmp_transport_t *qmp_transport_default(void) {
#if defined(__QNX__)
    return qmp_transport_qnx(); /* native message passing is the default on QNX */
#elif defined(QMP_TRANSPORT_UDS_BENCH_ONLY)
    return qmp_transport_uds(); /* host: real process boundaries for integration tests */
#else
    return qmp_transport_fake();
#endif
}

const qmp_transport_t *qmp_transport_by_name(const char *name) {
    if (name == NULL) {
        return NULL;
    }
    if (strcmp(name, "fake") == 0) {
        return qmp_transport_fake();
    }
#if defined(__QNX__)
    if (strcmp(name, "qnx") == 0) {
        return qmp_transport_qnx();
    }
#endif
#if defined(QMP_TRANSPORT_UDS_BENCH_ONLY)
    if (strcmp(name, "uds") == 0) {
        return qmp_transport_uds();
    }
#endif
    return NULL;
}
