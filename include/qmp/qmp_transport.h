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
#define QMP_ENDPOINT_STORAGE_U64 8

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
    int64_t rcvid;      /* reply token; transport-specific                       */
    int32_t scoid;      /* server-side connection id (QNX) or client slot (fake)  */
    int32_t pid;        /* sender pid when known, 0 otherwise                     */
    int32_t priority;   /* sender priority when known, 0 otherwise                */
    uint32_t msglen;    /* bytes copied into the buffer                           */
    uint32_t srcmsglen; /* bytes the sender sent (> msglen means truncated)       */
    uint16_t kind;      /* enum qmp_rcv_kind                                      */
    int8_t pulse_code;
    uint8_t _pad0;
    int32_t pulse_value;
} qmp_rcv_info_t;

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
    int (*listen)(qmp_endpoint_t *ep, const char *path);
    /* Blocks until a message or lifecycle event arrives. Returns EOK, or ECANCELED after
     * unblock(). The buffer is the worker's preallocated receive buffer. */
    int (*receive)(qmp_endpoint_t *ep, void *buf, size_t cap, qmp_rcv_info_t *info);
    int (*reply)(qmp_endpoint_t *ep, int64_t rcvid, const qmp_iov_t *iov, int parts);
    int (*error)(qmp_endpoint_t *ep, int64_t rcvid, int err);
    /* Wake every thread blocked in receive() so qmp_server_run() can return. */
    int (*unblock)(qmp_endpoint_t *ep);
    int (*close)(qmp_endpoint_t *ep);
} qmp_transport_t;

/* In-process transport for unit tests (runtime/transport/fake). Thread-safe. */
const qmp_transport_t *qmp_transport_fake(void);
/* qnx on __QNX__ (W2); fake on the host until the uds transport lands (W3). */
const qmp_transport_t *qmp_transport_default(void);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_TRANSPORT_H */
