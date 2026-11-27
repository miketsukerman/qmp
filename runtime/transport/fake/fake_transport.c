/* SPDX-License-Identifier: Apache-2.0 */
/*
 * In-process fake transport for host unit tests.
 *
 * Models the parts of QNX message passing the core relies on: a client blocks in call()
 * until the server replies or errors (so its IOVs can be read in place, zero-copy, like
 * MsgReceive copying from the SEND-blocked client), a server has N threads blocked in
 * receive(), reply/error are matched by rcvid, unblock() wakes every receiver, and a closed
 * endpoint makes pending callers fail with ESRCH.
 *
 * Global state: one documented init-time registry of QMP_FAKE_MAX_ENDPOINTS endpoints keyed
 * by path (runtime-c rules allow documented registries). Test-only; never built for target.
 */
#include <pthread.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <qmp/qmp_transport.h>

#define QMP_FAKE_MAX_ENDPOINTS 8
#define QMP_FAKE_MAX_SLOTS     16
#define QMP_FAKE_MAX_CONNS     16
#define QMP_FAKE_MAX_EVENTS    32
#define QMP_FAKE_MAX_NOTIFY    16
#define QMP_FAKE_NOTIFY_DEPTH  8
#define QMP_FAKE_MAX_REGS      32
#define QMP_FAKE_EVENT_MAGIC   0x514D5045U /* "QMPE" */

enum slot_state {
    SLOT_FREE = 0,
    SLOT_PENDING,  /* client posted, waiting for a receiver   */
    SLOT_RECEIVED, /* a server thread took it                 */
    SLOT_DONE,     /* server replied / errored; client wakes  */
    SLOT_ABANDONED /* client gave up while RECEIVED           */
};

typedef struct slot {
    enum slot_state state;
    uint32_t gen;
    const qmp_iov_t *siov;
    int sparts;
    const qmp_iov_t *riov;
    int rparts;
    size_t reply_len;
    int err; /* 0 = replied, else errno for the client */
    int32_t scoid;
    bool unblock_sent; /* the timeout already queued an UNBLOCK event */
} slot_t;

/* Lifecycle events (unblock/disconnect pulses on QNX), delivered before messages. */
typedef struct fake_event {
    uint16_t kind; /* enum qmp_rcv_kind */
    int32_t scoid;
    int32_t pid;
    int64_t rcvid;
} fake_event_t;

typedef struct endpoint {
    bool used;
    bool shutdown;
    uint32_t gen; /* bumped on every listen(): detects stale connections */
    char path[QMP_NAME_MAX];
    slot_t slots[QMP_FAKE_MAX_SLOTS];
    int32_t scoid_seq;
    bool conns[QMP_FAKE_MAX_CONNS]; /* indexed by scoid - 1 (mod table) */
    fake_event_t evq[QMP_FAKE_MAX_EVENTS];
    int ev_head;
    int ev_count;
    pthread_cond_t cv_server; /* signalled when a slot becomes PENDING or on shutdown */
    pthread_cond_t cv_client; /* broadcast when any slot becomes DONE                 */
} endpoint_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static endpoint_t g_eps[QMP_FAKE_MAX_ENDPOINTS];
static bool g_inited;

/* ---- notification channels (model of the client's private pulse channel) --- */
/* Bounded pulse queue per channel: a full queue silently drops the pulse, exactly like
 * MsgDeliverEvent failing to queue on QNX (V-QNX-044) — correctness must come from the
 * pull cursor, never from pulse delivery. */
typedef struct notify_chan {
    bool used;
    bool server_gone; /* the endpoint a registration pointed at shut down (COIDDEATH) */
    uint32_t gen;
    int64_t q[QMP_FAKE_NOTIFY_DEPTH];
    int q_head;
    int q_count;
    pthread_cond_t cv;
} notify_chan_t;
static notify_chan_t g_notify[QMP_FAKE_MAX_NOTIFY];

/* Registered events (model of MsgRegisterEvent): bound to the registering connection's
 * endpoint; deliver() through any other endpoint is refused with EACCES, like the kernel
 * refusing an unregistered event (V-QNX-042). Cleared when the connection goes away,
 * like kernel cleanup on ConnectDetach / process exit. */
typedef struct fake_reg {
    bool used;
    int notify_idx;
    uint32_t notify_gen;
    int ep_index;
    uint32_t ep_gen;
    int32_t scoid; /* registering connection */
    uint32_t reg_id;
} fake_reg_t;
static fake_reg_t g_regs[QMP_FAKE_MAX_REGS];
static uint32_t g_reg_seq;

/* Wire form of a fake registered event inside the SUBSCRIBE blob. */
typedef struct fake_event_blob {
    uint32_t magic;
    uint32_t reg_id;
    int32_t reg_idx;
    uint32_t _pad0;
} fake_event_blob_t;
QMP_STATIC_ASSERT(sizeof(fake_event_blob_t) <= QMP_EVENT_BLOB_MAX, "fake event blob");

static void init_locked(void) {
    if (g_inited) {
        return;
    }
    for (int i = 0; i < QMP_FAKE_MAX_ENDPOINTS; i++) {
        (void)pthread_cond_init(&g_eps[i].cv_server, NULL);
        (void)pthread_cond_init(&g_eps[i].cv_client, NULL);
    }
    for (int i = 0; i < QMP_FAKE_MAX_NOTIFY; i++) {
        (void)pthread_cond_init(&g_notify[i].cv, NULL);
    }
    g_inited = true;
}

typedef struct conn_impl {
    int ep_index;
    uint32_t ep_gen;
    int32_t scoid;   /* 0 = not connected */
    int notify_idx1; /* notify channel index + 1; 0 = none */
    uint32_t notify_gen;
} conn_impl_t;
QMP_STATIC_ASSERT(sizeof(conn_impl_t) <= sizeof(qmp_conn_t), "conn storage");

typedef struct ep_impl {
    int ep_index;
} ep_impl_t;
QMP_STATIC_ASSERT(sizeof(ep_impl_t) <= sizeof(qmp_endpoint_t), "endpoint storage");

static endpoint_t *find_locked(const char *path) {
    for (int i = 0; i < QMP_FAKE_MAX_ENDPOINTS; i++) {
        if (g_eps[i].used && strcmp(g_eps[i].path, path) == 0) {
            return &g_eps[i];
        }
    }
    return NULL;
}

static size_t iov_total(const qmp_iov_t *iov, int parts) {
    size_t n = 0;
    for (int i = 0; i < parts; i++) {
        n += iov[i].len;
    }
    return n;
}

/* Copy up to cap bytes of iov into a flat buffer; returns bytes copied. */
static size_t iov_gather(const qmp_iov_t *iov, int parts, uint8_t *dst, size_t cap) {
    size_t off = 0;
    for (int i = 0; i < parts && off < cap; i++) {
        size_t n = iov[i].len;
        if (n > cap - off) {
            n = cap - off;
        }
        memcpy(dst + off, iov[i].base, n);
        off += n;
    }
    return off;
}

/* Scatter src iovs into dst iovs; returns bytes written (bounded by dst capacity). */
static size_t iov_copy(const qmp_iov_t *src, int sparts, const qmp_iov_t *dst, int dparts) {
    size_t written = 0;
    int di = 0;
    size_t doff = 0;
    for (int si = 0; si < sparts; si++) {
        const uint8_t *p = (const uint8_t *)src[si].base;
        size_t left = src[si].len;
        while (left > 0 && di < dparts) {
            size_t room = dst[di].len - doff;
            if (room == 0) {
                di++;
                doff = 0;
                continue;
            }
            size_t n = left < room ? left : room;
            memcpy((uint8_t *)dst[di].base + doff, p, n);
            p += n;
            left -= n;
            doff += n;
            written += n;
        }
    }
    return written;
}

static int64_t make_rcvid(int slot, uint32_t gen) {
    return ((int64_t)gen << 16) | (int64_t)slot;
}

/* g_lock held. Drops the event when the queue is full (tests never get close). */
static void push_event(endpoint_t *ep, uint16_t kind, int32_t scoid, int32_t pid, int64_t rcvid) {
    if (ep->ev_count >= QMP_FAKE_MAX_EVENTS) {
        return;
    }
    fake_event_t *ev = &ep->evq[(ep->ev_head + ep->ev_count) % QMP_FAKE_MAX_EVENTS];
    ev->kind = kind;
    ev->scoid = scoid;
    ev->pid = pid;
    ev->rcvid = rcvid;
    ep->ev_count++;
    (void)pthread_cond_broadcast(&ep->cv_server);
}

/* g_lock held. Kernel-cleanup model: registrations die with the registering connection. */
static void drop_regs_for_conn(int ep_index, uint32_t ep_gen, int32_t scoid) {
    for (int i = 0; i < QMP_FAKE_MAX_REGS; i++) {
        fake_reg_t *r = &g_regs[i];
        if (r->used && r->ep_index == ep_index && r->ep_gen == ep_gen && r->scoid == scoid) {
            memset(r, 0, sizeof(*r));
        }
    }
}

/* g_lock held. The server endpoint shut down: registrations on it die and their notify
 * channels observe COIDDEATH (server_gone), like _NTO_CHF_COID_DISCONNECT (V-QNX-046). */
static void drop_regs_for_ep(int ep_index, uint32_t ep_gen) {
    for (int i = 0; i < QMP_FAKE_MAX_REGS; i++) {
        fake_reg_t *r = &g_regs[i];
        if (r->used && r->ep_index == ep_index && r->ep_gen == ep_gen) {
            notify_chan_t *nc = &g_notify[r->notify_idx];
            if (nc->used && nc->gen == r->notify_gen) {
                nc->server_gone = true;
                (void)pthread_cond_broadcast(&nc->cv);
            }
            memset(r, 0, sizeof(*r));
        }
    }
}

/* ---- client ------------------------------------------------------------- */
static int fake_connect(qmp_conn_t *conn, const char *path) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    (void)pthread_mutex_lock(&g_lock);
    init_locked();
    endpoint_t *ep = find_locked(path);
    int rc = EOK;
    if (ep == NULL || ep->shutdown) {
        rc = ENOENT;
    } else {
        int32_t scoid = 0;
        for (int i = 0; i < QMP_FAKE_MAX_CONNS; i++) {
            int32_t cand = ++ep->scoid_seq;
            if (cand <= 0) { /* wrapped */
                cand = ep->scoid_seq = 1;
            }
            if (!ep->conns[(cand - 1) % QMP_FAKE_MAX_CONNS]) {
                scoid = cand;
                ep->conns[(cand - 1) % QMP_FAKE_MAX_CONNS] = true;
                break;
            }
        }
        if (scoid == 0) {
            rc = EAGAIN;
        } else {
            c->ep_index = (int)(ep - g_eps);
            c->ep_gen = ep->gen;
            c->scoid = scoid;
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static void abs_deadline(struct timespec *ts, uint64_t timeout_ns) {
    (void)clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += (time_t)(timeout_ns / 1000000000ULL);
    ts->tv_nsec += (long)(timeout_ns % 1000000000ULL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static int fake_call(qmp_conn_t *conn, const qmp_iov_t *siov, int sparts, const qmp_iov_t *riov,
                     int rparts, uint64_t timeout_ns, size_t *reply_len) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    *reply_len = 0;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[c->ep_index];
    if (!ep->used || ep->shutdown || ep->gen != c->ep_gen) {
        (void)pthread_mutex_unlock(&g_lock);
        return ESRCH;
    }
    slot_t *sl = NULL;
    for (int i = 0; i < QMP_FAKE_MAX_SLOTS; i++) {
        if (ep->slots[i].state == SLOT_FREE) {
            sl = &ep->slots[i];
            break;
        }
    }
    if (sl == NULL) {
        (void)pthread_mutex_unlock(&g_lock);
        return EAGAIN;
    }
    sl->state = SLOT_PENDING;
    sl->gen++;
    sl->siov = siov;
    sl->sparts = sparts;
    sl->riov = riov;
    sl->rparts = rparts;
    sl->reply_len = 0;
    sl->err = 0;
    sl->scoid = c->scoid;
    sl->unblock_sent = false;
    /* Broadcast: a signal could wake only the pulse thread, which never takes messages. */
    (void)pthread_cond_broadcast(&ep->cv_server);

    struct timespec deadline;
    if (timeout_ns != 0) {
        abs_deadline(&deadline, timeout_ns);
    }
    int rc = EOK;
    while (sl->state != SLOT_DONE) {
        if (ep->shutdown || !ep->used) {
            rc = ESRCH; /* server died while we were blocked */
            break;
        }
        if (timeout_ns != 0 && !sl->unblock_sent) {
            int w = pthread_cond_timedwait(&ep->cv_client, &g_lock, &deadline);
            if (w == ETIMEDOUT && sl->state != SLOT_DONE) {
                if (sl->state == SLOT_PENDING) {
                    rc = ETIMEDOUT; /* SEND-blocked: the kernel unblocks us directly */
                    break;
                }
                /* REPLY-blocked: the kernel delivers an unblock pulse and keeps the
                 * client blocked until the server replies or errors (V-QNX-006). */
                sl->unblock_sent = true;
                push_event(ep, QMP_RCV_UNBLOCK, sl->scoid, (int32_t)getpid(),
                           make_rcvid((int)(sl - ep->slots), sl->gen));
            }
        } else {
            (void)pthread_cond_wait(&ep->cv_client, &g_lock);
        }
    }
    if (rc == EOK) {
        rc = sl->err;
        *reply_len = sl->reply_len;
        sl->state = SLOT_FREE;
    } else if (sl->state == SLOT_RECEIVED) {
        sl->state = SLOT_ABANDONED; /* server still owns the rcvid; its reply is dropped */
    } else {
        sl->state = SLOT_FREE;
    }
    sl->siov = NULL;
    sl->riov = NULL;
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static int fake_disconnect(qmp_conn_t *conn) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    (void)pthread_mutex_lock(&g_lock);
    if (c->ep_index >= 0 && c->scoid > 0) {
        endpoint_t *ep = &g_eps[c->ep_index];
        if (ep->used && ep->gen == c->ep_gen) {
            ep->conns[(c->scoid - 1) % QMP_FAKE_MAX_CONNS] = false;
            if (!ep->shutdown) {
                /* Last connection of this "process": the disconnect pulse. */
                push_event(ep, QMP_RCV_DISCONNECT, c->scoid, (int32_t)getpid(), 0);
            }
        }
        /* The kernel unregisters events when the registering connection detaches. */
        drop_regs_for_conn(c->ep_index, c->ep_gen, c->scoid);
    }
    (void)pthread_mutex_unlock(&g_lock);
    c->ep_index = -1;
    c->scoid = 0;
    return EOK;
}

/* ---- server ------------------------------------------------------------- */
static int fake_listen(qmp_endpoint_t *ep_, const char *path, uint32_t flags) {
    (void)flags; /* no scheduler in-process: QMP_LISTEN_FIXED_PRIORITY is a no-op */
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    if (strlen(path) >= QMP_NAME_MAX) {
        return ENAMETOOLONG;
    }
    (void)pthread_mutex_lock(&g_lock);
    init_locked();
    if (find_locked(path) != NULL) {
        (void)pthread_mutex_unlock(&g_lock);
        return EADDRINUSE;
    }
    endpoint_t *ep = NULL;
    for (int i = 0; i < QMP_FAKE_MAX_ENDPOINTS; i++) {
        if (!g_eps[i].used) {
            ep = &g_eps[i];
            break;
        }
    }
    if (ep == NULL) {
        (void)pthread_mutex_unlock(&g_lock);
        return ENOSPC;
    }
    ep->used = true;
    ep->shutdown = false;
    ep->gen++;
    memset(ep->slots, 0, sizeof(ep->slots));
    memset(ep->conns, 0, sizeof(ep->conns));
    memset(ep->evq, 0, sizeof(ep->evq));
    ep->scoid_seq = 0;
    ep->ev_head = 0;
    ep->ev_count = 0;
    strncpy(ep->path, path, sizeof(ep->path) - 1);
    ep->path[sizeof(ep->path) - 1] = '\0';
    e->ep_index = (int)(ep - g_eps);
    (void)pthread_mutex_unlock(&g_lock);
    return EOK;
}

static int fake_receive(qmp_endpoint_t *ep_, void *buf, size_t cap, qmp_rcv_info_t *info) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[e->ep_index];
    slot_t *sl = NULL;
    for (;;) {
        if (ep->shutdown) {
            (void)pthread_mutex_unlock(&g_lock);
            return ECANCELED;
        }
        if (ep->ev_count > 0) { /* pulses before messages, like MsgReceive */
            fake_event_t ev = ep->evq[ep->ev_head];
            ep->ev_head = (ep->ev_head + 1) % QMP_FAKE_MAX_EVENTS;
            ep->ev_count--;
            (void)pthread_mutex_unlock(&g_lock);
            memset(info, 0, sizeof(*info));
            info->kind = ev.kind;
            info->scoid = ev.scoid;
            info->pid = ev.pid;
            info->rcvid = ev.rcvid;
            return EOK;
        }
        for (int i = 0; i < QMP_FAKE_MAX_SLOTS; i++) {
            if (ep->slots[i].state == SLOT_PENDING) {
                sl = &ep->slots[i];
                break;
            }
        }
        if (sl != NULL) {
            break;
        }
        (void)pthread_cond_wait(&ep->cv_server, &g_lock);
    }
    sl->state = SLOT_RECEIVED;
    size_t total = iov_total(sl->siov, sl->sparts);
    size_t copied = iov_gather(sl->siov, sl->sparts, (uint8_t *)buf, cap);
    memset(info, 0, sizeof(*info));
    info->rcvid = make_rcvid((int)(sl - ep->slots), sl->gen);
    info->scoid = sl->scoid;
    /* In-process: the sender is this process; credentials mirror ConnectClientInfoExt. */
    info->pid = (int32_t)getpid();
    info->priority = 0;
    info->msglen = (uint32_t)copied;
    info->srcmsglen = (uint32_t)total;
    info->dstmsglen = (uint32_t)iov_total(sl->riov, sl->rparts);
    info->kind = QMP_RCV_MESSAGE;
    info->cred_valid = 1;
    info->uid = (int32_t)getuid();
    info->gid = (int32_t)getgid();
    info->type_id = 0;
    (void)pthread_mutex_unlock(&g_lock);
    return EOK;
}

/* Complete a RECEIVED slot. Returns ESRCH when the client already gave up. */
static int complete(endpoint_t *ep, int64_t rcvid, const qmp_iov_t *iov, int parts, int err) {
    int idx = (int)(rcvid & 0xFFFF);
    uint32_t gen = (uint32_t)(rcvid >> 16);
    if (idx < 0 || idx >= QMP_FAKE_MAX_SLOTS) {
        return EINVAL;
    }
    slot_t *sl = &ep->slots[idx];
    if (sl->gen != gen) {
        return ESRCH;
    }
    if (sl->state == SLOT_ABANDONED) {
        sl->state = SLOT_FREE;
        return ESRCH;
    }
    if (sl->state != SLOT_RECEIVED) {
        return EINVAL;
    }
    if (err == 0) {
        sl->reply_len = iov_copy(iov, parts, sl->riov, sl->rparts);
    }
    sl->err = err;
    sl->state = SLOT_DONE;
    (void)pthread_cond_broadcast(&ep->cv_client);
    return EOK;
}

static int fake_reply(qmp_endpoint_t *ep_, int64_t rcvid, const qmp_iov_t *iov, int parts) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    int rc = complete(&g_eps[e->ep_index], rcvid, iov, parts, 0);
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static int fake_error(qmp_endpoint_t *ep_, int64_t rcvid, int err) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    int rc = complete(&g_eps[e->ep_index], rcvid, NULL, 0, err != 0 ? err : EIO);
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

/* Pulse-only receive: lifecycle events without ever consuming a message (models a thread
 * blocked in MsgReceivePulse on the shared channel). */
static int fake_receive_pulse(qmp_endpoint_t *ep_, qmp_rcv_info_t *info) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[e->ep_index];
    for (;;) {
        if (ep->shutdown) {
            (void)pthread_mutex_unlock(&g_lock);
            return ECANCELED;
        }
        if (ep->ev_count > 0) {
            fake_event_t ev = ep->evq[ep->ev_head];
            ep->ev_head = (ep->ev_head + 1) % QMP_FAKE_MAX_EVENTS;
            ep->ev_count--;
            (void)pthread_mutex_unlock(&g_lock);
            memset(info, 0, sizeof(*info));
            info->kind = ev.kind;
            info->scoid = ev.scoid;
            info->pid = ev.pid;
            info->rcvid = ev.rcvid;
            return EOK;
        }
        (void)pthread_cond_wait(&ep->cv_server, &g_lock);
    }
}

/* MsgInfo(_NTO_MI_UNBLOCK_REQ) model: 1 only while the client owning this rcvid is still
 * blocked on it AND has asked to be unblocked (timeout fired / injected signal). A freed
 * or reused slot (different gen) reports 0, which is what defeats stale pulses. */
static int fake_unblock_req(qmp_endpoint_t *ep_, int64_t rcvid) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    int idx = (int)(rcvid & 0xFFFF);
    uint32_t gen = (uint32_t)(rcvid >> 16);
    if (idx < 0 || idx >= QMP_FAKE_MAX_SLOTS) {
        return 0;
    }
    (void)pthread_mutex_lock(&g_lock);
    slot_t *sl = &g_eps[e->ep_index].slots[idx];
    int req = sl->gen == gen && sl->state == SLOT_RECEIVED && sl->unblock_sent;
    (void)pthread_mutex_unlock(&g_lock);
    return req;
}

static int fake_unblock(qmp_endpoint_t *ep_) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[e->ep_index];
    ep->shutdown = true;
    drop_regs_for_ep(e->ep_index, ep->gen); /* subscribers see COIDDEATH */
    (void)pthread_cond_broadcast(&ep->cv_server);
    (void)pthread_cond_broadcast(&ep->cv_client); /* callers blocked on a dying server */
    (void)pthread_mutex_unlock(&g_lock);
    return EOK;
}

static int fake_close(qmp_endpoint_t *ep_) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[e->ep_index];
    ep->shutdown = true;
    ep->used = false;
    ep->path[0] = '\0';
    drop_regs_for_ep(e->ep_index, ep->gen);
    (void)pthread_cond_broadcast(&ep->cv_server);
    (void)pthread_cond_broadcast(&ep->cv_client);
    (void)pthread_mutex_unlock(&g_lock);
    e->ep_index = -1;
    return EOK;
}

/* ---- topics: notify channel, registered events, delivery ------------------ */

static int fake_notify_open(qmp_conn_t *conn) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    if (c->notify_idx1 != 0) {
        return EOK; /* idempotent */
    }
    (void)pthread_mutex_lock(&g_lock);
    init_locked();
    int rc = EAGAIN;
    for (int i = 0; i < QMP_FAKE_MAX_NOTIFY; i++) {
        notify_chan_t *nc = &g_notify[i];
        if (!nc->used) {
            nc->used = true;
            nc->server_gone = false;
            nc->gen++;
            nc->q_head = 0;
            nc->q_count = 0;
            c->notify_idx1 = i + 1;
            c->notify_gen = nc->gen;
            rc = EOK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static int fake_notify_wait(qmp_conn_t *conn, uint64_t timeout_ns, int64_t *value) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    *value = 0;
    if (c->notify_idx1 == 0) {
        return EINVAL;
    }
    struct timespec deadline;
    if (timeout_ns != 0) {
        abs_deadline(&deadline, timeout_ns);
    }
    (void)pthread_mutex_lock(&g_lock);
    notify_chan_t *nc = &g_notify[c->notify_idx1 - 1];
    int rc;
    for (;;) {
        if (!nc->used || nc->gen != c->notify_gen) {
            rc = ECANCELED; /* notify_close() ran */
            break;
        }
        if (nc->q_count > 0) { /* queued pulses are delivered before COIDDEATH */
            *value = nc->q[nc->q_head];
            nc->q_head = (nc->q_head + 1) % QMP_FAKE_NOTIFY_DEPTH;
            nc->q_count--;
            rc = EOK;
            break;
        }
        if (nc->server_gone) {
            rc = ESRCH;
            break;
        }
        if (timeout_ns != 0) {
            if (pthread_cond_timedwait(&nc->cv, &g_lock, &deadline) == ETIMEDOUT) {
                rc = ETIMEDOUT;
                break;
            }
        } else {
            (void)pthread_cond_wait(&nc->cv, &g_lock);
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static int fake_notify_close(qmp_conn_t *conn) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    if (c->notify_idx1 == 0) {
        return EOK;
    }
    (void)pthread_mutex_lock(&g_lock);
    notify_chan_t *nc = &g_notify[c->notify_idx1 - 1];
    if (nc->used && nc->gen == c->notify_gen) {
        nc->used = false;
        nc->gen++;
        (void)pthread_cond_broadcast(&nc->cv);
    }
    (void)pthread_mutex_unlock(&g_lock);
    c->notify_idx1 = 0;
    return EOK;
}

static int fake_event_register(qmp_conn_t *conn, uint8_t *blob, size_t cap, size_t *len) {
    conn_impl_t *c = (conn_impl_t *)(void *)conn;
    *len = 0;
    if (c->notify_idx1 == 0 || cap < sizeof(fake_event_blob_t)) {
        return EINVAL;
    }
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = c->ep_index >= 0 ? &g_eps[c->ep_index] : NULL;
    int rc;
    if (ep == NULL || !ep->used || ep->shutdown || ep->gen != c->ep_gen) {
        rc = ESRCH; /* registration is bound to a live server connection */
    } else {
        rc = EAGAIN;
        for (int i = 0; i < QMP_FAKE_MAX_REGS; i++) {
            fake_reg_t *r = &g_regs[i];
            if (r->used) {
                continue;
            }
            if (++g_reg_seq == 0) {
                ++g_reg_seq;
            }
            r->used = true;
            r->notify_idx = c->notify_idx1 - 1;
            r->notify_gen = c->notify_gen;
            r->ep_index = c->ep_index;
            r->ep_gen = c->ep_gen;
            r->scoid = c->scoid;
            r->reg_id = g_reg_seq;
            fake_event_blob_t b = {QMP_FAKE_EVENT_MAGIC, r->reg_id, i, 0};
            memcpy(blob, &b, sizeof(b));
            *len = sizeof(b);
            rc = EOK;
            break;
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static int fake_event_unregister(qmp_conn_t *conn, const uint8_t *blob, size_t len) {
    (void)conn;
    fake_event_blob_t b;
    if (len != sizeof(b)) {
        return EINVAL;
    }
    memcpy(&b, blob, sizeof(b));
    if (b.magic != QMP_FAKE_EVENT_MAGIC || b.reg_idx < 0 || b.reg_idx >= QMP_FAKE_MAX_REGS) {
        return EINVAL;
    }
    (void)pthread_mutex_lock(&g_lock);
    fake_reg_t *r = &g_regs[b.reg_idx];
    int rc = ESRCH;
    if (r->used && r->reg_id == b.reg_id) {
        memset(r, 0, sizeof(*r));
        rc = EOK;
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

/* Model of MsgDeliverEvent: non-blocking; refuses an event that was not registered
 * against THIS endpoint (EACCES, V-QNX-042); reports a gone subscriber with ESRCH. A
 * full pulse queue silently drops the pulse (V-QNX-044) — never an error, never a block. */
static int fake_deliver(qmp_endpoint_t *ep_, int64_t rcvid, const uint8_t *blob, size_t len,
                        int64_t value) {
    (void)rcvid; /* the fake matches by registration, not by receive id */
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    fake_event_blob_t b;
    if (len != sizeof(b)) {
        return EINVAL;
    }
    memcpy(&b, blob, sizeof(b));
    if (b.magic != QMP_FAKE_EVENT_MAGIC || b.reg_idx < 0 || b.reg_idx >= QMP_FAKE_MAX_REGS) {
        return EACCES; /* never registered */
    }
    (void)pthread_mutex_lock(&g_lock);
    fake_reg_t *r = &g_regs[b.reg_idx];
    int rc;
    if (!r->used || r->reg_id != b.reg_id) {
        rc = ESRCH; /* registration died with its connection */
    } else if (r->ep_index != e->ep_index || r->ep_gen != g_eps[e->ep_index].gen) {
        rc = EACCES; /* registered with a different server */
    } else {
        notify_chan_t *nc = &g_notify[r->notify_idx];
        if (!nc->used || nc->gen != r->notify_gen) {
            rc = ESRCH; /* subscriber's channel is gone */
        } else {
            if (nc->q_count < QMP_FAKE_NOTIFY_DEPTH) {
                nc->q[(nc->q_head + nc->q_count) % QMP_FAKE_NOTIFY_DEPTH] = value;
                nc->q_count++;
                (void)pthread_cond_broadcast(&nc->cv);
            }
            rc = EOK;
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}

static const qmp_transport_t g_fake = {
    "fake",
    fake_connect,
    fake_call,
    fake_disconnect,
    fake_listen,
    fake_receive,
    fake_reply,
    fake_error,
    fake_unblock,
    fake_close,
    fake_receive_pulse,
    fake_unblock_req,
    fake_notify_open,
    fake_notify_wait,
    fake_notify_close,
    fake_event_register,
    fake_event_unregister,
    fake_deliver,
};

const qmp_transport_t *qmp_transport_fake(void) {
    return &g_fake;
}

/* Test-only: simulate a signal hitting a REPLY-blocked client (kernel unblock pulse
 * before the client's own deadline). Picks the first RECEIVED slot without a pending
 * unblock; the client stays blocked until the server replies or errors, like the kernel. */
int qmp_transport_fake_unblock_inject(const char *path) {
    (void)pthread_mutex_lock(&g_lock);
    init_locked();
    endpoint_t *ep = find_locked(path);
    int rc = ESRCH;
    if (ep != NULL && !ep->shutdown) {
        for (int i = 0; i < QMP_FAKE_MAX_SLOTS; i++) {
            slot_t *sl = &ep->slots[i];
            if (sl->state == SLOT_RECEIVED && !sl->unblock_sent) {
                sl->unblock_sent = true;
                push_event(ep, QMP_RCV_UNBLOCK, sl->scoid, (int32_t)getpid(),
                           make_rcvid(i, sl->gen));
                rc = EOK;
                break;
            }
        }
    }
    (void)pthread_mutex_unlock(&g_lock);
    return rc;
}
