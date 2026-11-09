/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Client API. All functions return EOK or an errno value; the application status (UCode)
 * comes back through the int32_t *status out-parameter. No hidden threads (Q3), no heap
 * allocation after init. Storage is caller-provided.
 */
#ifndef QMP_QMP_CLIENT_H
#define QMP_QMP_CLIENT_H

#include <qmp/qmp.h>
#include <qmp/qmp_transport.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QMP_CLIENT_STORAGE_U64 24

typedef struct qmp_client {
    uint64_t _storage[QMP_CLIENT_STORAGE_U64];
} qmp_client_t;

typedef struct qmp_client_cfg {
    const qmp_transport_t *transport; /* NULL = qmp_transport_default()          */
    uint64_t default_timeout_ns;      /* 0 = block forever                       */
} qmp_client_cfg_t;

/* Thread: caller's. Never blocks. */
int qmp_client_init(qmp_client_t *c, const qmp_client_cfg_t *cfg);

/* Resolve "qmp/<svc>-<inst>/v<major>", connect and perform the HELLO handshake (Q4).
 * Blocking: yes. Returns ENOENT if the service is not registered, ENOTSUP on a major
 * version mismatch. Thread: caller's; the server inherits the caller's priority. */
int qmp_client_connect(qmp_client_t *c, uint16_t service_id, uint16_t instance_id,
                       uint8_t ver_major, uint8_t ver_minor);

/* Synchronous method call. req may be NULL when req_len == 0 (zero-field message);
 * rep may be NULL when rep_cap == 0. *status receives the server's UCode (0 = OK); when it
 * is non-zero the reply payload is left untouched. opts may be NULL.
 * Blocking: yes (MsgSendv). Thread: caller's. Priority: inherited by the server. */
int qmp_client_call(qmp_client_t *c, uint16_t resource_id, const void *req, size_t req_len,
                    void *rep, size_t rep_cap, int32_t *status, const qmp_call_opts_t *opts);

/* Disconnect. Safe to call on a never-connected client. */
int qmp_client_close(qmp_client_t *c);

int qmp_client_is_connected(const qmp_client_t *c);
uint32_t qmp_client_server_generation(const qmp_client_t *c);
uint8_t qmp_client_server_minor(const qmp_client_t *c);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_CLIENT_H */
