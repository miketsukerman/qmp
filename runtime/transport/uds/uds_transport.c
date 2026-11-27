/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Unix-domain-socket transport (ADR-004): AF_UNIX / SOCK_SEQPACKET behind the same
 * internal transport interface as the native QNX and the in-process fake transports.
 *
 * BENCH AND HOST-TEST ONLY (QMP_TRANSPORT_UDS_BENCH_ONLY). It exists so the core can be
 * exercised across real process boundaries on the Linux host, and so the benchmarks can
 * compare "sockets" with native message passing on the same API, payloads and board.
 *
 * !!! NO PRIORITY INHERITANCE !!!
 * A socket carries bytes, not scheduling context. The server thread that picks up a
 * request keeps running at ITS OWN priority, whatever the priority of the client that is
 * waiting for the reply: a low-priority server working for a high-priority client can be
 * preempted indefinitely by unrelated medium-priority threads (unbounded priority
 * inversion). qmp_rcv_info_t.priority is therefore always 0, and
 * QMP_LISTEN_FIXED_PRIORITY is a no-op because there is nothing to switch off. Never use
 * this transport for a production service; that is what transport/qnx is for.
 *
 * Other deliberate differences from the native transport:
 *   - Timeouts are local. The client gives up on its own deadline, tells the server with
 *     a best-effort CANCEL frame and returns ETIMEDOUT at once; a late reply is discarded
 *     by its call id. (On QNX the kernel keeps the client REPLY-blocked until the server
 *     answers the unblock pulse.)
 *   - There is no receive_pulse(): CANCEL and disconnect are seen by whichever worker is
 *     idle in receive(), so cancellation is delayed while every worker is in a handler.
 *   - The scoid is per connection, not per client process.
 *   - No kernel-registered events and no secpol. An "event" is a record the server keeps
 *     for a second (notification) socket of the same client; deliver() sends one small
 *     frame on it without blocking and drops it when the socket buffer is full, which
 *     models a pulse that failed to queue. Correctness comes from the pull cursor.
 *   - Credentials: uid/gid are kernel-supplied (SO_PEERCRED / getpeereid). The pid is
 *     kernel-supplied on Linux and self-declared by the client elsewhere.
 *   - One message is one SEQPACKET record, so the largest message is bounded by the
 *     socket buffer size (EMSGSIZE beyond that).
 *
 * VERIFY(qnx8): V-QNX-047 — io-sock provides AF_UNIX/SOCK_SEQPACKET with the record,
 * peek, non-blocking and peer-death semantics used below (confirmed on Linux).
 *
 * Naming: the service path "qmp/<svc>-<inst>/v<major>" maps to the socket file
 * "<dir>/qmp.<svc>-<inst>.v<major>.sock", <dir> = $QMP_UDS_DIR or /tmp.
 *
 * Wire: every record starts with a 24-byte uds_frame_t, followed by the qmp message for
 * CALL and REPLY. Both ends are the same machine, so there is no byte-order handling.
 *
 * Server threading: leader/followers. One thread at a time (the leader) polls the
 * listening socket, the wake socket and every connection, reads ONE record into its own
 * work buffer, and hands leadership on before it returns the record to the core. The
 * connection table is allocated once in listen() (init time); there is no heap on the
 * call path.
 */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* struct ucred / SO_PEERCRED */ // NOLINT(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
#endif

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>

#include <qmp/qmp_transport.h>

#define UDS_MAX_CONNS    64 /* main + notification sockets of all clients       */
#define UDS_MAX_REGS     8  /* events per notification socket (client max subs) */
#define UDS_MAX_IOV      7  /* caller iov parts per record                      */
#define UDS_CANCEL_RING  4  /* remembered cancelled call ids per connection     */
#define UDS_ID_SLOT_BITS 6
#define UDS_FRAME_MAGIC  0x5155U       /* "QU" */
#define UDS_EVENT_MAGIC  0x51554556U   /* "QUEV" */
#define UDS_CTRL_TIMEOUT 2000000000ULL /* attach / event (un)register round trips */
#define UDS_DEFAULT_DIR  "/tmp"
#define UDS_DIR_ENV      "QMP_UDS_DIR"
QMP_STATIC_ASSERT((1 << UDS_ID_SLOT_BITS) == UDS_MAX_CONNS, "slot bits");

enum uds_kind {
    UDS_CALL = 1,          /* c->s main:   id, len, aux = reply capacity, value = pid */
    UDS_CANCEL = 2,        /* c->s main:   id of the call the client gave up on       */
    UDS_REPLY = 3,         /* s->c main:   id, len, aux = errno (0 = replied)         */
    UDS_NOTIFY_ATTACH = 4, /* c->s notify: first record on a notification socket      */
    UDS_NOTIFY_ACK = 5,    /* s->c notify: value = notify id                          */
    UDS_NOTIFY = 6,        /* s->c notify: value = advisory sequence number           */
    UDS_EVREG = 7,         /* c->s main:   value = notify id; REPLY value = reg id    */
    UDS_EVUNREG = 8,       /* c->s main:   value = notify id << 32 | reg id           */
};

typedef struct uds_frame {
    uint16_t magic;
    uint16_t kind; /* enum uds_kind */
    uint32_t id;
    uint32_t len; /* bytes following this frame as sent (CALL, REPLY) */
    int32_t aux;
    int64_t value;
} uds_frame_t;
QMP_STATIC_ASSERT(sizeof(uds_frame_t) == 24, "uds frame layout");

/* Registered-event blob carried inside SUBSCRIBE. */
typedef struct uds_event_blob {
    uint32_t magic;
    uint32_t notify_id;
    uint32_t reg_id;
    uint32_t _pad0;
} uds_event_blob_t;
QMP_STATIC_ASSERT(sizeof(uds_event_blob_t) <= QMP_EVENT_BLOB_MAX, "uds event blob");

/* ---- shared helpers --------------------------------------------------------- */

static uint64_t uds_mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
}

/* The peer is gone: one errno for "server/client died", like the native transport. */
static int uds_peer_err(int err) {
    if (err == EPIPE || err == ECONNRESET || err == ENOTCONN || err == EBADF) {
        return ESRCH;
    }
    return err != 0 ? err : EIO;
}

static int uds_sockaddr(const char *path, struct sockaddr_un *sa, socklen_t *len) {
    const char *dir = getenv(UDS_DIR_ENV);
    if (dir == NULL || dir[0] == '\0') {
        dir = UDS_DEFAULT_DIR;
    }
    memset(sa, 0, sizeof(*sa));
    sa->sun_family = AF_UNIX;
    int n = snprintf(sa->sun_path, sizeof(sa->sun_path), "%s/%s.sock", dir, path);
    if (n < 0 || (size_t)n >= sizeof(sa->sun_path)) {
        return ENAMETOOLONG;
    }
    for (char *p = sa->sun_path + strlen(dir) + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '.'; /* one flat file per service; no directories to create */
        }
    }
    *len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(sa->sun_path) + 1U);
    return EOK;
}

static void uds_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD);
    if (fl != -1) {
        (void)fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
    }
}

static void uds_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl != -1) {
        (void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

static int uds_socket(void) {
    int fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd == -1) {
        return -1;
    }
    uds_cloexec(fd);
    return fd;
}

/* One record = frame + iov parts. Records are atomic: no partial sends. */
static int uds_send(int fd, const uds_frame_t *f, const qmp_iov_t *iov, int parts, int flags) {
    struct iovec v[UDS_MAX_IOV + 1];
    if (parts < 0 || parts > UDS_MAX_IOV) {
        return EINVAL;
    }
    v[0].iov_base = (void *)(uintptr_t)f;
    v[0].iov_len = sizeof(*f);
    for (int i = 0; i < parts; i++) {
        v[i + 1].iov_base = iov[i].base;
        v[i + 1].iov_len = iov[i].len;
    }
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = v;
    m.msg_iovlen = (size_t)parts + 1U;
    for (;;) {
        if (sendmsg(fd, &m, flags | MSG_NOSIGNAL) != -1) {
            return EOK;
        }
        if (errno != EINTR) {
            return errno != 0 ? errno : EIO;
        }
    }
}

/* Returns the record bytes copied (0 = peer closed), or -1 with errno. *trunc is set
 * when the record was longer than the buffers. */
static ssize_t uds_recv(int fd, uds_frame_t *f, const qmp_iov_t *iov, int parts, int flags,
                        bool *trunc) {
    struct iovec v[UDS_MAX_IOV + 1];
    if (parts < 0 || parts > UDS_MAX_IOV) {
        errno = EINVAL;
        return -1;
    }
    v[0].iov_base = f;
    v[0].iov_len = sizeof(*f);
    for (int i = 0; i < parts; i++) {
        v[i + 1].iov_base = iov[i].base;
        v[i + 1].iov_len = iov[i].len;
    }
    struct msghdr m;
    memset(&m, 0, sizeof(m));
    m.msg_iov = v;
    m.msg_iovlen = (size_t)parts + 1U;
    for (;;) {
        ssize_t n = recvmsg(fd, &m, flags);
        if (n != -1) {
            *trunc = (m.msg_flags & MSG_TRUNC) != 0;
            return n;
        }
        if (errno != EINTR) {
            return -1;
        }
    }
}

static bool uds_again(int err) {
    return err == EAGAIN || err == EWOULDBLOCK;
}

/* Wait until fd is ready for `events` or the absolute monotonic deadline passes. */
static int uds_wait(int fd, short events, uint64_t deadline_ns) {
    for (;;) {
        uint64_t now = uds_mono_ns();
        if (now >= deadline_ns) {
            return ETIMEDOUT;
        }
        uint64_t ms = (deadline_ns - now + 999999ULL) / 1000000ULL;
        struct pollfd p = {fd, events, 0};
        int r = poll(&p, 1, ms > (uint64_t)INT_MAX ? INT_MAX : (int)ms);
        if (r > 0) {
            return EOK; /* readable/writable, or HUP/ERR: the I/O call reports it */
        }
        if (r == -1 && errno != EINTR) {
            return errno != 0 ? errno : EIO;
        }
    }
}

static size_t uds_iov_total(const qmp_iov_t *iov, int parts) {
    size_t n = 0;
    for (int i = 0; i < parts; i++) {
        n += iov[i].len;
    }
    return n;
}

/* ---- client ----------------------------------------------------------------- */

typedef struct uds_conn {
    int fd1;             /* main socket + 1; 0 = not connected                    */
    int nfd1;            /* notification socket + 1; 0 = none                     */
    uint32_t call_id;    /* id of the newest call; never 0 on the wire            */
    uint32_t notify_id;  /* server's id for the notification socket               */
    uint8_t notify_want; /* notify_open() ran: re-attach after a reconnect        */
    uint8_t _pad0[3];
    int32_t pid;
    char path[QMP_NAME_MAX]; /* service path, to open the notification socket    */
} uds_conn_t;
QMP_STATIC_ASSERT(sizeof(uds_conn_t) <= sizeof(qmp_conn_t), "conn storage");

static int uds_open(const char *path, int *out_fd) {
    struct sockaddr_un sa;
    socklen_t salen = 0;
    int rc = uds_sockaddr(path, &sa, &salen);
    if (rc != EOK) {
        return rc;
    }
    int fd = uds_socket();
    if (fd == -1) {
        return errno != 0 ? errno : EIO;
    }
    for (;;) {
        if (connect(fd, (const struct sockaddr *)(const void *)&sa, salen) == 0) {
            *out_fd = fd;
            return EOK;
        }
        if (errno != EINTR) {
            break;
        }
    }
    int err = errno;
    (void)close(fd);
    if (err == ECONNREFUSED) {
        return ENOENT; /* a stale socket file without a listener: "no such service" */
    }
    return err != 0 ? err : EIO;
}

static int uds_connect(qmp_conn_t *conn, const char *path) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    if (strlen(path) >= sizeof(c->path)) {
        return ENAMETOOLONG;
    }
    int fd = -1;
    int rc = uds_open(path, &fd);
    if (rc != EOK) {
        return rc;
    }
    c->fd1 = fd + 1;
    c->pid = (int32_t)getpid();
    memcpy(c->path, path, strlen(path) + 1U);
    return EOK;
}

/* One request/reply exchange on the main socket. On EOK *rep is the matching REPLY
 * frame and the reply bytes are in riov. */
static int uds_roundtrip(uds_conn_t *c, uds_frame_t *req, const qmp_iov_t *siov, int sparts,
                         const qmp_iov_t *riov, int rparts, uint64_t timeout_ns, uds_frame_t *rep) {
    if (c->fd1 <= 0) {
        return EBADF;
    }
    int fd = c->fd1 - 1;
    if (++c->call_id == 0) {
        ++c->call_id;
    }
    req->magic = UDS_FRAME_MAGIC;
    req->id = c->call_id;
    uint64_t deadline = timeout_ns != 0 ? uds_mono_ns() + timeout_ns : 0;
    for (;;) {
        int rc = uds_send(fd, req, siov, sparts, deadline != 0 ? MSG_DONTWAIT : 0);
        if (rc == EOK) {
            break;
        }
        if (!uds_again(rc) || deadline == 0) {
            return uds_peer_err(rc);
        }
        rc = uds_wait(fd, POLLOUT, deadline); /* server is not reading: "SEND-blocked" */
        if (rc != EOK) {
            return rc; /* nothing was sent, nothing to cancel */
        }
    }
    for (;;) {
        if (deadline != 0) {
            int rc = uds_wait(fd, POLLIN, deadline);
            if (rc == ETIMEDOUT) {
                /* Local timeout: tell the server (best effort) so it can cancel a parked
                 * or running request, then give up. A late reply is skipped by its id. */
                uds_frame_t cf = {UDS_FRAME_MAGIC, UDS_CANCEL, req->id, 0, 0, 0};
                (void)uds_send(fd, &cf, NULL, 0, MSG_DONTWAIT);
                return ETIMEDOUT;
            }
            if (rc != EOK) {
                return rc;
            }
        }
        bool trunc = false;
        ssize_t n = uds_recv(fd, rep, riov, rparts, deadline != 0 ? MSG_DONTWAIT : 0, &trunc);
        if (n == -1) {
            if (uds_again(errno)) {
                continue;
            }
            return uds_peer_err(errno);
        }
        if (n == 0) {
            return ESRCH; /* server closed the connection or died */
        }
        if ((size_t)n < sizeof(*rep) || rep->magic != UDS_FRAME_MAGIC) {
            return EPROTO;
        }
        if (rep->kind == UDS_REPLY && rep->id == req->id) {
            return EOK;
        }
        /* Stale reply to a call this client already timed out on: drop, keep waiting. */
    }
}

static int uds_call(qmp_conn_t *conn, const qmp_iov_t *siov, int sparts, const qmp_iov_t *riov,
                    int rparts, uint64_t timeout_ns, size_t *reply_len) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    *reply_len = 0;
    size_t slen = uds_iov_total(siov, sparts);
    size_t rcap = uds_iov_total(riov, rparts);
    if (slen > UINT32_MAX) {
        return EMSGSIZE;
    }
    uds_frame_t req = {
        0,     UDS_CALL, 0, (uint32_t)slen, rcap > (size_t)INT32_MAX ? INT32_MAX : (int32_t)rcap,
        c->pid};
    uds_frame_t rep;
    memset(&rep, 0, sizeof(rep));
    int rc = uds_roundtrip(c, &req, siov, sparts, riov, rparts, timeout_ns, &rep);
    if (rc != EOK) {
        return rc;
    }
    if (rep.aux != 0) {
        return rep.aux; /* the server's error(): errno, same machine */
    }
    /* Bytes the server sent; more than the reply buffers hold means it was cut, which
     * the core reports as EMSGSIZE. */
    *reply_len = rep.len;
    return EOK;
}

static void uds_notify_drop(uds_conn_t *c) {
    if (c->nfd1 > 0) {
        (void)shutdown(c->nfd1 - 1, SHUT_RDWR);
        (void)close(c->nfd1 - 1);
        c->nfd1 = 0;
    }
    c->notify_id = 0;
}

static int uds_disconnect(qmp_conn_t *conn) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    /* The notification socket belongs to this server session: it goes with the main
     * socket and is re-attached by the next event_register() after a reconnect. */
    uds_notify_drop(c);
    if (c->fd1 > 0) {
        (void)close(c->fd1 - 1);
        c->fd1 = 0;
    }
    return EOK;
}

/* Open the notification socket and learn the server's id for it. */
static int uds_notify_attach(uds_conn_t *c) {
    if (c->nfd1 > 0) {
        return EOK;
    }
    if (c->fd1 <= 0) {
        return ESRCH;
    }
    int fd = -1;
    int rc = uds_open(c->path, &fd);
    if (rc != EOK) {
        return rc == ENOENT ? ESRCH : rc;
    }
    uint64_t deadline = uds_mono_ns() + UDS_CTRL_TIMEOUT;
    uds_frame_t f = {UDS_FRAME_MAGIC, UDS_NOTIFY_ATTACH, 0, 0, 0, c->pid};
    rc = uds_send(fd, &f, NULL, 0, 0);
    if (rc == EOK) {
        rc = uds_wait(fd, POLLIN, deadline);
    }
    if (rc == EOK) {
        bool trunc = false;
        ssize_t n = uds_recv(fd, &f, NULL, 0, MSG_DONTWAIT, &trunc);
        if (n == 0) {
            rc = ESRCH;
        } else if (n == -1) {
            rc = uds_peer_err(errno);
        } else if ((size_t)n < sizeof(f) || f.magic != UDS_FRAME_MAGIC ||
                   f.kind != UDS_NOTIFY_ACK || f.value <= 0) {
            rc = EPROTO;
        }
    }
    if (rc != EOK) {
        (void)close(fd);
        return uds_peer_err(rc);
    }
    c->nfd1 = fd + 1;
    c->notify_id = (uint32_t)f.value;
    return EOK;
}

static int uds_notify_open(qmp_conn_t *conn) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    c->notify_want = 1;
    return uds_notify_attach(c); /* idempotent */
}

static int uds_notify_wait(qmp_conn_t *conn, uint64_t timeout_ns, int64_t *value) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    *value = 0;
    if (c->notify_want == 0) {
        return EINVAL;
    }
    if (c->nfd1 <= 0) {
        return ESRCH; /* the session died and nothing has re-registered yet */
    }
    int fd = c->nfd1 - 1;
    uint64_t deadline = timeout_ns != 0 ? uds_mono_ns() + timeout_ns : 0;
    for (;;) {
        if (deadline != 0) {
            int rc = uds_wait(fd, POLLIN, deadline);
            if (rc != EOK) {
                return rc;
            }
        }
        uds_frame_t f;
        bool trunc = false;
        ssize_t n = uds_recv(fd, &f, NULL, 0, deadline != 0 ? MSG_DONTWAIT : 0, &trunc);
        if (c->notify_want == 0) {
            return ECANCELED; /* notify_close() ran */
        }
        if (n == -1) {
            if (uds_again(errno)) {
                continue;
            }
            return uds_peer_err(errno);
        }
        if (n == 0) {
            return ESRCH; /* the server is gone: the socket analogue of COIDDEATH */
        }
        if ((size_t)n >= sizeof(f) && f.magic == UDS_FRAME_MAGIC && f.kind == UDS_NOTIFY) {
            *value = f.value; /* advisory only */
            return EOK;
        }
    }
}

static int uds_notify_close(qmp_conn_t *conn) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    c->notify_want = 0;
    uds_notify_drop(c);
    return EOK;
}

/* The socket analogue of MsgRegisterEvent(): ask the connected server to bind our
 * notification socket to this connection and to hand out a registration id. Only that
 * server can ever notify us, because only it holds the other end of the socket. */
static int uds_event_register(qmp_conn_t *conn, uint8_t *blob, size_t cap, size_t *len) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    *len = 0;
    if (c->notify_want == 0 || cap < sizeof(uds_event_blob_t)) {
        return EINVAL;
    }
    int rc = uds_notify_attach(c); /* first use, or first use after a reconnect */
    if (rc != EOK) {
        return rc;
    }
    uds_frame_t req = {0, UDS_EVREG, 0, 0, 0, (int64_t)c->notify_id};
    uds_frame_t rep;
    memset(&rep, 0, sizeof(rep));
    rc = uds_roundtrip(c, &req, NULL, 0, NULL, 0, UDS_CTRL_TIMEOUT, &rep);
    if (rc != EOK) {
        return rc;
    }
    if (rep.aux != 0) {
        return rep.aux;
    }
    uds_event_blob_t b = {UDS_EVENT_MAGIC, c->notify_id, (uint32_t)rep.value, 0};
    memcpy(blob, &b, sizeof(b));
    *len = sizeof(b);
    return EOK;
}

static int uds_event_unregister(qmp_conn_t *conn, const uint8_t *blob, size_t len) {
    uds_conn_t *c = (uds_conn_t *)(void *)conn;
    uds_event_blob_t b;
    if (len != sizeof(b)) {
        return EINVAL;
    }
    memcpy(&b, blob, sizeof(b));
    if (b.magic != UDS_EVENT_MAGIC) {
        return EINVAL;
    }
    uds_frame_t req = {
        0, UDS_EVUNREG, 0, 0, 0, (int64_t)(((uint64_t)b.notify_id << 32) | (uint64_t)b.reg_id)};
    uds_frame_t rep;
    memset(&rep, 0, sizeof(rep));
    int rc = uds_roundtrip(c, &req, NULL, 0, NULL, 0, UDS_CTRL_TIMEOUT, &rep);
    return rc != EOK ? rc : rep.aux;
}

/* ---- server ----------------------------------------------------------------- */

enum uds_role {
    UDS_ROLE_NEW = 0, /* accepted, first record decides */
    UDS_ROLE_MAIN,
    UDS_ROLE_NOTIFY,
};

typedef struct uds_sconn {
    int fd;        /* -1 = free slot                                             */
    int32_t id;    /* scoid (main) / notify id; > 0, unique per accept           */
    int32_t owner; /* notify: scoid of the main connection it is bound to, or 0  */
    int32_t pid;
    int32_t uid;
    int32_t gid;
    uint32_t gen; /* survives slot reuse so ids are never handed out twice      */
    uint8_t role; /* enum uds_role */
    uint8_t cred_valid;
    uint8_t pid_valid; /* pid came from the kernel, not from the client          */
    uint8_t cancel_n;  /* valid entries in cancelled[]                           */
    uint8_t cancel_pos;
    uint8_t _pad0[3];
    uint32_t cancelled[UDS_CANCEL_RING];
    uint32_t regs[UDS_MAX_REGS]; /* notify: live registration ids, 0 = free       */
} uds_sconn_t;

/* Heap, allocated once in listen() and freed in close(). mu guards everything except
 * the leader's scratch (pfds, pmap), which only the current leader touches. Only the
 * leader accepts and closes connections while the server runs. */
typedef struct uds_server {
    pthread_mutex_t mu;
    pthread_cond_t cv; /* leadership became free, or stop */
    bool leader;
    bool stop;
    int lfd;
    int wake_r; /* readable (never drained) once stop is set: wakes poll() */
    int wake_w;
    int rr; /* round-robin start so one busy connection cannot starve others */
    uint32_t reg_seq;
    struct sockaddr_un addr;
    uds_sconn_t conns[UDS_MAX_CONNS];
    struct pollfd pfds[UDS_MAX_CONNS + 2];
    int pmap[UDS_MAX_CONNS + 2]; /* pfds index -> conns index */
} uds_server_t;

typedef struct uds_ep {
    uds_server_t *srv;
} uds_ep_t;
QMP_STATIC_ASSERT(sizeof(uds_ep_t) <= sizeof(qmp_endpoint_t), "endpoint storage");

static uds_server_t *uds_srv(qmp_endpoint_t *ep) {
    return ((uds_ep_t *)(void *)ep)->srv;
}

/* mu held. */
static uds_sconn_t *uds_find(uds_server_t *s, int32_t id) {
    if (id <= 0) {
        return NULL;
    }
    uds_sconn_t *c = &s->conns[(uint32_t)id & (UDS_MAX_CONNS - 1U)];
    return (c->fd >= 0 && c->id == id) ? c : NULL;
}

static int64_t uds_rcvid(int32_t scoid, uint32_t call_id) {
    return (int64_t)(((uint64_t)(uint32_t)scoid << 32) | (uint64_t)call_id);
}

static void uds_fill_creds(uds_sconn_t *c) {
#if defined(__linux__)
    struct ucred uc;
    socklen_t ul = sizeof(uc);
    if (getsockopt(c->fd, SOL_SOCKET, SO_PEERCRED, &uc, &ul) == 0) {
        c->pid = (int32_t)uc.pid;
        c->uid = (int32_t)uc.uid;
        c->gid = (int32_t)uc.gid;
        c->cred_valid = 1;
        c->pid_valid = 1;
    }
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (getpeereid(c->fd, &uid, &gid) == 0) {
        c->uid = (int32_t)uid;
        c->gid = (int32_t)gid;
        c->cred_valid = 1; /* the pid stays self-declared (CALL frame) */
    }
#endif
}

/* mu held. Accept every pending connection; a full table refuses by closing. */
static void uds_accept_all(uds_server_t *s) {
    for (;;) {
        int fd = accept(s->lfd, NULL, NULL);
        if (fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        uds_cloexec(fd);
        uds_sconn_t *c = NULL;
        for (int i = 0; i < UDS_MAX_CONNS; i++) {
            if (s->conns[i].fd < 0) {
                c = &s->conns[i];
                break;
            }
        }
        if (c == NULL) {
            (void)close(fd);
            continue;
        }
        uint32_t gen = c->gen + 1U;
        if ((gen & 0x1FFFFFFU) == 0) {
            gen++;
        }
        memset(c, 0, sizeof(*c));
        c->gen = gen;
        c->fd = fd;
        c->id = (int32_t)(((gen & 0x1FFFFFFU) << UDS_ID_SLOT_BITS) | (uint32_t)(c - s->conns));
        c->uid = -1;
        c->gid = -1;
        uds_fill_creds(c);
    }
}

/* mu held. */
static void uds_close_conn(uds_server_t *s, uds_sconn_t *c) {
    if (c->fd < 0) {
        return;
    }
    (void)close(c->fd);
    c->fd = -1;
    if (c->role == UDS_ROLE_MAIN) {
        /* Registrations die with the registering connection (kernel-cleanup model). */
        for (int i = 0; i < UDS_MAX_CONNS; i++) {
            uds_sconn_t *n = &s->conns[i];
            if (n->fd >= 0 && n->role == UDS_ROLE_NOTIFY && n->owner == c->id) {
                (void)close(n->fd);
                n->fd = -1;
            }
        }
    }
}

/* mu held. Never blocks: the peer's buffer is empty unless it stopped reading. */
static int uds_send_reply(uds_sconn_t *c, uint32_t call_id, int err, int64_t value,
                          const qmp_iov_t *iov, int parts) {
    size_t total = uds_iov_total(iov, parts);
    if (total > UINT32_MAX) {
        return EMSGSIZE;
    }
    uds_frame_t f = {UDS_FRAME_MAGIC, UDS_REPLY, call_id, (uint32_t)total, err, value};
    int rc = uds_send(c->fd, &f, iov, parts, MSG_DONTWAIT);
    return rc == EOK ? EOK : uds_peer_err(rc);
}

/* mu held. Bind the notification socket to this main connection and record one event. */
static int uds_do_evreg(uds_server_t *s, uds_sconn_t *c, int64_t value, int64_t *reg_out) {
    uds_sconn_t *n = uds_find(s, (int32_t)value);
    if (n == NULL || n->role != UDS_ROLE_NOTIFY) {
        return ESRCH;
    }
    if (n->owner != 0 && n->owner != c->id) {
        return EACCES; /* someone else's notification socket */
    }
    for (int i = 0; i < UDS_MAX_REGS; i++) {
        if (n->regs[i] == 0) {
            if (++s->reg_seq == 0) {
                ++s->reg_seq;
            }
            n->owner = c->id;
            n->regs[i] = s->reg_seq;
            *reg_out = (int64_t)s->reg_seq;
            return EOK;
        }
    }
    return EAGAIN;
}

/* mu held. */
static int uds_do_evunreg(uds_server_t *s, const uds_sconn_t *c, int64_t value) {
    uint32_t reg_id = (uint32_t)((uint64_t)value & 0xFFFFFFFFU);
    uds_sconn_t *n = uds_find(s, (int32_t)((uint64_t)value >> 32));
    if (n == NULL || n->role != UDS_ROLE_NOTIFY || n->owner != c->id || reg_id == 0) {
        return ESRCH;
    }
    for (int i = 0; i < UDS_MAX_REGS; i++) {
        if (n->regs[i] == reg_id) {
            n->regs[i] = 0;
            return EOK;
        }
    }
    return ESRCH;
}

/* mu held. The connection is over: close it and, for a main connection, report the
 * disconnect to the core (EOK); otherwise nothing to report (EAGAIN). */
static int uds_drop(uds_server_t *s, uds_sconn_t *c, qmp_rcv_info_t *info) {
    uint8_t role = c->role;
    int32_t scoid = c->id;
    int32_t pid = c->pid;
    uds_close_conn(s, c);
    if (role != UDS_ROLE_MAIN) {
        return EAGAIN;
    }
    info->kind = QMP_RCV_DISCONNECT;
    info->scoid = scoid;
    info->pid = pid;
    return EOK;
}

/* mu held. Handle the one record the leader read from c. Returns EOK with *info filled
 * when the core must see it, or EAGAIN when it was consumed inside the transport. */
static int uds_on_record(uds_server_t *s, uds_sconn_t *c, const uds_frame_t *f, ssize_t n,
                         bool trunc, size_t cap, qmp_rcv_info_t *info) {
    if (n <= 0 || (size_t)n < sizeof(*f) || f->magic != UDS_FRAME_MAGIC) {
        return uds_drop(s, c, info); /* peer closed, died, or broke the framing */
    }
    size_t body = (size_t)n - sizeof(*f);
    switch (f->kind) {
    case UDS_CALL:
        if (c->role == UDS_ROLE_NOTIFY || (!trunc && body != f->len) || f->id == 0) {
            break;
        }
        c->role = UDS_ROLE_MAIN;
        if (c->pid_valid == 0) {
            c->pid = (int32_t)f->value; /* self-declared where the kernel has no pid */
        }
        info->kind = QMP_RCV_MESSAGE;
        info->rcvid = uds_rcvid(c->id, f->id);
        info->scoid = c->id;
        info->pid = c->pid;
        info->priority = 0; /* sockets carry no priority: nothing is inherited */
        info->msglen = (uint32_t)(body < cap ? body : cap);
        info->srcmsglen = f->len;
        info->dstmsglen = f->aux > 0 ? (uint32_t)f->aux : 0;
        info->cred_valid = c->cred_valid;
        info->uid = c->uid;
        info->gid = c->gid;
        return EOK;
    case UDS_CANCEL:
        if (c->role != UDS_ROLE_MAIN) {
            break;
        }
        c->cancelled[c->cancel_pos] = f->id;
        c->cancel_pos = (uint8_t)((c->cancel_pos + 1U) % UDS_CANCEL_RING);
        if (c->cancel_n < UDS_CANCEL_RING) {
            c->cancel_n++;
        }
        info->kind = QMP_RCV_UNBLOCK;
        info->rcvid = uds_rcvid(c->id, f->id);
        info->scoid = c->id;
        info->pid = c->pid;
        return EOK;
    case UDS_EVREG: {
        if (c->role == UDS_ROLE_NOTIFY) {
            break;
        }
        c->role = UDS_ROLE_MAIN;
        int64_t reg = 0;
        int err = uds_do_evreg(s, c, f->value, &reg);
        (void)uds_send_reply(c, f->id, err, reg, NULL, 0);
        return EAGAIN;
    }
    case UDS_EVUNREG:
        if (c->role != UDS_ROLE_MAIN) {
            break;
        }
        (void)uds_send_reply(c, f->id, uds_do_evunreg(s, c, f->value), 0, NULL, 0);
        return EAGAIN;
    case UDS_NOTIFY_ATTACH: {
        if (c->role != UDS_ROLE_NEW) {
            break;
        }
        c->role = UDS_ROLE_NOTIFY;
        uds_frame_t ack = {UDS_FRAME_MAGIC, UDS_NOTIFY_ACK, 0, 0, 0, (int64_t)c->id};
        (void)uds_send(c->fd, &ack, NULL, 0, MSG_DONTWAIT);
        return EAGAIN;
    }
    default:
        break;
    }
    return uds_drop(s, c, info); /* protocol violation: same as a dead peer */
}

static int uds_receive(qmp_endpoint_t *ep_, void *buf, size_t cap, qmp_rcv_info_t *info) {
    uds_server_t *s = uds_srv(ep_);
    (void)pthread_mutex_lock(&s->mu);
    for (;;) {
        while (s->leader && !s->stop) {
            (void)pthread_cond_wait(&s->cv, &s->mu);
        }
        if (s->stop) {
            (void)pthread_mutex_unlock(&s->mu);
            return ECANCELED;
        }
        s->leader = true;
        /* Poll set: wake socket, listener, then every connection starting at rr. */
        nfds_t np = 0;
        s->pfds[np].fd = s->wake_r;
        s->pfds[np].events = POLLIN;
        s->pmap[np++] = -1;
        s->pfds[np].fd = s->lfd;
        s->pfds[np].events = POLLIN;
        s->pmap[np++] = -1;
        for (int k = 0; k < UDS_MAX_CONNS; k++) {
            int i = (s->rr + k) % UDS_MAX_CONNS;
            if (s->conns[i].fd >= 0) {
                s->pfds[np].fd = s->conns[i].fd;
                s->pfds[np].events = POLLIN;
                s->pmap[np++] = i;
            }
        }
        (void)pthread_mutex_unlock(&s->mu);

        int pr = poll(s->pfds, np, -1);
        int ready = -1; /* first ready connection, in round-robin order */
        if (pr > 0 && s->pfds[0].revents == 0) {
            for (nfds_t k = 2; k < np; k++) {
                if (s->pfds[k].revents != 0) {
                    ready = s->pmap[k];
                    break;
                }
            }
        }
        /* Only the leader closes connections, so the fd is ours to read unlocked; the
         * record lands straight in this worker's buffer. */
        uds_frame_t f;
        memset(&f, 0, sizeof(f));
        bool trunc = false;
        ssize_t n = -1;
        bool have = false;
        if (ready >= 0) {
            qmp_iov_t iov = {buf, cap};
            n = uds_recv(s->conns[ready].fd, &f, &iov, 1, MSG_DONTWAIT, &trunc);
            /* Another reader cannot exist, but HUP without data reports EAGAIN on some
             * stacks: treat only "would block" as "nothing happened". */
            have = !(n == -1 && uds_again(errno));
        }

        (void)pthread_mutex_lock(&s->mu);
        int rc = EAGAIN;
        if (pr > 0 && s->pfds[0].revents == 0) {
            if (s->pfds[1].revents != 0) {
                uds_accept_all(s);
            }
            if (have) {
                s->rr = (ready + 1) % UDS_MAX_CONNS;
                memset(info, 0, sizeof(*info));
                info->uid = -1;
                info->gid = -1;
                rc = uds_on_record(s, &s->conns[ready], &f, n, trunc, cap, info);
            }
        }
        s->leader = false;
        (void)pthread_cond_signal(&s->cv);
        if (rc == EOK) {
            (void)pthread_mutex_unlock(&s->mu);
            return EOK;
        }
    }
}

static int uds_reply(qmp_endpoint_t *ep_, int64_t rcvid, const qmp_iov_t *iov, int parts) {
    uds_server_t *s = uds_srv(ep_);
    (void)pthread_mutex_lock(&s->mu);
    uds_sconn_t *c = uds_find(s, (int32_t)((uint64_t)rcvid >> 32));
    int rc = c != NULL
                 ? uds_send_reply(c, (uint32_t)((uint64_t)rcvid & 0xFFFFFFFFU), 0, 0, iov, parts)
                 : ESRCH;
    (void)pthread_mutex_unlock(&s->mu);
    return rc;
}

static int uds_error(qmp_endpoint_t *ep_, int64_t rcvid, int err) {
    uds_server_t *s = uds_srv(ep_);
    (void)pthread_mutex_lock(&s->mu);
    uds_sconn_t *c = uds_find(s, (int32_t)((uint64_t)rcvid >> 32));
    int rc = c != NULL ? uds_send_reply(c, (uint32_t)((uint64_t)rcvid & 0xFFFFFFFFU),
                                        err != 0 ? err : EIO, 0, NULL, 0)
                       : ESRCH;
    (void)pthread_mutex_unlock(&s->mu);
    return rc;
}

/* 1 when the client of rcvid has given up on it: its CANCEL was read already, or is the
 * next unread record on the connection (the client timed out before a worker even picked
 * the call up, the socket analogue of an unblock request on a just-received message). */
static int uds_unblock_req(qmp_endpoint_t *ep_, int64_t rcvid) {
    uds_server_t *s = uds_srv(ep_);
    uint32_t call_id = (uint32_t)((uint64_t)rcvid & 0xFFFFFFFFU);
    int req = 0;
    (void)pthread_mutex_lock(&s->mu);
    uds_sconn_t *c = uds_find(s, (int32_t)((uint64_t)rcvid >> 32));
    if (c != NULL) {
        for (unsigned i = 0; i < c->cancel_n; i++) {
            if (c->cancelled[i] == call_id) {
                req = 1;
            }
        }
        if (req == 0) {
            uds_frame_t f;
            /* MSG_DONTWAIT: a peek, never a wait, so holding mu across it is fine. */
            // NOLINTNEXTLINE(clang-analyzer-unix.BlockInCriticalSection)
            ssize_t n = recv(c->fd, &f, sizeof(f), MSG_PEEK | MSG_DONTWAIT);
            req = n >= (ssize_t)sizeof(f) && f.magic == UDS_FRAME_MAGIC && f.kind == UDS_CANCEL &&
                  f.id == call_id;
        }
    }
    (void)pthread_mutex_unlock(&s->mu);
    return req;
}

static int uds_unblock(qmp_endpoint_t *ep_) {
    uds_server_t *s = uds_srv(ep_);
    (void)pthread_mutex_lock(&s->mu);
    if (!s->stop) {
        s->stop = true;
        char b = 1;
        (void)send(s->wake_w, &b, 1, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    (void)pthread_cond_broadcast(&s->cv);
    (void)pthread_mutex_unlock(&s->mu);
    return EOK;
}

static void uds_destroy(uds_server_t *s, bool unlink_path) {
    for (int i = 0; i < UDS_MAX_CONNS; i++) {
        if (s->conns[i].fd >= 0) {
            (void)close(s->conns[i].fd);
        }
    }
    if (s->lfd >= 0) {
        (void)close(s->lfd);
    }
    if (unlink_path) {
        (void)unlink(s->addr.sun_path);
    }
    if (s->wake_r >= 0) {
        (void)close(s->wake_r);
    }
    if (s->wake_w >= 0) {
        (void)close(s->wake_w);
    }
    (void)pthread_cond_destroy(&s->cv);
    (void)pthread_mutex_destroy(&s->mu);
    free(s);
}

static int uds_close(qmp_endpoint_t *ep_) {
    uds_ep_t *e = (uds_ep_t *)(void *)ep_;
    if (e->srv == NULL) {
        return EOK;
    }
    /* Closing every connection is what makes blocked clients see ESRCH. */
    uds_destroy(e->srv, true);
    e->srv = NULL;
    return EOK;
}

static int uds_listen(qmp_endpoint_t *ep_, const char *path, uint32_t flags) {
    (void)flags; /* QMP_LISTEN_FIXED_PRIORITY: sockets never inherit, nothing to disable */
    uds_ep_t *e = (uds_ep_t *)(void *)ep_;
    e->srv = NULL;
    uds_server_t *s = calloc(1, sizeof(*s)); /* init time only */
    if (s == NULL) {
        return ENOMEM;
    }
    s->lfd = -1;
    s->wake_r = -1;
    s->wake_w = -1;
    for (int i = 0; i < UDS_MAX_CONNS; i++) {
        s->conns[i].fd = -1;
    }
    (void)pthread_mutex_init(&s->mu, NULL);
    (void)pthread_cond_init(&s->cv, NULL);
    socklen_t salen = 0;
    bool bound = false;
    int err = uds_sockaddr(path, &s->addr, &salen);
    if (err != EOK) {
        goto fail;
    }
    s->lfd = uds_socket();
    if (s->lfd == -1) {
        err = errno != 0 ? errno : EIO;
        goto fail;
    }
    if (bind(s->lfd, (const struct sockaddr *)(const void *)&s->addr, salen) == -1) {
        if (errno != EADDRINUSE) {
            err = errno != 0 ? errno : EIO;
            goto fail;
        }
        /* The file exists. A live server answers connect(); a file left behind by a
         * crashed one refuses it and is reclaimed. */
        int probe = -1;
        if (uds_open(path, &probe) == EOK) {
            (void)close(probe);
            err = EADDRINUSE;
            goto fail;
        }
        (void)unlink(s->addr.sun_path);
        if (bind(s->lfd, (const struct sockaddr *)(const void *)&s->addr, salen) == -1) {
            err = errno != 0 ? errno : EIO;
            goto fail;
        }
    }
    bound = true;
    if (listen(s->lfd, 16) == -1) {
        err = errno != 0 ? errno : EIO;
        goto fail;
    }
    uds_nonblock(s->lfd);
    int sp[2];
    /* A socket pair rather than a pipe: no dependency on the QNX pipe manager. */
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) == -1) {
        err = errno != 0 ? errno : EIO;
        goto fail;
    }
    s->wake_r = sp[0];
    s->wake_w = sp[1];
    uds_cloexec(sp[0]);
    uds_cloexec(sp[1]);
    e->srv = s;
    return EOK;
fail:
    uds_destroy(s, bound);
    return err;
}

/* The socket analogue of MsgDeliverEvent(): one small frame on the subscriber's
 * notification socket. Never blocks; a full socket buffer drops the notification, like a
 * pulse that failed to queue. ESRCH when the subscriber or its registration is gone,
 * EACCES when the blob was not registered by the connection that subscribed. */
static int uds_deliver(qmp_endpoint_t *ep_, int64_t rcvid, const uint8_t *blob, size_t len,
                       int64_t value) {
    uds_server_t *s = uds_srv(ep_);
    uds_event_blob_t b;
    if (len != sizeof(b)) {
        return EINVAL;
    }
    memcpy(&b, blob, sizeof(b));
    if (b.magic != UDS_EVENT_MAGIC || b.reg_id == 0) {
        return EACCES; /* never registered */
    }
    int rc = ESRCH;
    (void)pthread_mutex_lock(&s->mu);
    const uds_sconn_t *c = uds_find(s, (int32_t)((uint64_t)rcvid >> 32));
    uds_sconn_t *n = uds_find(s, (int32_t)b.notify_id);
    if (c != NULL && n != NULL && n->role == UDS_ROLE_NOTIFY) {
        if (n->owner != c->id) {
            rc = EACCES; /* registered by a different connection */
        } else {
            for (int i = 0; i < UDS_MAX_REGS; i++) {
                if (n->regs[i] == b.reg_id) {
                    uds_frame_t f = {UDS_FRAME_MAGIC, UDS_NOTIFY, 0, 0, 0, value};
                    int src = uds_send(n->fd, &f, NULL, 0, MSG_DONTWAIT);
                    rc = (src == EOK || uds_again(src) || src == ENOBUFS) ? EOK : uds_peer_err(src);
                    break;
                }
            }
        }
    }
    (void)pthread_mutex_unlock(&s->mu);
    return rc;
}

static const qmp_transport_t g_uds = {
    "uds",
    uds_connect,
    uds_call,
    uds_disconnect,
    uds_listen,
    uds_receive,
    uds_reply,
    uds_error,
    uds_unblock,
    uds_close,
    NULL, /* receive_pulse: lifecycle events are served by idle workers (see top) */
    uds_unblock_req,
    uds_notify_open,
    uds_notify_wait,
    uds_notify_close,
    uds_event_register,
    uds_event_unregister,
    uds_deliver,
};

const qmp_transport_t *qmp_transport_uds(void) {
    return &g_uds;
}
