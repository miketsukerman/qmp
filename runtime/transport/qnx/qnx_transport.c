/* SPDX-License-Identifier: Apache-2.0 */
/*
 * QNX native transport (MsgSend/MsgReceive/MsgReply) — PLACEHOLDER until W2.
 *
 * client.c and server.c fall back to qmp_transport_default() when the config leaves the
 * transport NULL, so the symbol must exist on target for libqmp to link. Until the real
 * transport lands, every operation fails with ENOSYS. This is deliberately not the in-process
 * fake: on target, client and server live in different processes, and silently routing them
 * through a per-process registry would turn into a confusing ENOENT instead of a clear error.
 * Code that wants the fake on target (e.g. qmp_core_tests) passes qmp_transport_fake().
 */
#include <errno.h>

#include <qmp/qmp_transport.h>

static int qnx_connect(qmp_conn_t *conn, const char *path) {
    (void)conn;
    (void)path;
    return ENOSYS;
}

static int qnx_call(qmp_conn_t *conn, const qmp_iov_t *siov, int sparts, const qmp_iov_t *riov,
                    int rparts, uint64_t timeout_ns, size_t *reply_len) {
    (void)conn;
    (void)siov;
    (void)sparts;
    (void)riov;
    (void)rparts;
    (void)timeout_ns;
    if (reply_len != NULL) {
        *reply_len = 0;
    }
    return ENOSYS;
}

static int qnx_disconnect(qmp_conn_t *conn) {
    (void)conn;
    return ENOSYS;
}

static int qnx_listen(qmp_endpoint_t *ep, const char *path) {
    (void)ep;
    (void)path;
    return ENOSYS;
}

static int qnx_receive(qmp_endpoint_t *ep, void *buf, size_t cap, qmp_rcv_info_t *info) {
    (void)ep;
    (void)buf;
    (void)cap;
    (void)info;
    return ENOSYS;
}

static int qnx_reply(qmp_endpoint_t *ep, int64_t rcvid, const qmp_iov_t *iov, int parts) {
    (void)ep;
    (void)rcvid;
    (void)iov;
    (void)parts;
    return ENOSYS;
}

static int qnx_error(qmp_endpoint_t *ep, int64_t rcvid, int err) {
    (void)ep;
    (void)rcvid;
    (void)err;
    return ENOSYS;
}

static int qnx_unblock(qmp_endpoint_t *ep) {
    (void)ep;
    return ENOSYS;
}

static int qnx_close(qmp_endpoint_t *ep) {
    (void)ep;
    return ENOSYS;
}

static const qmp_transport_t g_qnx_stub = {
    "qnx-stub", qnx_connect, qnx_call,  qnx_disconnect, qnx_listen,
    qnx_receive, qnx_reply,  qnx_error, qnx_unblock,    qnx_close,
};

const qmp_transport_t *qmp_transport_default(void) {
    return &g_qnx_stub; /* W2 replaces this file with the MsgSend/MsgReceive transport */
}
