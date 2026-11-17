/* SPDX-License-Identifier: Apache-2.0 */
/* Client: connect + HELLO handshake, synchronous calls. No hidden threads, no heap. */
#include <string.h>
#include <time.h>

#include "internal.h"

static qmp_client_impl_t *impl(qmp_client_t *c) {
    return (qmp_client_impl_t *)(void *)c;
}

static const qmp_client_impl_t *cimpl(const qmp_client_t *c) {
    return (const qmp_client_impl_t *)(const void *)c;
}

static uint64_t mono_ns(void) {
    struct timespec ts;
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int qmp_client_init(qmp_client_t *c, const qmp_client_cfg_t *cfg) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    memset(ci, 0, sizeof(*ci));
    ci->tr = (cfg != NULL && cfg->transport != NULL) ? cfg->transport : qmp_transport_default();
    ci->default_timeout_ns = cfg != NULL ? cfg->default_timeout_ns : 0;
    ci->reconnect_initial_ms =
        (cfg != NULL && cfg->reconnect_initial_ms != 0) ? cfg->reconnect_initial_ms : 10U;
    ci->reconnect_max_ms =
        (cfg != NULL && cfg->reconnect_max_ms != 0) ? cfg->reconnect_max_ms : 1000U;
    ci->reconnect_attempts =
        (cfg != NULL && cfg->reconnect_attempts != 0) ? cfg->reconnect_attempts : 10U;
    return ci->tr != NULL ? EOK : EINVAL;
}

/* Round trip with header validation. rep_cap bounds the payload the caller can hold. */
static int do_call(qmp_client_impl_t *ci, qmp_hdr_t *req_hdr, const void *req, size_t req_len,
                   qmp_hdr_t *rep_hdr, void *rep, size_t rep_cap, uint64_t timeout_ns) {
    qmp_iov_t siov[2] = {{req_hdr, QMP_HDR_SIZE}, {(void *)(uintptr_t)req, req_len}};
    qmp_iov_t riov[2] = {{rep_hdr, QMP_HDR_SIZE}, {rep, rep_cap}};
    int sparts = req_len > 0 ? 2 : 1;
    int rparts = rep_cap > 0 ? 2 : 1;
    size_t reply_len = 0;
    memset(rep_hdr, 0, sizeof(*rep_hdr));
    uint64_t t0 = timeout_ns != 0 ? mono_ns() : 0;
    int rc = ci->tr->call(&ci->conn, siov, sparts, riov, rparts, timeout_ns, &reply_len);
    if (rc == ECANCELED && timeout_ns != 0 && mono_ns() - t0 >= timeout_ns) {
        /* The server cannot tell a timeout from a signal and answers a kernel unblock
         * with neutral ECANCELED (DESIGN §3.8). Only the client knows its own deadline:
         * report ETIMEDOUT exactly when it has passed, else keep ECANCELED (signal). */
        rc = ETIMEDOUT;
    }
    if (rc != EOK) {
        return rc;
    }
    if (reply_len == 0) {
        /* Transport did not report a length: trust payload_len (bounded by rep_cap below). */
        reply_len = QMP_HDR_SIZE + (size_t)rep_hdr->payload_len;
    }
    return qmp_hdr_validate_reply(rep_hdr, req_hdr, reply_len, rep_cap);
}

/* Resolve, connect and HELLO using the identity stored in the impl. */
static int connect_once(qmp_client_impl_t *ci) {
    char path[QMP_NAME_MAX];
    int rc = qmp_service_path(path, sizeof(path), ci->service_id, ci->instance_id, ci->ver_major);
    if (rc != EOK) {
        return rc;
    }
    rc = ci->tr->connect(&ci->conn, path);
    if (rc != EOK) {
        return rc;
    }
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hello_t hello = {ci->service_id, ci->instance_id, ci->ver_major, ci->ver_minor, {0, 0}};
    qmp_hello_reply_t hr;
    qmp_hdr_init(&req_hdr, ci->service_id, ci->instance_id, QMP_OP_HELLO, ci->ver_major,
                 ci->ver_minor, QMP_F_CONTROL, (uint32_t)sizeof(hello));
    rc = do_call(ci, &req_hdr, &hello, sizeof(hello), &rep_hdr, &hr, sizeof(hr),
                 ci->default_timeout_ns);
    if (rc == EOK && (rep_hdr.payload_len != sizeof(hr) || rep_hdr.status != 0)) {
        rc = EPROTO;
    }
    if (rc == EOK) {
        rc = qmp_version_compatible(hr.ver_major, hr.ver_minor, ci->ver_major, ci->ver_minor);
    }
    if (rc != EOK) {
        (void)ci->tr->disconnect(&ci->conn);
        return rc;
    }
    ci->server_minor = hr.ver_minor;
    ci->server_generation = hr.server_generation;
    ci->server_max_msg_size = hr.max_msg_size;
    ci->connected = 1;
    return EOK;
}

int qmp_client_connect(qmp_client_t *c, uint16_t service_id, uint16_t instance_id,
                       uint8_t ver_major, uint8_t ver_minor) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (ci->tr == NULL) {
        return EINVAL;
    }
    if (ci->connected) {
        return EISCONN;
    }
    ci->service_id = service_id;
    ci->instance_id = instance_id;
    ci->ver_major = ver_major;
    ci->ver_minor = ver_minor;
    int rc = connect_once(ci);
    if (rc == EOK) {
        ci->have_params = 1;
    }
    return rc;
}

/* A dead server makes the connection useless: drop it so calls fail fast with ENOTCONN
 * and qmp_client_reconnect() can run. Never retries the call itself (DESIGN §3.7). Only
 * reached in the connected state: EBADF (stale coid after the server died) is normalized
 * to ESRCH so callers see one "server gone" errno. Connect-path errors stay raw. */
static int mark_dead_on(int rc, qmp_client_impl_t *ci) {
    if (rc == ESRCH || rc == EBADF) {
        (void)ci->tr->disconnect(&ci->conn);
        ci->connected = 0;
        rc = ESRCH;
    }
    return rc;
}

/* ---- topics (ADR-003 notify-then-pull) ------------------------------------ */

/* Control round trip (QMP_F_CONTROL). Expects status 0 and exactly rep_cap bytes. */
static int control_call(qmp_client_impl_t *ci, uint16_t op, const void *req, size_t req_len,
                        void *rep, size_t rep_cap) {
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hdr_init(&req_hdr, ci->service_id, ci->instance_id, op, ci->ver_major, ci->ver_minor,
                 QMP_F_CONTROL, (uint32_t)req_len);
    int rc = do_call(ci, &req_hdr, req, req_len, &rep_hdr, rep, rep_cap, ci->default_timeout_ns);
    if (rc != EOK) {
        return rc;
    }
    return (rep_hdr.status == 0 && rep_hdr.payload_len == rep_cap) ? EOK : EPROTO;
}

/* Register a fresh event with the connected server and SUBSCRIBE with it. Fills the
 * sub's event blob, sub_id and cursor. */
static int resubscribe(qmp_client_impl_t *ci, qmp_sub_impl_t *sub) {
    qmp_subscribe_t req;
    memset(&req, 0, sizeof(req));
    req.topic_id = sub->topic_id;
    size_t blob_len = 0;
    int rc = ci->tr->event_register(&ci->conn, req.event, sizeof(req.event), &blob_len);
    if (rc != EOK) {
        return rc;
    }
    req.event_len = (uint16_t)blob_len;
    qmp_subscribe_reply_t rep;
    rc = control_call(ci, QMP_OP_SUBSCRIBE, &req, sizeof(req), &rep, sizeof(rep));
    if (rc != EOK) {
        (void)ci->tr->event_unregister(&ci->conn, req.event, blob_len);
        return rc;
    }
    memcpy(sub->event, req.event, sizeof(sub->event));
    sub->event_len = req.event_len;
    sub->sub_id = rep.sub_id;
    sub->last_seq = rep.seq; /* only NEW updates are delivered */
    return EOK;
}

/* Pull once: header + qmp_pull_reply_t + sample land in three iov parts. Returns EOK
 * with *pr filled (count == 0 means drained) and *sample_len set for count == 1. */
static int pull_once(qmp_client_impl_t *ci, qmp_sub_impl_t *sub, qmp_pull_reply_t *pr,
                     uint32_t *sample_len) {
    qmp_pull_t req;
    memset(&req, 0, sizeof(req));
    req.topic_id = sub->topic_id;
    req.after_seq = sub->last_seq;
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hdr_init(&req_hdr, ci->service_id, ci->instance_id, QMP_OP_PULL, ci->ver_major,
                 ci->ver_minor, QMP_F_CONTROL, (uint32_t)sizeof(req));
    qmp_iov_t siov[2] = {{&req_hdr, QMP_HDR_SIZE}, {&req, sizeof(req)}};
    qmp_iov_t riov[3] = {{&rep_hdr, QMP_HDR_SIZE}, {pr, sizeof(*pr)}, {sub->buf, sub->buf_cap}};
    size_t reply_len = 0;
    memset(&rep_hdr, 0, sizeof(rep_hdr));
    memset(pr, 0, sizeof(*pr));
    *sample_len = 0;
    int rc = ci->tr->call(&ci->conn, siov, 2, riov, 3, ci->default_timeout_ns, &reply_len);
    if (rc != EOK) {
        return rc;
    }
    if (reply_len == 0) {
        reply_len = QMP_HDR_SIZE + (size_t)rep_hdr.payload_len;
    }
    rc = qmp_hdr_validate_reply(&rep_hdr, &req_hdr, reply_len, sizeof(*pr) + sub->buf_cap);
    if (rc != EOK) {
        return rc;
    }
    if (rep_hdr.status != 0 || rep_hdr.payload_len < sizeof(*pr) || pr->count > 1 ||
        (pr->count == 0 && rep_hdr.payload_len != sizeof(*pr))) {
        return EPROTO;
    }
    *sample_len = rep_hdr.payload_len - (uint32_t)sizeof(*pr);
    return EOK;
}

/* Pull a subscription dry, running the callback per sample in sequence order. */
static int drain_sub(qmp_client_impl_t *ci, qmp_sub_impl_t *sub) {
    for (;;) {
        qmp_pull_reply_t pr;
        uint32_t slen = 0;
        int rc = pull_once(ci, sub, &pr, &slen);
        if (rc != EOK) {
            return rc;
        }
        if (pr.count == 0) {
            return EOK;
        }
        sub->last_seq = pr.seq;
        if (sub->cb != NULL) {
            sub->cb(sub->cb_ctx, sub->buf, slen, pr.seq, pr.dropped);
        }
        if (pr.remaining == 0) {
            return EOK;
        }
    }
}

int qmp_client_reconnect(qmp_client_t *c) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (ci->tr == NULL || !ci->have_params) {
        return ENOTCONN;
    }
    if (ci->connected) {
        return EISCONN;
    }
    uint32_t delay_ms = ci->reconnect_initial_ms;
    int rc = ENOTCONN;
    for (uint32_t attempt = 0; attempt < ci->reconnect_attempts; attempt++) {
        if (attempt != 0) {
            struct timespec ts = {(time_t)(delay_ms / 1000U), (long)(delay_ms % 1000U) * 1000000L};
            (void)nanosleep(&ts, NULL);
            delay_ms = delay_ms < ci->reconnect_max_ms / 2U ? delay_ms * 2U : ci->reconnect_max_ms;
        }
        rc = connect_once(ci);
        if (rc == EOK || rc == ENOTSUP) {
            break; /* connected, or a major-version mismatch retries can't fix */
        }
    }
    if (rc != EOK) {
        return rc;
    }
    /* Resubscribe active subscriptions on the new session. Old subscription ids and
     * event registrations died with the old connection: register fresh events (the
     * registration is bound to the old server coid) and send new SUBSCRIBEs. Sequence
     * numbers restart with the server; each cursor resets to the new current value. */
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        qmp_sub_impl_t *sub = ci->subs[i];
        if (sub == NULL) {
            continue;
        }
        int src = resubscribe(ci, sub);
        if (src != EOK) {
            sub->owner = NULL; /* dropped; the connection itself stays usable */
            ci->subs[i] = NULL;
            rc = src;
        }
    }
    return rc;
}

int qmp_client_call(qmp_client_t *c, uint16_t resource_id, const void *req, size_t req_len,
                    void *rep, size_t rep_cap, int32_t *status, const qmp_call_opts_t *opts) {
    if (c == NULL || status == NULL || (req == NULL && req_len != 0) ||
        (rep == NULL && rep_cap != 0) || req_len > QMP_MAX_PAYLOAD) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (!ci->connected) {
        return ENOTCONN;
    }
    uint64_t timeout_ns = ci->default_timeout_ns;
    uint16_t flags = 0;
    uint8_t up_priority = 0;
    if (opts != NULL) {
        if (opts->timeout_ns != 0) {
            timeout_ns = opts->timeout_ns;
        }
        up_priority = opts->up_priority;
    }
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hdr_init(&req_hdr, ci->service_id, ci->instance_id, resource_id, ci->ver_major,
                 ci->ver_minor, flags, (uint32_t)req_len);
    req_hdr.up_priority = up_priority;
    int rc = do_call(ci, &req_hdr, req, req_len, &rep_hdr, rep, rep_cap, timeout_ns);
    if (rc != EOK) {
        return mark_dead_on(rc, ci);
    }
    *status = rep_hdr.status;
    if (rep_hdr.status == 0 && rep_hdr.payload_len != rep_cap) {
        /* A successful reply must carry exactly the declared reply struct. */
        return EPROTO;
    }
    return EOK;
}

int qmp_client_oneway(qmp_client_t *c, uint16_t resource_id, const void *req, size_t req_len,
                      const qmp_call_opts_t *opts) {
    if (c == NULL || (req == NULL && req_len != 0) || req_len > QMP_MAX_PAYLOAD) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (!ci->connected) {
        return ENOTCONN;
    }
    uint64_t timeout_ns = ci->default_timeout_ns;
    uint8_t up_priority = 0;
    if (opts != NULL) {
        if (opts->timeout_ns != 0) {
            timeout_ns = opts->timeout_ns;
        }
        up_priority = opts->up_priority;
    }
    qmp_hdr_t req_hdr;
    qmp_hdr_t rep_hdr;
    qmp_hdr_init(&req_hdr, ci->service_id, ci->instance_id, resource_id, ci->ver_major,
                 ci->ver_minor, QMP_F_ONEWAY, (uint32_t)req_len);
    req_hdr.up_priority = up_priority;
    int rc = do_call(ci, &req_hdr, req, req_len, &rep_hdr, NULL, 0, timeout_ns);
    if (rc != EOK) {
        return mark_dead_on(rc, ci);
    }
    /* The receipt reply is header-only with status 0; anything else is a protocol bug. */
    return (rep_hdr.status == 0 && rep_hdr.payload_len == 0) ? EOK : EPROTO;
}

int qmp_client_subscribe(qmp_client_t *c, uint16_t topic_id, void *sample_buf, size_t sample_cap,
                         qmp_sub_cb_t cb, void *ctx, qmp_sub_t *sub_) {
    if (c == NULL || sub_ == NULL || sample_buf == NULL || sample_cap == 0 ||
        ((uintptr_t)sample_buf & 7U) != 0) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (!ci->connected) {
        return ENOTCONN;
    }
    if (ci->tr->notify_open == NULL || ci->tr->event_register == NULL) {
        return ENOSYS; /* transport has no notification support */
    }
    int slot = -1;
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        if (ci->subs[i] == NULL) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        return EAGAIN; /* bounded by design */
    }
    if (!ci->notify_open) {
        int rc = ci->tr->notify_open(&ci->conn);
        if (rc != EOK) {
            return rc;
        }
        ci->notify_open = 1;
    }
    qmp_sub_impl_t *sub = (qmp_sub_impl_t *)(void *)sub_;
    memset(sub, 0, sizeof(*sub));
    sub->buf = sample_buf;
    sub->buf_cap = sample_cap;
    sub->cb = cb;
    sub->cb_ctx = ctx;
    sub->topic_id = topic_id;
    int rc = resubscribe(ci, sub);
    if (rc != EOK) {
        return mark_dead_on(rc, ci);
    }
    sub->owner = ci;
    ci->subs[slot] = sub;
    return EOK;
}

int qmp_client_unsubscribe(qmp_client_t *c, qmp_sub_t *sub_) {
    if (c == NULL || sub_ == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    qmp_sub_impl_t *sub = (qmp_sub_impl_t *)(void *)sub_;
    if (sub->owner != ci) {
        return EOK; /* inactive (or already dropped by a failed resubscribe) */
    }
    int rc = EOK;
    if (ci->connected) {
        qmp_unsubscribe_t req;
        memset(&req, 0, sizeof(req));
        req.sub_id = sub->sub_id;
        rc = control_call(ci, QMP_OP_UNSUBSCRIBE, &req, sizeof(req), NULL, 0);
        if (rc != EOK) {
            rc = mark_dead_on(rc, ci);
        }
        if (ci->connected && ci->tr->event_unregister != NULL) {
            (void)ci->tr->event_unregister(&ci->conn, sub->event, sub->event_len);
        }
    }
    /* Local state is always released: a dead server cleans up on disconnect. */
    sub->owner = NULL;
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        if (ci->subs[i] == sub) {
            ci->subs[i] = NULL;
        }
    }
    return rc == ESRCH ? EOK : rc;
}

int qmp_client_poll(qmp_client_t *c, uint64_t timeout_ns) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    if (!ci->connected) {
        return ENOTCONN;
    }
    if (!ci->notify_open) {
        return EINVAL; /* no active subscription */
    }
    bool any = false;
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        any = any || ci->subs[i] != NULL;
    }
    if (!any) {
        return EINVAL; /* everything was unsubscribed */
    }
    int64_t value = 0;
    int rc = ci->tr->notify_wait(&ci->conn, timeout_ns, &value);
    if (rc != EOK) {
        return mark_dead_on(rc, ci);
    }
    /* The pulse (and its value) is only a hint: pulses can be coalesced or fail to queue
     * (V-QNX-044). Drain every subscription; the cursor is the source of truth. */
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        if (ci->subs[i] != NULL) {
            rc = drain_sub(ci, ci->subs[i]);
            if (rc != EOK) {
                return mark_dead_on(rc, ci);
            }
        }
    }
    return EOK;
}

int qmp_client_close(qmp_client_t *c) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    int rc = EOK;
    for (unsigned i = 0; i < QMP_CLIENT_MAX_SUBS; i++) {
        if (ci->subs[i] != NULL) {
            ci->subs[i]->owner = NULL;
            ci->subs[i] = NULL;
        }
    }
    if (ci->notify_open) {
        if (ci->tr->notify_close != NULL) {
            (void)ci->tr->notify_close(&ci->conn);
        }
        ci->notify_open = 0;
    }
    if (ci->connected) {
        rc = ci->tr->disconnect(&ci->conn);
        ci->connected = 0;
    }
    return rc;
}

int qmp_client_is_connected(const qmp_client_t *c) {
    return c != NULL && cimpl(c)->connected;
}

uint32_t qmp_client_server_generation(const qmp_client_t *c) {
    return c != NULL ? cimpl(c)->server_generation : 0;
}

uint8_t qmp_client_server_minor(const qmp_client_t *c) {
    return c != NULL ? cimpl(c)->server_minor : 0;
}
