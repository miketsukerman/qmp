/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Internal transport interface. Public so that out-of-tree transports can be written and so
 * tests can pick the in-process fake. Application code uses qmp_client.h / qmp_server.h.
 *
 * A transport provides: client connect/call/disconnect and server listen/receive/reply/
 * error/unblock/close. It never interprets qmp headers.
 */
#ifndef QMP_QMP_TRANSPORT_H
#define QMP_QMP_TRANSPORT_H

#include <qmp/qmp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Same layout as struct iovec (void *iov_base; size_t iov_len) on QNX and Linux. */
typedef struct qmp_iov {
    void *base;
    size_t len;
} qmp_iov_t;

#define QMP_CONN_STORAGE_U64     8
#define QMP_ENDPOINT_STORAGE_U64 16

/* Caller-provided opaque storage for one client connection. */
typedef struct qmp_conn {
    uint64_t _storage[QMP_CONN_STORAGE_U64];
} qmp_conn_t;

/* Caller-provided opaque storage for one server endpoint (channel). */
typedef struct qmp_endpoint {
    uint64_t _storage[QMP_ENDPOINT_STORAGE_U64];
} qmp_endpoint_t;

enum qmp_rcv_kind {
    QMP_RCV_MESSAGE = 0,    /* a message was copied into the buffer                */
    QMP_RCV_DISCONNECT = 1, /* client process gone (scoid valid)        — W2 qnx  */
    QMP_RCV_UNBLOCK = 2,    /* client wants to unblock (rcvid valid)    — W2 qnx  */
    QMP_RCV_PULSE = 3,      /* other pulse (code/value valid)           — W2 qnx  */
};

typedef struct qmp_rcv_info {
    int64_t rcvid;       /* reply token; transport-specific                       */
    int64_t pulse_value; /* 64 bits: QNX 8.0 pulse values are 64-bit              */
    int32_t scoid;       /* server-side connection id (QNX) or client slot (fake) */
    int32_t pid;         /* sender pid when known, 0 otherwise                    */
    int32_t priority;    /* sender priority when known, 0 otherwise               */
    uint32_t msglen;     /* bytes copied into the buffer                          */
    uint32_t srcmsglen;  /* bytes the sender sent (> msglen means truncated)      */
    uint32_t dstmsglen;  /* sender's reply-buffer capacity, 0 when unknown        */
    uint16_t kind;       /* enum qmp_rcv_kind                                     */
    int8_t pulse_code;
    uint8_t cred_valid; /* uid/gid/type_id below are kernel-supplied             */
    int32_t uid;        /* effective uid, -1 when unknown                        */
    int32_t gid;        /* effective gid, -1 when unknown                        */
    uint32_t type_id;   /* secpol type id, 0 when none/unknown                   */
    uint32_t _pad0;
} qmp_rcv_info_t;

/* listen() flags. */
enum qmp_listen_flags {
    /* Negative control for priority-inheritance demos: create the channel with a fixed
     * priority (QNX: _NTO_CHF_FIXED_PRIORITY). NEVER use on production service channels
     * (runtime-c rules); every other channel flag stays identical to the default mode. */
    QMP_LISTEN_FIXED_PRIORITY = 1U << 0,
};

typedef struct qmp_transport {
    const char *name;

    /* -- client side -- */
    int (*connect)(qmp_conn_t *conn, const char *path);
    /* Blocking round trip. *reply_len receives the total reply bytes when the transport
     * knows it (0 otherwise). Returns EOK or errno (ETIMEDOUT, ESRCH, EBADF, ...). */
    int (*call)(qmp_conn_t *conn, const qmp_iov_t *siov, int sparts, const qmp_iov_t *riov,
                int rparts, uint64_t timeout_ns, size_t *reply_len);
    int (*disconnect)(qmp_conn_t *conn);

    /* -- server side -- */
    int (*listen)(qmp_endpoint_t *ep, const char *path, uint32_t flags);
    /* Blocks until a message or lifecycle event arrives. Returns EOK, or ECANCELED after
     * unblock(). The buffer is the worker's preallocated receive buffer. */
    int (*receive)(qmp_endpoint_t *ep, void *buf, size_t cap, qmp_rcv_info_t *info);
    int (*reply)(qmp_endpoint_t *ep, int64_t rcvid, const qmp_iov_t *iov, int parts);
    int (*error)(qmp_endpoint_t *ep, int64_t rcvid, int err);
    /* Wake every thread blocked in receive() so qmp_server_run() can return. */
    int (*unblock)(qmp_endpoint_t *ep);
    int (*close)(qmp_endpoint_t *ep);

    /* Optional (NULL = not provided). Blocks until a lifecycle event / pulse arrives and
     * never consumes messages (QNX: MsgReceivePulse). Lets the core run a dedicated pulse
     * thread so UNBLOCK/DISCONNECT are served even when every worker is busy in a handler.
     * Returns EOK with info->kind in {UNBLOCK, DISCONNECT, PULSE}, or ECANCELED after
     * unblock(). */
    int (*receive_pulse)(qmp_endpoint_t *ep, qmp_rcv_info_t *info);
    /* Optional (NULL = assume 1). Returns 1 when the client blocked on rcvid is currently
     * asking to be unblocked (QNX: MsgInfo _NTO_MI_UNBLOCK_REQ), 0 when it is not (stale
     * pulse, or the rcvid is gone/reused by a client that still wants the reply). */
    int (*unblock_req)(qmp_endpoint_t *ep, int64_t rcvid);

    /* ---- topics (ADR-003 notify-then-pull). Optional as a group: when notify_open is
     * NULL the transport has no notification support and subscribe fails with ENOSYS. */

    /* Client: create the private notification channel (QNX: ChannelCreate with
     * _NTO_CHF_COID_DISCONNECT + a side-channel connection to self). One per connection;
     * idempotent. Never called on the publish path. */
    int (*notify_open)(qmp_conn_t *conn);
    /* Client: block until a notification pulse arrives on the private channel.
     * Returns EOK (*value = 64-bit pulse value, advisory only), ETIMEDOUT (timeout_ns
     * elapsed; 0 = block forever), ESRCH when the server connection died
     * (QNX: _PULSE_CODE_COIDDEATH — VERIFY(qnx8): V-QNX-046), ECANCELED after
     * notify_close(). Thread: any single caller at a time. */
    int (*notify_wait)(qmp_conn_t *conn, uint64_t timeout_ns, int64_t *value);
    int (*notify_close)(qmp_conn_t *conn);
    /* Client: build and register an event that ONLY the connected server may deliver
     * (QNX: SIGEV_PULSE on the notify channel + MsgRegisterEvent(ev, server coid)).
     * Writes the opaque registered blob (<= QMP_EVENT_BLOB_MAX) for SUBSCRIBE. */
    int (*event_register)(qmp_conn_t *conn, uint8_t *blob, size_t cap, size_t *len);
    int (*event_unregister)(qmp_conn_t *conn, const uint8_t *blob, size_t len);
    /* Server: deliver a registered event to the subscriber identified by the rcvid of
     * its SUBSCRIBE message (QNX: MsgDeliverEvent — the rcvid stays usable for event
     * delivery after the reply, VERIFY(qnx8): V-QNX-043). Never blocks. value is the
     * advisory pulse value (sequence number). Returns EOK, or ESRCH/EACCES/EBADF when
     * the subscriber or its registration is gone (caller drops the subscription). */
    int (*deliver)(qmp_endpoint_t *ep, int64_t rcvid, const uint8_t *blob, size_t len,
                   int64_t value);
} qmp_transport_t;

/* In-process transport for unit tests (runtime/transport/fake). Thread-safe. */
const qmp_transport_t *qmp_transport_fake(void);
/* Test-only (fake transport): simulate a signal hitting a client that is REPLY-blocked on
 * the endpoint at `path` (kernel unblock pulse without the client's deadline passing).
 * Returns EOK, or ESRCH when no client is currently REPLY-blocked there. */
int qmp_transport_fake_unblock_inject(const char *path);
/* Native QNX message passing (runtime/transport/qnx). Only defined on __QNX__. */
const qmp_transport_t *qmp_transport_qnx(void);
/* qnx on __QNX__; fake on the host until the uds transport lands (W3). */
const qmp_transport_t *qmp_transport_default(void);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_TRANSPORT_H */
