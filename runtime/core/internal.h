/* SPDX-License-Identifier: Apache-2.0 */
/* Private layouts behind the caller-provided opaque storage types. Not installed. */
#ifndef QMP_CORE_INTERNAL_H
#define QMP_CORE_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>

#include <qmp/qmp.h>
#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#include <qmp/qmp_transport.h>

#include "log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- client ------------------------------------------------------------- */
typedef struct qmp_client_impl {
    const qmp_transport_t *tr;
    uint64_t default_timeout_ns;
    qmp_conn_t conn;
    uint16_t service_id;
    uint16_t instance_id;
    uint8_t ver_major;
    uint8_t ver_minor;
    uint8_t server_minor;
    uint8_t connected;
    uint32_t server_generation;
    uint32_t server_max_msg_size;
    uint8_t have_params; /* connect() ran once: reconnect() may reuse the identity */
    uint8_t _pad0[3];
    uint32_t reconnect_initial_ms;
    uint32_t reconnect_max_ms;
    uint32_t reconnect_attempts;
} qmp_client_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_client_impl_t) <= sizeof(qmp_client_t), "qmp_client_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_client_impl_t) <= QMP_ALIGNOF(qmp_client_t),
                  "qmp_client_t align");

/* ---- request ------------------------------------------------------------ */
enum qmp_req_state {
    QMP_REQ_FREE = 0,
    QMP_REQ_ACTIVE,  /* handler running on a worker                   */
    QMP_REQ_PARKED,  /* QMP_DEFERRED returned; waiting for the app    */
    QMP_REQ_CLAIMED, /* reply/cancel in progress, slot pinned         */
    QMP_REQ_DONE     /* replied while ACTIVE; dispatch frees the slot */
};

/* Per-worker in-flight slot: the synchronous request this worker is currently running a
 * handler for. Preallocated in the server (one per worker, no heap); guarded by pool_mu.
 * An UNBLOCK pulse claims the slot (RUNNING -> CANCELED) and sends the error reply; the
 * owning worker is the only other writer and clears the slot when the handler returns. */
enum qmp_infl_state {
    QMP_INFL_IDLE = 0,
    QMP_INFL_RUNNING,  /* handler running; rcvid/scoid valid              */
    QMP_INFL_CANCELED, /* unblock/disconnect cancelled it; do not reply   */
};

typedef struct qmp_inflight {
    int64_t rcvid;
    int32_t scoid;
    uint8_t state; /* enum qmp_infl_state */
    uint8_t _pad0[3];
} qmp_inflight_t;

typedef struct qmp_req_impl {
    struct qmp_server_impl *server;
    qmp_inflight_t *infl; /* worker's in-flight slot while the handler runs; else NULL */
    int64_t rcvid;
    qmp_req_info_t info;
    qmp_hdr_t h;       /* request header, needed to build the deferred reply */
    uint32_t out_size; /* method's reply payload size                        */
    uint8_t state;     /* enum qmp_req_state; pool slots guarded by pool_mu  */
    uint8_t pooled;    /* 0: stack storage inside dispatch (sync only)       */
    uint8_t _pad0[2];
} qmp_req_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_req_impl_t) <= sizeof(qmp_req_t), "qmp_req_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_req_impl_t) <= QMP_ALIGNOF(qmp_req_t), "qmp_req_t align");

/* ---- server ------------------------------------------------------------- */
typedef struct qmp_server_impl {
    const qmp_transport_t *tr;
    qmp_endpoint_t ep;
    const qmp_service_desc_t *desc;
    const void *ops;
    void *ctx;
    uint8_t *workbuf;
    size_t workbuf_size;
    uint32_t max_msg_size;
    uint32_t generation;
    uint16_t instance_id;
    uint8_t nthreads;
    uint8_t registered;
    uint8_t listening;
    uint8_t running;
    uint8_t verbose;       /* log pulses/cancellations (off the fast path)      */
    uint8_t has_pulse_thr; /* transport provides receive_pulse; thread running  */
    uint32_t listen_flags; /* enum qmp_listen_flags                             */
    uint8_t _pad0[4];
    pthread_t workers[QMP_SERVER_MAX_THREADS];
    pthread_t pulse_thread;
    /* One in-flight slot per worker (index = worker index); guarded by pool_mu. */
    qmp_inflight_t inflight[QMP_SERVER_MAX_THREADS];
    char path[QMP_NAME_MAX];
    /* Deferred-reply pool (caller storage) and lifecycle callbacks (cfg). pool_mu guards
     * slot states only; it is never held across a reply or a callback. */
    pthread_mutex_t pool_mu;
    qmp_req_impl_t *pool;
    size_t pool_count;
    int (*acl_check)(void *ctx, const qmp_cred_t *cred, uint16_t resource_id);
    void (*on_cancel)(void *ctx, qmp_req_t *req);
    void (*on_client_gone)(void *ctx, int32_t pid);
} qmp_server_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_server_impl_t) <= sizeof(qmp_server_t), "qmp_server_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_server_impl_t) <= QMP_ALIGNOF(qmp_server_t),
                  "qmp_server_t align");

/* Dispatch one received message held in rcv (len bytes). Writes the reply into rep
 * (capacity rep_cap) or sends a transport error. Never returns an error to the loop:
 * everything is reported to the client. infl is the calling worker's in-flight slot
 * (NULL = cancellation of running handlers disabled, e.g. direct test calls).
 * Exposed for unit tests. */
void qmp_server_dispatch(qmp_server_impl_t *s, const qmp_rcv_info_t *info, uint8_t *rcv, size_t len,
                         uint8_t *rep, size_t rep_cap, qmp_inflight_t *infl);

/* Pure dispatch decision (testable without a transport). Returns EOK when the request may
 * run and sets *out_m; else the errno the client must receive. */
int qmp_dispatch_lookup(const qmp_service_desc_t *desc, uint16_t instance_id, const qmp_hdr_t *h,
                        const qmp_method_desc_t **out_m);

/* Lifecycle events from the transport (worker thread). */
void qmp_server_on_unblock(qmp_server_impl_t *s, int64_t rcvid);
void qmp_server_on_disconnect(qmp_server_impl_t *s, int32_t scoid, int32_t pid);

#ifdef __cplusplus
}
#endif
#endif /* QMP_CORE_INTERNAL_H */
