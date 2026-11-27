/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Server API. Generated code builds a qmp_service_desc_t (method table + trampolines) and
 * registers it; the runtime owns the receive loop, header/payload validation and replies.
 *
 * Threading (DESIGN §5.1): qmp_server_run() spawns nthreads-1 workers and runs the last
 * one on the calling thread; all are RECEIVE-blocked on one channel. Handlers run on a
 * worker at the CALLER's priority (inherited). Never hold a mutex across a reply.
 */
#ifndef QMP_QMP_SERVER_H
#define QMP_QMP_SERVER_H

#include <qmp/qmp.h>
#include <qmp/qmp_transport.h>

#ifdef __cplusplus
extern "C" {
#endif

#define QMP_SERVER_STORAGE_U64 88
#define QMP_REQ_STORAGE_U64    10
#define QMP_SERVER_MAX_THREADS 8

/* Topic storage bounds (ADR-003). Subscriptions across all topics share one fixed table;
 * a full table fails SUBSCRIBE with EAGAIN (bounded by design, never grown). */
#define QMP_SERVER_MAX_SUBS 16
#define QMP_SUB_ENTRY_SIZE  96U
/* Per-topic block: a fixed header + nslots ring slots of (u64 seq + padded sample).
 * LATEST topics use nslots == 1, QUEUE topics nslots == depth. Generated headers compose
 * these into a per-service <MACRO>_TOPIC_STORAGE_SIZE. */
#define QMP_TOPIC_ALIGN8(n)         (((size_t)(n) + 7U) & ~(size_t)7U)
#define QMP_TOPIC_SLOT_SIZE(sample) (8U + QMP_TOPIC_ALIGN8(sample))
#define QMP_TOPIC_HEADER_SIZE       64U
#define QMP_TOPIC_STATE_SIZE(sample, nslots)                                                       \
    (QMP_TOPIC_HEADER_SIZE + (size_t)(nslots) * QMP_TOPIC_SLOT_SIZE(sample))
#define QMP_SERVER_SUBTABLE_SIZE ((size_t)QMP_SERVER_MAX_SUBS * QMP_SUB_ENTRY_SIZE)

typedef struct qmp_server {
    uint64_t _storage[QMP_SERVER_STORAGE_U64];
} qmp_server_t;

/* One in-flight request. Valid inside the handler; after QMP_DEFERRED until it is
 * replied with qmp_req_reply(), cancelled (on_cancel) or the client disconnects. */
typedef struct qmp_req {
    uint64_t _storage[QMP_REQ_STORAGE_U64];
} qmp_req_t;

typedef struct qmp_req_info {
    int32_t pid; /* 0 when the transport does not know            */
    int32_t scoid;
    int32_t priority;    /* caller's priority, 0 when unknown              */
    uint8_t up_priority; /* hdr.up_priority (0 = native call)              */
    uint8_t ver_minor;   /* client's minor version                         */
    uint8_t _pad0[2];
} qmp_req_info_t;

/* Kernel-supplied caller identity for the ACL hook. On QNX this comes from
 * ConnectClientInfo, cached per scoid; the fake transport reports the test process. */
typedef struct qmp_cred {
    int32_t pid;
    int32_t uid;      /* effective uid, -1 when the transport has no credentials */
    int32_t gid;      /* effective gid, -1 when the transport has no credentials */
    uint32_t type_id; /* secpol type id, 0 = none/unknown (V-QNX-014)            */
} qmp_cred_t;

/* Generated trampoline: casts `ops` to the typed ops table and `in`/`out` to the payload
 * structs. Returns QMP_HANDLED, QMP_DEFERRED or a positive errno. */
typedef int (*qmp_handler_fn)(const void *ops, void *ctx, qmp_req_t *req, const void *in, void *out,
                              int32_t *status);
/* Generated bounds check for an incoming payload. Returns EOK or ERANGE. */
typedef int (*qmp_validate_fn)(const void *payload);

enum qmp_method_flags {
    QMP_MF_ONEWAY = 1U << 0,
};

typedef struct qmp_method_desc {
    uint16_t id;      /* resource id 0x0000..0x7FFF                                  */
    uint16_t flags;   /* enum qmp_method_flags                                       */
    uint32_t in_size; /* sizeof(request struct), 0 for a zero-field message          */
    uint32_t out_size;
    uint32_t _pad0;
    qmp_validate_fn validate_in; /* NULL when the request has no bounded fields     */
    qmp_handler_fn handler;      /* NULL = not generated yet -> client gets ENOSYS  */
    const char *name;
} qmp_method_desc_t;

typedef struct qmp_topic_desc {
    uint16_t id;    /* resource id 0x8000..0xFFFF             */
    uint16_t mode;  /* enum qmp_topic_mode                    */
    uint16_t depth; /* ring depth for QUEUE, ignored for LATEST */
    uint16_t _pad0;
    uint32_t sample_size;
    uint32_t _pad1;
    const char *name;
} qmp_topic_desc_t;

typedef struct qmp_service_desc {
    uint16_t service_id;
    uint8_t ver_major;
    uint8_t ver_minor;
    uint32_t max_msg_size;            /* header + largest payload of this interface */
    const char *name;                 /* uprotocol.service_name                     */
    const qmp_method_desc_t *methods; /* sorted by id                    */
    size_t nmethods;
    const qmp_topic_desc_t *topics; /* sorted by id; NULL when ntopics == 0 */
    size_t ntopics;
    /* Reflection JSON served to tools through QMP_OP_REFLECT (generated code points this
     * at the embedded <svc>.reflect.json). NULL/0 = none: REFLECT answers ENOSYS. */
    const unsigned char *reflect_json;
    size_t reflect_json_len;
} qmp_service_desc_t;

typedef struct qmp_server_cfg {
    const qmp_transport_t *transport; /* NULL = qmp_transport_default()                    */
    int nthreads;                     /* 1..QMP_SERVER_MAX_THREADS; 0 = default 4          */
    /* enum qmp_listen_flags (qmp_transport.h). QMP_LISTEN_FIXED_PRIORITY is a negative
     * control for demos only; the default (0) keeps priority inheritance. */
    uint32_t listen_flags;
    /* Log pulses, cancellations and dropped (stale/no-match) unblock pulses. Never on the
     * fast path: only lifecycle events are logged. */
    int verbose;
    /* Caller-provided work buffer: nthreads x (receive + reply) x max_msg_size bytes.
     * Use QMP_SERVER_WORKBUF_SIZE(). Must be 8-byte aligned. */
    void *workbuf;
    size_t workbuf_size;

    /* Deferred replies (DESIGN §3.3). Handlers may return QMP_DEFERRED only when a pool
     * is configured: caller-provided qmp_req_t storage, 8-byte aligned, bounded. When the
     * pool is full the call fails with EAGAIN at the client. NULL = deferral disabled. */
    qmp_req_t *deferred_pool;
    size_t deferred_count;

    /* Per-method ACL hook. Called on the worker thread (caller's priority) after header
     * validation and before the handler, with kernel-supplied credentials cached per
     * scoid. Return EOK to allow; anything else sends EACCES. NULL = allow everything.
     * Policy (qmp.allow_types, permission levels) is layered on top of this hook. */
    int (*acl_check)(void *ctx, const qmp_cred_t *cred, uint16_t resource_id);

    /* A deferred request was cancelled before the app replied: the client timed out or
     * was signalled (kernel unblock pulse) or its process disconnected. Fires for PARKED
     * requests and when a handler returns QMP_DEFERRED for a request that was already
     * cancelled while it ran. The req pointer is invalid once this returns; do not call
     * qmp_req_reply() on it. Worker or pulse thread. NULL = no notification. */
    void (*on_cancel)(void *ctx, qmp_req_t *req);

    /* A client process closed its last connection (QNX disconnect pulse). The runtime
     * has already dropped its parked requests (each with on_cancel). Worker thread. */
    void (*on_client_gone)(void *ctx, int32_t pid);

    /* Topic storage: caller-provided, 8-byte aligned, QMP_SERVER_SUBTABLE_SIZE plus the
     * sum of QMP_TOPIC_STATE_SIZE() over the topics — generated code emits the exact
     * <MACRO>_TOPIC_STORAGE_SIZE. Owned by the server until qmp_server_close(). NULL
     * disables topics: SUBSCRIBE and qmp_server_publish() return ENOTSUP. */
    void *topicbuf;
    size_t topicbuf_size;
} qmp_server_cfg_t;

#define QMP_SERVER_WORKBUF_SIZE(max_msg_size, nthreads)                                            \
    ((size_t)(max_msg_size) * 2U * (size_t)(nthreads))

/* Thread: caller's. Never blocks. */
int qmp_server_init(qmp_server_t *s, const qmp_server_cfg_t *cfg);

/* Register exactly one service (v1) and attach the channel under
 * "qmp/<service_id>-<instance_id>/v<major>". Returns EEXIST if already registered,
 * EADDRINUSE if the name is taken, EMSGSIZE if the work buffer is too small. */
int qmp_server_register(qmp_server_t *s, const qmp_service_desc_t *desc, uint16_t instance_id,
                        const void *ops, void *ctx);

/* Run the receive loop on the calling thread (plus nthreads-1 workers) until
 * qmp_server_stop() is called from another thread. Returns EOK after all workers joined. */
int qmp_server_run(qmp_server_t *s);
/* Any thread. Wakes all receivers; qmp_server_run() returns. */
int qmp_server_stop(qmp_server_t *s);
/* Detach the name and release the channel. Call after qmp_server_run() returned. */
int qmp_server_close(qmp_server_t *s);

uint32_t qmp_server_generation(const qmp_server_t *s);

/* Publish one sample on a declared topic (ADR-003). Records the sample under the next
 * sequence number, then notifies every subscriber with a non-blocking event delivery; a
 * dead or unreachable subscriber is dropped from the table, the others are unaffected.
 * No heap, no logging, no lock held across the kernel delivery call. len must equal the
 * topic's sample size. Any thread (also from inside handlers). Returns EOK, ENOENT
 * (unknown topic id), EINVAL (bad len), ENOTSUP (no topic storage configured). */
int qmp_server_publish(qmp_server_t *s, uint16_t topic_id, const void *sample, size_t len);

/* Inside a handler: information about the caller. */
int qmp_req_get_info(const qmp_req_t *req, qmp_req_info_t *info);

/* Inside a RUNNING handler: 1 when the request was cancelled (client timeout/signal
 * unblock, or its process disconnected). The runtime has already released the client (or
 * the client is gone); the handler should stop early and return — its result is discarded
 * and no second reply is sent. Takes a short internal lock: poll at a modest rate, not in
 * the innermost loop. For PARKED (deferred) requests use on_cancel instead; after the
 * handler returned this function must not be called. */
int qmp_req_canceled(const qmp_req_t *req);

/* Complete a request whose handler returned QMP_DEFERRED. Any thread. status == 0
 * requires payload_len == the method's reply size; status != 0 requires payload_len == 0.
 * Returns EOK (a client that died meanwhile counts as delivered), EINVAL if the request
 * was already completed or cancelled. After EOK or on_cancel the pointer is dead: using
 * it again is undefined, like a double free. Never blocks on the client. */
int qmp_req_reply(qmp_req_t *req, const void *payload, size_t payload_len, int32_t status);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_SERVER_H */
