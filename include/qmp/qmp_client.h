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

#define QMP_CLIENT_STORAGE_U64 40
#define QMP_CLIENT_MAX_SUBS    8
#define QMP_SUB_STORAGE_U64    24

typedef struct qmp_client {
    uint64_t _storage[QMP_CLIENT_STORAGE_U64];
} qmp_client_t;

/* Caller-provided storage for one subscription (ADR-003). */
typedef struct qmp_sub {
    uint64_t _storage[QMP_SUB_STORAGE_U64];
} qmp_sub_t;

/* Called from qmp_client_poll() on the polling thread, once per pulled sample, in
 * sequence order. sample points at the subscription's sample buffer and is valid for the
 * duration of the call. dropped counts updates lost (overwritten in LATEST mode or
 * pushed out of a full QUEUE ring) between the previous delivery and this one. */
typedef void (*qmp_sub_cb_t)(void *ctx, const void *sample, uint32_t len, uint64_t seq,
                             uint64_t dropped);

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
 * last connect error (ENOENT, ETIMEDOUT, ...).
 * Active subscriptions are re-registered and re-subscribed on the new session: old
 * subscription ids and event registrations die with the old connection, sequence numbers
 * restart, and each subscription's cursor is reset to the restarted server's current
 * sequence. When resubscription fails its errno is returned but the connection stays up;
 * the failed subscription is dropped (its storage may be reused). */
int qmp_client_reconnect(qmp_client_t *c);

/* ---- topics (ADR-003 notify-then-pull) ----------------------------------- */
/* Subscribe to a topic. sample_buf (8-byte aligned, sample_cap >= the topic's sample
 * size) holds the pulled sample handed to cb. sub must stay alive until unsubscribe or
 * close. Opens the private notification channel on first use. Only new updates are
 * delivered: the cursor starts at the server's current sequence number.
 * Returns EOK, ENOTCONN, ENOSYS (transport has no notification support), EAGAIN (client
 * or server subscription table full), ENOENT (unknown topic). Blocking: yes (one call). */
int qmp_client_subscribe(qmp_client_t *c, uint16_t topic_id, void *sample_buf, size_t sample_cap,
                         qmp_sub_cb_t cb, void *ctx, qmp_sub_t *sub);

/* Unsubscribe and release the slot. When the server is unreachable the local state is
 * still released (the server cleans up on disconnect). Safe on an inactive sub. */
int qmp_client_unsubscribe(qmp_client_t *c, qmp_sub_t *sub);

/* Wait for a topic notification, then pull every pending update on every active
 * subscription and run the callbacks (caller's thread). The pulse is only a hint:
 * correctness depends on the pull cursor, not on every pulse arriving. Returns EOK (>= 0
 * callbacks ran), ETIMEDOUT, ESRCH (server died; client marked disconnected — use
 * qmp_client_reconnect()), EINVAL (no active subscription). timeout_ns 0 blocks forever. */
int qmp_client_poll(qmp_client_t *c, uint64_t timeout_ns);

/* ---- reflection (tools) --------------------------------------------------- */
/* Read up to cap bytes of the service's reflection JSON starting at offset. *len receives
 * the bytes written to buf, *total_len the size of the whole document; loop with
 * offset += *len until offset == *total_len. The bytes are not NUL-terminated. Meant for
 * tools (qmpctl), never for a control loop. Returns EOK, ENOTCONN, ENOSYS (the service
 * carries no reflection data, or its runtime predates the opcode), EPROTO.
 * Blocking: yes (one call per chunk, at most QMP_REFLECT_CHUNK_MAX bytes each). */
int qmp_client_reflect(qmp_client_t *c, uint32_t offset, void *buf, size_t cap, uint32_t *len,
                       uint32_t *total_len);

int qmp_client_is_connected(const qmp_client_t *c);
uint32_t qmp_client_server_generation(const qmp_client_t *c);
uint8_t qmp_client_server_minor(const qmp_client_t *c);
/* Wire version the server advertised in HELLO (1 for a pre-0.3.0 server); 0 if never
 * connected. */
uint8_t qmp_client_server_wire_version(const qmp_client_t *c);
/* QMP_VERSION_HEX of the server's libqmp from HELLO; 0 if unknown (pre-0.3.0 server or
 * never connected). */
uint32_t qmp_client_server_runtime_version(const qmp_client_t *c);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_CLIENT_H */
