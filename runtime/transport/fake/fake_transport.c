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

#include <qmp/qmp_transport.h>

#define QMP_FAKE_MAX_ENDPOINTS 8
#define QMP_FAKE_MAX_SLOTS     16

enum slot_state {
    SLOT_FREE = 0,
    SLOT_PENDING,  /* client posted, waiting for a receiver   */
    SLOT_RECEIVED, /* a server thread took it                 */
    SLOT_DONE,     /* server replied / errored; client wakes  */
    SLOT_ABANDONED /* client timed out while RECEIVED         */
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
    int client_pid;
} slot_t;

typedef struct endpoint {
    bool used;
    bool shutdown;
    uint32_t gen; /* bumped on every listen(): detects stale connections */
    char path[QMP_NAME_MAX];
    slot_t slots[QMP_FAKE_MAX_SLOTS];
    pthread_cond_t cv_server; /* signalled when a slot becomes PENDING or on shutdown */
    pthread_cond_t cv_client; /* broadcast when any slot becomes DONE                 */
} endpoint_t;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static endpoint_t g_eps[QMP_FAKE_MAX_ENDPOINTS];
static bool g_inited;

static void init_locked(void) {
    if (g_inited) {
        return;
    }
    for (int i = 0; i < QMP_FAKE_MAX_ENDPOINTS; i++) {
        (void)pthread_cond_init(&g_eps[i].cv_server, NULL);
        (void)pthread_cond_init(&g_eps[i].cv_client, NULL);
    }
    g_inited = true;
}

typedef struct conn_impl {
    int ep_index;
    uint32_t ep_gen;
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
        c->ep_index = (int)(ep - g_eps);
        c->ep_gen = ep->gen;
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
    sl->client_pid = 0;
    (void)pthread_cond_signal(&ep->cv_server);

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
        int w = timeout_ns != 0 ? pthread_cond_timedwait(&ep->cv_client, &g_lock, &deadline)
                                : pthread_cond_wait(&ep->cv_client, &g_lock);
        if (w == ETIMEDOUT && sl->state != SLOT_DONE) {
            rc = ETIMEDOUT;
            break;
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
    c->ep_index = -1;
    return EOK;
}

/* ---- server ------------------------------------------------------------- */
static int fake_listen(qmp_endpoint_t *ep_, const char *path) {
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
    info->scoid = (int32_t)(sl - ep->slots) + 1;
    info->pid = 0;
    info->priority = 0;
    info->msglen = (uint32_t)copied;
    info->srcmsglen = (uint32_t)total;
    info->kind = QMP_RCV_MESSAGE;
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

static int fake_unblock(qmp_endpoint_t *ep_) {
    ep_impl_t *e = (ep_impl_t *)(void *)ep_;
    (void)pthread_mutex_lock(&g_lock);
    endpoint_t *ep = &g_eps[e->ep_index];
    ep->shutdown = true;
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
    (void)pthread_cond_broadcast(&ep->cv_server);
    (void)pthread_cond_broadcast(&ep->cv_client);
    (void)pthread_mutex_unlock(&g_lock);
    e->ep_index = -1;
    return EOK;
}

static const qmp_transport_t g_fake = {
    "fake",       fake_connect, fake_call,  fake_disconnect, fake_listen,
    fake_receive, fake_reply,   fake_error, fake_unblock,    fake_close,
};

const qmp_transport_t *qmp_transport_fake(void) {
    return &g_fake;
}

#if !defined(__QNX__)
const qmp_transport_t *qmp_transport_default(void) {
    return &g_fake; /* W3 replaces this with the uds transport on the host */
}
#endif
