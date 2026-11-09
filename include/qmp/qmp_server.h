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

#define QMP_SERVER_STORAGE_U64 64
#define QMP_REQ_STORAGE_U64    8
#define QMP_SERVER_MAX_THREADS 8

typedef struct qmp_server {
    uint64_t _storage[QMP_SERVER_STORAGE_U64];
} qmp_server_t;

/* One in-flight request. Valid inside the handler; after QMP_DEFERRED (W2) until replied,
 * cancelled or the client disconnects. */
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

typedef struct qmp_service_desc {
    uint16_t service_id;
    uint8_t ver_major;
    uint8_t ver_minor;
    uint32_t max_msg_size;            /* header + largest payload of this interface */
    const char *name;                 /* uprotocol.service_name                     */
    const qmp_method_desc_t *methods; /* sorted by id                    */
    size_t nmethods;
} qmp_service_desc_t;

typedef struct qmp_server_cfg {
    const qmp_transport_t *transport; /* NULL = qmp_transport_default()                    */
    int nthreads;                     /* 1..QMP_SERVER_MAX_THREADS; 0 = default 4          */
    /* Caller-provided work buffer: nthreads x (receive + reply) x max_msg_size bytes.
     * Use QMP_SERVER_WORKBUF_SIZE(). Must be 8-byte aligned. */
    void *workbuf;
    size_t workbuf_size;
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

/* Inside a handler: information about the caller. */
int qmp_req_get_info(const qmp_req_t *req, qmp_req_info_t *info);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_SERVER_H */
