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
    /* qmp_client_reconnect() backoff. 0 picks the default in parentheses. */
    uint32_t reconnect_initial_ms; /* first delay (10)                           */
    uint32_t reconnect_max_ms;     /* delay ceiling, doubling (1000)             */
    uint32_t reconnect_attempts;   /* connection attempts (10)                   */
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

/* One-way method call (DESIGN §3.4). The server replies (header only) on receipt, before
 * running the handler, so priority inheritance covers only the delivery. No application
 * status comes back; delivery failures return an errno (ETIMEDOUT, ESRCH, EACCES, ...).
 * Blocking: yes, until the receipt reply. Thread: caller's. */
int qmp_client_oneway(qmp_client_t *c, uint16_t resource_id, const void *req, size_t req_len,
                      const qmp_call_opts_t *opts);

/* Disconnect. Safe to call on a never-connected client. */
int qmp_client_close(qmp_client_t *c);

/* Reconnect after the server died (a call returned ESRCH or EBADF and the client marked
 * itself disconnected). Retries connect + HELLO with exponential backoff per the cfg.
 * Never replays a failed call: the application decides what is safe to repeat (DESIGN
 * §3.7). Compare qmp_client_server_generation() before and after to learn whether the
 * server restarted (it did when the value changed). Blocking: yes, up to the full backoff
 * schedule. Returns EOK, EISCONN if still connected, ENOTCONN if never connected, or the
 * last connect error (ENOENT, ETIMEDOUT, ...). */
int qmp_client_reconnect(qmp_client_t *c);

int qmp_client_is_connected(const qmp_client_t *c);
uint32_t qmp_client_server_generation(const qmp_client_t *c);
uint8_t qmp_client_server_minor(const qmp_client_t *c);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_CLIENT_H */
