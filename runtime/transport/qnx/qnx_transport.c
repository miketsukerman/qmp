/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Native QNX message-passing transport (DESIGN §1, §3).
 *
 * Server: the channel is ALWAYS created by us (ChannelCreate), wrapped in a dispatch
 * handle (dispatch_create_channel) and registered with name_attach() as
 * "qmp/<svc>-<inst>/v<major>" under /dev/name/local. The flags are
 * _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT — the same set
 * name_attach() would have used (VERIFY(qnx8): V-QNX-023/V-QNX-039) — plus, only when
 * QMP_LISTEN_FIXED_PRIORITY is requested (negative control for demos),
 * _NTO_CHF_FIXED_PRIORITY. That flag is the ONLY difference between the two modes.
 *
 * Receive loop order (runtime-c.instructions.md):
 *   1. pulses (_PULSE_CODE_DISCONNECT, _PULSE_CODE_UNBLOCK, others)
 *   2. _IO_CONNECT -> MsgReply(EOK)        VERIFY(qnx8): V-QNX-003
 *   3. other _IO_* -> MsgError(ENOSYS)
 *   4. qmp messages -> returned to the core, which validates the header fully
 *   5. anything else -> MsgError(EINVAL)
 *
 * A dedicated core thread calls receive_pulse() (MsgReceivePulse) so UNBLOCK/DISCONNECT
 * pulses are served even when every worker is busy inside a handler (V-QNX-038).
 *
 * Client: name_open() + MsgSendv with TimerTimeout(_NTO_TIMEOUT_SEND|_NTO_TIMEOUT_REPLY),
 * covering both the SEND-blocked and the REPLY-blocked state.
 *
 * No heap on the call path. The per-scoid credential cache is allocated once in listen()
 * (init time) and freed in close().
 */
#if defined(__QNX__)

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pthread.h>
#include <sys/dispatch.h>
#include <sys/iomsg.h>
#include <sys/neutrino.h>

#include <qmp/qmp_transport.h>

/* qmp_iov_t is layout-compatible with the QNX iov_t by design. */
QMP_STATIC_ASSERT(sizeof(qmp_iov_t) == sizeof(iov_t), "iov layout");
QMP_STATIC_ASSERT(offsetof(qmp_iov_t, base) == offsetof(iov_t, iov_base), "iov layout");
QMP_STATIC_ASSERT(offsetof(qmp_iov_t, len) == offsetof(iov_t, iov_len), "iov layout");

/* Internal stop pulse used by unblock(); workers daisy-chain it so every receiver wakes. */
#define QMP_QNX_PULSE_STOP (_PULSE_CODE_MINAVAIL + 0)

/* ---- per-scoid credential cache (kernel-supplied, cached on first message) -- */
#define QMP_QNX_CRED_CACHE 64

typedef struct cred_entry {
    int32_t scoid; /* 0 = free slot */
    int32_t pid;
    int32_t uid;
    int32_t gid;
    uint32_t type_id;
} cred_entry_t;

typedef struct qnx_ep_impl {
    name_attach_t *attach;
    dispatch_t *dpp;     /* wraps our own chid; see qnx_listen()/qnx_close()  */
    cred_entry_t *creds; /* QMP_QNX_CRED_CACHE entries, allocated in listen() */
    int chid;            /* created by us with ChannelCreate                  */
    int self_coid;       /* side channel for the stop pulse                   */
    volatile unsigned stop;
    pthread_mutex_t cred_mu;
} qnx_ep_impl_t;
QMP_STATIC_ASSERT(sizeof(qnx_ep_impl_t) <= sizeof(qmp_endpoint_t), "endpoint storage");

typedef struct qnx_conn_impl {
    int coid;
} qnx_conn_impl_t;
QMP_STATIC_ASSERT(sizeof(qnx_conn_impl_t) <= sizeof(qmp_conn_t), "conn storage");

/* ---- client --------------------------------------------------------------- */

static int qnx_connect(qmp_conn_t *conn, const char *path) {
    qnx_conn_impl_t *c = (qnx_conn_impl_t *)(void *)conn;
    /* VERIFY(qnx8): V-QNX-002 — hierarchical names with slashes resolve via name_open. */
    int coid = name_open(path, 0);
    if (coid == -1) {
        return errno != 0 ? errno : ENOENT;
    }
    c->coid = coid;
    return EOK;
}

static int qnx_call(qmp_conn_t *conn, const qmp_iov_t *siov, int sparts, const qmp_iov_t *riov,
                    int rparts, uint64_t timeout_ns, size_t *reply_len) {
    qnx_conn_impl_t *c = (qnx_conn_impl_t *)(void *)conn;
    *reply_len = 0;
    if (timeout_ns != 0) {
        uint64_t ns = timeout_ns;
        /* Arms a one-shot kernel timeout for the next blocking state (DESIGN Q2).
         * VERIFY(qnx8): V-QNX-006 — client sees ETIMEDOUT only after the server
         * answers the unblock pulse with MsgError. */
        (void)TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND | _NTO_TIMEOUT_REPLY, NULL, &ns,
                           NULL);
    }
    /* The server replies with status = total reply bytes, so MsgSendv's return value is
     * the reply length (lets the core detect short replies against hdr.payload_len). */
    long rc = MsgSendv(c->coid, (const iov_t *)(const void *)siov, (size_t)sparts,
                       (const iov_t *)(const void *)riov, (size_t)rparts);
    if (rc == -1) {
        return errno != 0 ? errno : EIO;
    }
    *reply_len = rc > 0 ? (size_t)rc : 0;
    return EOK;
}

static int qnx_disconnect(qmp_conn_t *conn) {
    qnx_conn_impl_t *c = (qnx_conn_impl_t *)(void *)conn;
    if (c->coid > 0) {
        (void)name_close(c->coid);
        c->coid = -1;
    }
    return EOK;
}

/* ---- server --------------------------------------------------------------- */

static int qnx_listen(qmp_endpoint_t *ep_, const char *path, uint32_t flags) {
    qnx_ep_impl_t *e = (qnx_ep_impl_t *)(void *)ep_;
    memset(e, 0, sizeof(*e));
    e->chid = -1;
    e->self_coid = -1;
    e->creds = calloc(QMP_QNX_CRED_CACHE, sizeof(cred_entry_t)); /* init time only */
    if (e->creds == NULL) {
        return ENOMEM;
    }
    (void)pthread_mutex_init(&e->cred_mu, NULL);
    /* Always create the channel ourselves so QMP_LISTEN_FIXED_PRIORITY is the single
     * difference between the two modes; every flag name_attach() would have set stays.
     * VERIFY(qnx8): V-QNX-039 — name_attach() over dispatch_create_channel() keeps our
     * chid (attach->chid == chid) and sets no extra channel flags. */
    unsigned cflags = _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT | _NTO_CHF_COID_DISCONNECT;
    if ((flags & QMP_LISTEN_FIXED_PRIORITY) != 0) {
        /* Negative control for priority-inheritance demos ONLY (V-QNX-041). */
        cflags |= _NTO_CHF_FIXED_PRIORITY;
    }
    int err;
    e->chid = ChannelCreate(cflags);
    if (e->chid == -1) {
        err = errno != 0 ? errno : EIO;
        goto fail;
    }
    e->dpp = dispatch_create_channel(e->chid, 0);
    if (e->dpp == NULL) {
        err = errno != 0 ? errno : EIO;
        goto fail;
    }
    /* The flags argument is documented as unused for local attach in 8.0. */
    e->attach = name_attach(e->dpp, path, 0);
    if (e->attach == NULL) {
        err = errno != 0 ? errno : EADDRINUSE;
        goto fail;
    }
    e->self_coid = ConnectAttach(0, 0, e->attach->chid, _NTO_SIDE_CHANNEL, 0);
    if (e->self_coid == -1) {
        err = errno != 0 ? errno : EIO;
        (void)name_detach(e->attach, 0);
        e->attach = NULL;
        e->chid = -1; /* name_detach destroyed the channel (V-QNX-040) */
        goto fail;
    }
    return EOK;
fail:
    if (e->chid != -1) {
        (void)ChannelDestroy(e->chid);
        e->chid = -1;
    }
    e->dpp = NULL;
    free(e->creds);
    e->creds = NULL;
    return err;
}

/* ConnectClientInfo is called once per scoid, then served from the cache. It uses
 * caller storage (no heap), unlike ConnectClientInfoExt. */
static void qnx_fill_creds(qnx_ep_impl_t *e, int32_t scoid, qmp_rcv_info_t *info) {
    (void)pthread_mutex_lock(&e->cred_mu);
    cred_entry_t *free_slot = NULL;
    const cred_entry_t *hit = NULL;
    for (int i = 0; i < QMP_QNX_CRED_CACHE; i++) {
        if (e->creds[i].scoid == scoid) {
            hit = &e->creds[i];
            break;
        }
        if (free_slot == NULL && e->creds[i].scoid == 0) {
            free_slot = &e->creds[i];
        }
    }
    cred_entry_t ent;
    if (hit == NULL) {
        struct _client_info ci;
        memset(&ci, 0, sizeof(ci));
        /* VERIFY(qnx8): V-QNX-014 — the secpol type id needs ConnectClientInfoExt with
         * _NTO_CLIENTINFO_GETTYPEID; until that is verified on target, type_id stays 0. */
        if (ConnectClientInfo(scoid, &ci, 0) != EOK) {
            (void)pthread_mutex_unlock(&e->cred_mu);
            return; /* cred_valid stays 0; the ACL hook decides what to do */
        }
        ent.scoid = scoid;
        ent.pid = (int32_t)ci.pid;
        ent.uid = (int32_t)ci.cred.euid;
        ent.gid = (int32_t)ci.cred.egid;
        ent.type_id = 0;
        if (free_slot != NULL) {
            *free_slot = ent; /* cache full: still answered, just not cached */
        }
        hit = &ent;
    }
    info->pid = hit->pid;
    info->uid = hit->uid;
    info->gid = hit->gid;
    info->type_id = hit->type_id;
    info->cred_valid = 1;
    (void)pthread_mutex_unlock(&e->cred_mu);
}

static int32_t qnx_drop_creds(qnx_ep_impl_t *e, int32_t scoid) {
    int32_t pid = 0;
    (void)pthread_mutex_lock(&e->cred_mu);
    for (int i = 0; i < QMP_QNX_CRED_CACHE; i++) {
        if (e->creds[i].scoid == scoid) {
            pid = e->creds[i].pid;
            memset(&e->creds[i], 0, sizeof(e->creds[i]));
            break;
        }
    }
    (void)pthread_mutex_unlock(&e->cred_mu);
    return pid;
}

/* Shared pulse handling for qnx_receive() and qnx_receive_pulse(). Returns EOK with
 * *info filled, or ECANCELED on the internal stop pulse. */
static int qnx_handle_pulse(qnx_ep_impl_t *e, const struct _pulse *p, qmp_rcv_info_t *info) {
    switch (p->code) {
    case _PULSE_CODE_DISCONNECT:
        /* Once per client process when its last connection goes (V-QNX-004).
         * ConnectDetach here; the core releases its per-process state. */
        info->kind = QMP_RCV_DISCONNECT;
        info->scoid = p->scoid;
        info->pid = qnx_drop_creds(e, p->scoid);
        (void)ConnectDetach(p->scoid);
        return EOK;
    case _PULSE_CODE_UNBLOCK:
        /* The pulse value carries the rcvid of the blocked client (64-bit).
         * VERIFY(qnx8): V-QNX-031 — sigval member holding it on 8.0. */
        info->kind = QMP_RCV_UNBLOCK;
        info->rcvid = (int64_t)(intptr_t)p->value.sival_ptr;
        info->scoid = p->scoid;
        return EOK;
    case QMP_QNX_PULSE_STOP:
        /* Daisy-chain so every receiver (workers and pulse thread) wakes up. */
        e->stop = 1;
        (void)MsgSendPulse(e->self_coid, -1, QMP_QNX_PULSE_STOP, 0);
        return ECANCELED;
    default:
        info->kind = QMP_RCV_PULSE;
        info->pulse_code = p->code;
        info->pulse_value = (int64_t)(intptr_t)p->value.sival_ptr;
        info->scoid = p->scoid;
        return EOK;
    }
}

static int qnx_receive(qmp_endpoint_t *ep_, void *buf, size_t cap, qmp_rcv_info_t *info) {
    qnx_ep_impl_t *e = (qnx_ep_impl_t *)(void *)ep_;
    if (cap < sizeof(struct _pulse) || cap < sizeof(uint16_t)) {
        return EINVAL;
    }
    for (;;) {
        if (e->stop != 0) {
            return ECANCELED;
        }
        struct _msg_info mi;
        memset(&mi, 0, sizeof(mi));
        rcvid_t rcvid = MsgReceive(e->attach->chid, buf, cap, &mi);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ESRCH) {
                return ECANCELED; /* channel destroyed under us */
            }
            return errno != 0 ? errno : EIO;
        }
        memset(info, 0, sizeof(*info));
        if (rcvid == 0) {
            /* 1. Pulses. Values are 64 bits in 8.0; never truncate. */
            return qnx_handle_pulse(e, (const struct _pulse *)buf, info);
        }
        uint16_t type;
        if (mi.msglen < (int64_t)sizeof(type)) {
            (void)MsgError(rcvid, EINVAL);
            continue;
        }
        memcpy(&type, buf, sizeof(type));
        if (type == _IO_CONNECT) {
            /* 2. name_open() sends _IO_CONNECT; answer EOK so the open succeeds. */
            (void)MsgReply(rcvid, EOK, NULL, 0);
            continue;
        }
        if (type <= _IO_MAX) {
            /* 3. Other resource-manager messages are not supported. */
            (void)MsgError(rcvid, ENOSYS);
            continue;
        }
        if (type != QMP_MSG_TYPE) {
            /* 5. Unknown non-qmp message. */
            (void)MsgError(rcvid, EINVAL);
            continue;
        }
        /* 4. qmp envelope: hand to the core, which validates the header fully. */
        info->kind = QMP_RCV_MESSAGE;
        info->rcvid = (int64_t)rcvid;
        info->scoid = mi.scoid;
        info->priority = mi.priority;
        info->msglen = (uint32_t)mi.msglen;
        info->srcmsglen = (uint32_t)mi.srcmsglen;
        /* dstmsglen = client reply capacity: undersized reply buffers are detected
         * explicitly by the core instead of being silently truncated (V-QNX-013). */
        info->dstmsglen = (uint32_t)mi.dstmsglen;
        info->pid = (int32_t)mi.pid;
        qnx_fill_creds(e, mi.scoid, info);
        return EOK;
    }
}

/* Pulse-only receive for the core's dedicated pulse thread. MsgReceivePulse() never
 * consumes a message, so client sends stay queued for the MsgReceive workers while
 * UNBLOCK/DISCONNECT pulses are served even when all workers are busy.
 * VERIFY(qnx8): V-QNX-038 — pulse routing between MsgReceive- and MsgReceivePulse-blocked
 * threads on one channel. */
static int qnx_receive_pulse(qmp_endpoint_t *ep_, qmp_rcv_info_t *info) {
    qnx_ep_impl_t *e = (qnx_ep_impl_t *)(void *)ep_;
    for (;;) {
        if (e->stop != 0) {
            return ECANCELED;
        }
        struct _pulse p;
        memset(&p, 0, sizeof(p));
        if (MsgReceivePulse(e->attach->chid, &p, sizeof(p), NULL) == -1) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == ESRCH) {
                return ECANCELED; /* channel destroyed under us */
            }
            return errno != 0 ? errno : EIO;
        }
        memset(info, 0, sizeof(*info));
        return qnx_handle_pulse(e, &p, info);
    }
}

/* Is the client blocked on rcvid still asking to be unblocked? Defeats stale pulses: a
 * reused rcvid whose client wants its reply reports no unblock request, so it is never
 * cancelled by an old pulse. VERIFY(qnx8): V-QNX-037 — _NTO_MI_UNBLOCK_REQ semantics. */
static int qnx_unblock_req(qmp_endpoint_t *ep_, int64_t rcvid) {
    (void)ep_;
    struct _msg_info mi;
    memset(&mi, 0, sizeof(mi));
    if (MsgInfo((rcvid_t)rcvid, &mi) == -1) {
        return 0; /* rcvid gone: nothing left to cancel */
    }
    return (mi.flags & _NTO_MI_UNBLOCK_REQ) != 0;
}

static int qnx_reply(qmp_endpoint_t *ep_, int64_t rcvid, const qmp_iov_t *iov, int parts) {
    (void)ep_;
    size_t total = 0;
    for (int i = 0; i < parts; i++) {
        total += iov[i].len;
    }
    /* status = reply byte count; the client reads it from MsgSendv's return value. */
    if (MsgReplyv((rcvid_t)rcvid, (int)total, (const iov_t *)(const void *)iov, (size_t)parts) ==
        -1) {
        return errno != 0 ? errno : EIO;
    }
    return EOK;
}

static int qnx_error(qmp_endpoint_t *ep_, int64_t rcvid, int err) {
    (void)ep_;
    if (MsgError((rcvid_t)rcvid, err) == -1) {
        return errno != 0 ? errno : EIO;
    }
    return EOK;
}

static int qnx_unblock(qmp_endpoint_t *ep_) {
    qnx_ep_impl_t *e = (qnx_ep_impl_t *)(void *)ep_;
    e->stop = 1;
    if (MsgSendPulse(e->self_coid, -1, QMP_QNX_PULSE_STOP, 0) == -1) {
        return errno != 0 ? errno : EIO;
    }
    return EOK;
}

static int qnx_close(qmp_endpoint_t *ep_) {
    qnx_ep_impl_t *e = (qnx_ep_impl_t *)(void *)ep_;
    int rc = EOK;
    if (e->self_coid > 0) {
        (void)ConnectDetach(e->self_coid);
        e->self_coid = -1;
    }
    if (e->attach != NULL) {
        /* VERIFY(qnx8): V-QNX-040 — name_detach(attach, 0) removes the name and destroys
         * the channel, including one we supplied via dispatch_create_channel(). The
         * dispatch handle itself is intentionally not freed: close happens once per
         * process lifetime and dispatch has no documented channel-preserving destroy. */
        if (name_detach(e->attach, 0) == -1) {
            rc = errno != 0 ? errno : EIO;
        }
        e->attach = NULL;
        e->dpp = NULL;
        e->chid = -1;
    }
    (void)pthread_mutex_destroy(&e->cred_mu);
    free(e->creds);
    e->creds = NULL;
    return rc;
}

static const qmp_transport_t g_qnx = {
    "qnx",     qnx_connect, qnx_call,    qnx_disconnect, qnx_listen,        qnx_receive,
    qnx_reply, qnx_error,   qnx_unblock, qnx_close,      qnx_receive_pulse, qnx_unblock_req,
};

const qmp_transport_t *qmp_transport_qnx(void) {
    return &g_qnx;
}

const qmp_transport_t *qmp_transport_default(void) {
    return &g_qnx; /* native message passing is the default on QNX */
}

#endif /* __QNX__ */
