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
typedef struct qmp_sub_impl {
    struct qmp_client_impl *owner; /* NULL = inactive slot                       */
    void *buf;                     /* caller's sample buffer                     */
    size_t buf_cap;
    qmp_sub_cb_t cb;
    void *cb_ctx;
    uint64_t last_seq; /* pull cursor: newest sequence consumed      */
    uint32_t sub_id;   /* server-side id for UNSUBSCRIBE             */
    uint16_t topic_id;
    uint16_t event_len;
    uint8_t event[QMP_EVENT_BLOB_MAX]; /* registered blob, for event_unregister */
} qmp_sub_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_sub_impl_t) <= sizeof(qmp_sub_t), "qmp_sub_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_sub_impl_t) <= QMP_ALIGNOF(qmp_sub_t), "qmp_sub_t align");

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
    uint8_t have_params;         /* connect() ran once: reconnect() may reuse the identity */
    uint8_t notify_open;         /* private notification channel exists                    */
    uint8_t server_wire_version; /* from HELLO; 1 for a pre-0.3.0 server           */
    uint8_t _pad0;
    uint32_t reconnect_initial_ms;
    uint32_t reconnect_max_ms;
    uint32_t reconnect_attempts;
    uint32_t server_runtime_version; /* server's QMP_VERSION_HEX from HELLO; 0 = unknown */
    qmp_sub_impl_t *subs[QMP_CLIENT_MAX_SUBS]; /* caller-provided qmp_sub_t storage */
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
    /* The worker's own scheduling, sampled before its first receive (0 = unknown). On QNX
     * a receiving thread takes the sender's priority and KEEPS it after the reply, until
     * its next receive (V-QNX-011), so one-way dispatch restores this explicitly. */
    uint8_t base_prio;
    uint8_t base_policy;
    uint8_t _pad0;
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
    /* The handler returned QMP_DEFERRED while qmp_req_reply() on another thread held the
     * slot CLAIMED: that reply frees the slot itself. Guarded by pool_mu. */
    uint8_t handler_returned;
    uint8_t _pad0;
} qmp_req_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_req_impl_t) <= sizeof(qmp_req_t), "qmp_req_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_req_impl_t) <= QMP_ALIGNOF(qmp_req_t), "qmp_req_t align");

/* ---- server ------------------------------------------------------------- */
/* One subscription (ADR-003). Lives in the caller-provided topicbuf; guarded by
 * topic_mu. rcvid is the SUBSCRIBE message's receive id, kept after the reply for
 * MsgDeliverEvent (V-QNX-043). */
typedef struct qmp_sub_entry {
    int64_t rcvid;
    int32_t scoid;
    int32_t pid;
    uint32_t sub_id; /* 0 = free slot */
    uint16_t topic_idx;
    uint16_t event_len;
    uint8_t event[QMP_EVENT_BLOB_MAX];
} qmp_sub_entry_t;
QMP_STATIC_ASSERT(sizeof(qmp_sub_entry_t) <= QMP_SUB_ENTRY_SIZE, "sub entry size");

/* Per-topic ring header at the start of each topic block in topicbuf. Slots follow:
 * nslots x slot_stride bytes, each slot = u64 seq + padded sample. next_seq - 1 is the
 * newest published sequence; slot index for seq is (seq - 1) % nslots. */
typedef struct qmp_topic_state {
    uint64_t next_seq; /* starts at 1                               */
    uint32_t nslots;
    uint32_t slot_stride;
    uint8_t *slots;
} qmp_topic_state_t;
QMP_STATIC_ASSERT(sizeof(qmp_topic_state_t) <= QMP_TOPIC_HEADER_SIZE, "topic header size");

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
    /* Topic state (caller storage, laid out in qmp_server_register). topic_mu guards the
     * subscription table and the rings; it is never held across deliver() or a reply. */
    pthread_mutex_t topic_mu;
    qmp_sub_entry_t *subs;      /* QMP_SERVER_MAX_SUBS entries, NULL = no topics */
    qmp_topic_state_t *tstates; /* desc->ntopics ring headers (contiguous array) */
    uint8_t *topicbuf;          /* cfg storage, laid out in qmp_server_register  */
    size_t topicbuf_size;
    uint32_t sub_id_seq; /* monotonically increasing, never handed out 0  */
    uint32_t _pad1;
} qmp_server_impl_t;
QMP_STATIC_ASSERT(sizeof(qmp_server_impl_t) <= sizeof(qmp_server_t), "qmp_server_t too small");
QMP_STATIC_ASSERT(QMP_ALIGNOF(qmp_server_impl_t) <= QMP_ALIGNOF(qmp_server_t),
                  "qmp_server_t align");

/* Dispatch one received message held in rcv (len bytes). Writes the reply into rep
 * (capacity rep_cap) or sends a transport error. Never returns an error to the loop:
 * everything is reported to the client. infl is the calling worker's in-flight slot
 * (NULL = cancellation of running handlers disabled, e.g. direct test calls).
 * Exposed for unit tests. */
/* Put the calling worker back at the priority it had before its first receive. Called
 * after the receipt reply of a one-way request, so the handler does not keep running at
 * the priority of a caller that is no longer waiting (DESIGN §3.4). No-op when the
 * priority is unchanged (host transports, fixed-priority channels) or unknown. */
void qmp_worker_restore_priority(const qmp_inflight_t *infl);

void qmp_server_dispatch(qmp_server_impl_t *s, const qmp_rcv_info_t *info, uint8_t *rcv, size_t len,
                         uint8_t *rep, size_t rep_cap, qmp_inflight_t *infl);

/* Pure dispatch decision (testable without a transport). Returns EOK when the request may
 * run and sets *out_m; else the errno the client must receive. */
int qmp_dispatch_lookup(const qmp_service_desc_t *desc, uint16_t instance_id, const qmp_hdr_t *h,
                        const qmp_method_desc_t **out_m);

/* Lifecycle events from the transport (worker thread). */
void qmp_server_on_unblock(qmp_server_impl_t *s, int64_t rcvid);
void qmp_server_on_disconnect(qmp_server_impl_t *s, int32_t scoid, int32_t pid);

/* ---- topics (runtime/core/topics.c) -------------------------------------- */
/* Lay out subs table + ring headers + slots inside s->topicbuf. EMSGSIZE when the buffer
 * is too small, EINVAL on a malformed topic table. Called from qmp_server_register. */
int qmp_topics_init(qmp_server_impl_t *s);
/* Returns EOK and *idx, or ENOENT. */
int qmp_topic_find(const qmp_server_impl_t *s, uint16_t topic_id, uint32_t *idx);
/* Add a subscription; fills *rep. EAGAIN when the table is full. */
int qmp_topic_sub_add(qmp_server_impl_t *s, uint32_t idx, const qmp_rcv_info_t *info,
                      const qmp_subscribe_t *req, qmp_subscribe_reply_t *rep);
/* Remove by sub_id; the scoid must match the subscriber's (ownership). ENOENT if gone. */
int qmp_topic_sub_remove(qmp_server_impl_t *s, uint32_t sub_id, int32_t scoid);
/* Copy the next sample after after_seq into sample_out (cap >= sample_size) and fill
 * *rep (count == 0 when there is nothing new). */
int qmp_topic_pull(qmp_server_impl_t *s, uint32_t idx, uint64_t after_seq, qmp_pull_reply_t *rep,
                   void *sample_out);
/* Drop every subscription owned by a disconnected client process. */
void qmp_topic_drop_scoid(qmp_server_impl_t *s, int32_t scoid);

#ifdef __cplusplus
}
#endif
#endif /* QMP_CORE_INTERNAL_H */
