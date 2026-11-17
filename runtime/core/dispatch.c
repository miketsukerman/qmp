/* SPDX-License-Identifier: Apache-2.0 */
/* Request dispatch (runtime-c rules, step 4): validate everything before touching payload. */
#include <string.h>

#include "internal.h"

static const qmp_method_desc_t *find_method(const qmp_service_desc_t *desc, uint16_t id) {
    /* Tables are small and sorted; a linear scan is cheaper than bsearch indirection. */
    for (size_t i = 0; i < desc->nmethods; i++) {
        if (desc->methods[i].id == id) {
            return &desc->methods[i];
        }
    }
    return NULL;
}

int qmp_dispatch_lookup(const qmp_service_desc_t *desc, uint16_t instance_id, const qmp_hdr_t *h,
                        const qmp_method_desc_t **out_m) {
    *out_m = NULL;
    if (h->service_id != desc->service_id || h->instance_id != instance_id) {
        return ENOENT;
    }
    if (h->ver_major != desc->ver_major) {
        return ENOTSUP;
    }
    if (h->resource_id >= 0x8000U) {
        return ENOENT; /* topics are not callable methods */
    }
    const qmp_method_desc_t *m = find_method(desc, h->resource_id);
    if (m == NULL) {
        return ENOENT;
    }
    if (h->payload_len != m->in_size) {
        return EINVAL;
    }
    if (m->handler == NULL) {
        return ENOSYS; /* declared but not implemented */
    }
    if (((h->flags & QMP_F_ONEWAY) != 0) != ((m->flags & QMP_MF_ONEWAY) != 0)) {
        return EINVAL; /* the one-way flag must match the method's declaration */
    }
    *out_m = m;
    return EOK;
}

static void send_error(qmp_server_impl_t *s, int64_t rcvid, int err) {
    int rc = s->tr->error(&s->ep, rcvid, err);
    if (rc != EOK && rc != ESRCH && rc != EBADF) {
        qmp_log("error(%lld, %d) failed: %d", (long long)rcvid, err, rc);
    }
}

static void send_reply(qmp_server_impl_t *s, int64_t rcvid, const qmp_hdr_t *hdr,
                       const void *payload, size_t payload_len) {
    qmp_iov_t iov[2] = {{(void *)(uintptr_t)hdr, QMP_HDR_SIZE},
                        {(void *)(uintptr_t)payload, payload_len}};
    int rc = s->tr->reply(&s->ep, rcvid, iov, payload_len > 0 ? 2 : 1);
    if (rc != EOK && rc != ESRCH && rc != EBADF) {
        qmp_log("reply(%lld) failed: %d", (long long)rcvid, rc);
    }
}

static void handle_control(qmp_server_impl_t *s, const qmp_rcv_info_t *info, const qmp_hdr_t *h,
                           const uint8_t *payload, uint8_t *rep) {
    if (h->resource_id != QMP_OP_HELLO) {
        send_error(s, info->rcvid, ENOSYS); /* SUBSCRIBE/UNSUBSCRIBE/PULL: W3 */
        return;
    }
    if (h->payload_len != sizeof(qmp_hello_t)) {
        send_error(s, info->rcvid, EINVAL);
        return;
    }
    qmp_hello_t hello;
    memcpy(&hello, payload, sizeof(hello));
    if (hello.service_id != s->desc->service_id || hello.instance_id != s->instance_id) {
        send_error(s, info->rcvid, ENOENT);
        return;
    }
    if (qmp_version_compatible(s->desc->ver_major, s->desc->ver_minor, hello.ver_major,
                               hello.ver_minor) != EOK) {
        send_error(s, info->rcvid, ENOTSUP);
        return;
    }
    qmp_hdr_t *rh = (qmp_hdr_t *)(void *)rep;
    qmp_hello_reply_t hr = {s->desc->ver_major, s->desc->ver_minor, {0, 0},
                            s->generation,      s->max_msg_size,    0};
    qmp_hdr_init_reply(rh, h, 0, (uint32_t)sizeof(hr));
    memcpy(rep + QMP_HDR_SIZE, &hr, sizeof(hr));
    send_reply(s, info->rcvid, rh, rep + QMP_HDR_SIZE, sizeof(hr));
}

/* ---- deferred-reply pool (slots live in caller-provided cfg storage) ----- */
static qmp_req_impl_t *pool_alloc(qmp_server_impl_t *s) {
    if (s->pool == NULL) {
        return NULL;
    }
    qmp_req_impl_t *r = NULL;
    (void)pthread_mutex_lock(&s->pool_mu);
    for (size_t i = 0; i < s->pool_count; i++) {
        if (s->pool[i].state == QMP_REQ_FREE) {
            s->pool[i].state = QMP_REQ_ACTIVE;
            r = &s->pool[i];
            break;
        }
    }
    (void)pthread_mutex_unlock(&s->pool_mu);
    return r;
}

static void pool_set_state(qmp_server_impl_t *s, qmp_req_impl_t *r, uint8_t state) {
    (void)pthread_mutex_lock(&s->pool_mu);
    r->state = state;
    (void)pthread_mutex_unlock(&s->pool_mu);
}

static void fill_req(qmp_req_impl_t *req, qmp_server_impl_t *s, const qmp_rcv_info_t *info,
                     const qmp_hdr_t *h, uint32_t out_size, uint8_t pooled, qmp_inflight_t *infl) {
    req->server = s;
    req->infl = infl;
    req->rcvid = info->rcvid;
    req->info.pid = info->pid;
    req->info.scoid = info->scoid;
    req->info.priority = info->priority;
    req->info.up_priority = h->up_priority;
    req->info.ver_minor = h->ver_minor;
    req->info._pad0[0] = 0;
    req->info._pad0[1] = 0;
    req->h = *h;
    req->out_size = out_size;
    req->pooled = pooled;
    req->_pad0[0] = 0;
    req->_pad0[1] = 0;
}

/* Record the worker's in-flight slot before the handler runs. pool_mu guards the slot. */
static void infl_record(qmp_server_impl_t *s, qmp_inflight_t *infl, const qmp_rcv_info_t *info) {
    (void)pthread_mutex_lock(&s->pool_mu);
    infl->rcvid = info->rcvid;
    infl->scoid = info->scoid;
    infl->state = QMP_INFL_RUNNING;
    (void)pthread_mutex_unlock(&s->pool_mu);
}

/* Clear the slot when the handler is done. Returns 1 when the request was cancelled
 * while it ran (error reply already sent, or the client is gone): discard the result. */
static int infl_clear(qmp_server_impl_t *s, qmp_inflight_t *infl) {
    (void)pthread_mutex_lock(&s->pool_mu);
    int cancelled = infl->state == QMP_INFL_CANCELED;
    infl->state = QMP_INFL_IDLE;
    infl->rcvid = 0;
    infl->scoid = 0;
    (void)pthread_mutex_unlock(&s->pool_mu);
    return cancelled;
}

void qmp_server_dispatch(qmp_server_impl_t *s, const qmp_rcv_info_t *info, uint8_t *rcv, size_t len,
                         uint8_t *rep, size_t rep_cap, qmp_inflight_t *infl) {
    if (info->srcmsglen > len) {
        send_error(s, info->rcvid, EMSGSIZE); /* client sent more than we can hold */
        return;
    }
    qmp_hdr_t h;
    if (len < QMP_HDR_SIZE) {
        send_error(s, info->rcvid, EINVAL);
        return;
    }
    memcpy(&h, rcv, sizeof(h));
    int rc = qmp_hdr_validate(&h, len);
    if (rc != EOK) {
        send_error(s, info->rcvid, rc);
        return;
    }
    if ((h.flags & QMP_F_CONTROL) != 0) {
        handle_control(s, info, &h, rcv + QMP_HDR_SIZE, rep);
        return;
    }
    const qmp_method_desc_t *m = NULL;
    rc = qmp_dispatch_lookup(s->desc, s->instance_id, &h, &m);
    if (rc != EOK) {
        send_error(s, info->rcvid, rc);
        return;
    }
    if (s->acl_check != NULL) {
        qmp_cred_t cred = {info->pid, info->cred_valid != 0 ? info->uid : -1,
                           info->cred_valid != 0 ? info->gid : -1, info->type_id};
        if (s->acl_check(s->ctx, &cred, h.resource_id) != EOK) {
            send_error(s, info->rcvid, EACCES);
            return;
        }
    }
    int oneway = (m->flags & QMP_MF_ONEWAY) != 0;
    if (QMP_HDR_SIZE + (size_t)m->out_size > rep_cap) {
        send_error(s, info->rcvid, EMSGSIZE); /* cannot happen with a sized work buffer */
        return;
    }
    /* The kernel silently truncates a reply to the client's buffer (V-QNX-032): detect an
     * undersized buffer here instead of letting the client read a cut struct. */
    if (!oneway && info->dstmsglen != 0 &&
        (size_t)info->dstmsglen < QMP_HDR_SIZE + (size_t)m->out_size) {
        send_error(s, info->rcvid, EMSGSIZE);
        return;
    }
    const void *in = m->in_size > 0 ? rcv + QMP_HDR_SIZE : NULL;
    if (m->validate_in != NULL && m->validate_in(in) != EOK) {
        send_error(s, info->rcvid, ERANGE);
        return;
    }
    qmp_hdr_t *rh = (qmp_hdr_t *)(void *)rep;
    if (oneway) {
        /* Reply BEFORE the work so the caller is unblocked and priority inheritance ends
         * with the delivery (DESIGN §3.4). The handler gets no reply buffer. */
        qmp_hdr_init_reply(rh, &h, 0, 0);
        send_reply(s, info->rcvid, rh, NULL, 0);
        qmp_req_t req_storage;
        qmp_req_impl_t *req = (qmp_req_impl_t *)(void *)&req_storage;
        memset(req, 0, sizeof(*req));
        fill_req(req, s, info, &h, 0, 0, NULL); /* already replied: not cancellable */
        req->state = QMP_REQ_ACTIVE;
        int32_t status = 0;
        rc = m->handler(s->ops, s->ctx, &req_storage, in, NULL, &status);
        if (rc == QMP_DEFERRED) {
            qmp_log("one-way handler for 0x%04x returned QMP_DEFERRED; ignored",
                    (unsigned)h.resource_id);
        }
        return;
    }
    /* Allocate the request from the deferred pool when one is configured, so the pointer
     * the handler keeps across QMP_DEFERRED stays valid. Pool missing or full: the slot
     * lives on this stack and QMP_DEFERRED degrades to EAGAIN at the client. */
    qmp_req_t req_storage;
    qmp_req_impl_t *req = pool_alloc(s);
    uint8_t pooled = req != NULL;
    if (!pooled) {
        req = (qmp_req_impl_t *)(void *)&req_storage;
        memset(req, 0, sizeof(*req));
        req->state = QMP_REQ_ACTIVE;
    }
    fill_req(req, s, info, &h, m->out_size, pooled, infl);

    if (infl != NULL) {
        infl_record(s, infl, info);
        /* An unblock pulse may have arrived before the slot existed (dropped as no-match):
         * ask the transport whether this client is still waiting to be unblocked. */
        if (s->tr->unblock_req != NULL && s->tr->unblock_req(&s->ep, info->rcvid) == 1) {
            (void)infl_clear(s, infl);
            if (pooled) {
                pool_set_state(s, req, QMP_REQ_FREE);
            }
            if (s->verbose) {
                qmp_log("rcvid=%lld: unblock requested before the handler ran; cancelled",
                        (long long)info->rcvid);
            }
            send_error(s, info->rcvid, ECANCELED);
            return;
        }
    }

    void *out = m->out_size > 0 ? rep + QMP_HDR_SIZE : NULL;
    if (out != NULL) {
        memset(out, 0, m->out_size); /* padding bytes are zero on the wire */
    }
    int32_t status = 0;
    rc = m->handler(s->ops, s->ctx, (qmp_req_t *)(void *)req, in, out, &status);
    /* Only the owning worker ever replies through its slot: once the slot has left
     * RUNNING here, a late unblock pulse finds no match and is dropped. If a canceller
     * claimed it first (CANCELED), the error reply already went out — or the client
     * disconnected — so the handler's result is discarded and there is no second reply. */
    int cancelled = infl != NULL ? infl_clear(s, infl) : 0;
    req->infl = NULL;
    if (rc == QMP_DEFERRED) {
        if (!pooled) {
            if (!cancelled) {
                send_error(s, info->rcvid, EAGAIN);
            }
            return;
        }
        (void)pthread_mutex_lock(&s->pool_mu);
        uint8_t st = req->state;
        if (st == QMP_REQ_ACTIVE) {
            /* Cancelled while running: never park a dead request. */
            req->state = cancelled ? QMP_REQ_CLAIMED : QMP_REQ_PARKED;
        } else if (st == QMP_REQ_DONE) {
            req->state = QMP_REQ_FREE; /* qmp_req_reply() already ran inside the handler */
        }
        (void)pthread_mutex_unlock(&s->pool_mu);
        if (st == QMP_REQ_ACTIVE && cancelled) {
            if (s->on_cancel != NULL) {
                s->on_cancel(s->ctx, (qmp_req_t *)(void *)req); /* app drops its pointer */
            }
            pool_set_state(s, req, QMP_REQ_FREE);
        }
        return;
    }
    if (pooled) {
        pool_set_state(s, req, QMP_REQ_FREE);
    }
    if (cancelled) {
        if (s->verbose) {
            qmp_log("rcvid=%lld: handler finished after cancellation; result discarded",
                    (long long)info->rcvid);
        }
        return;
    }
    if (rc != QMP_HANDLED) {
        send_error(s, info->rcvid, rc > 0 ? rc : EIO);
        return;
    }
    uint32_t payload_len = status == 0 ? m->out_size : 0;
    qmp_hdr_init_reply(rh, &h, status, payload_len);
    send_reply(s, info->rcvid, rh, out, payload_len);
}

int qmp_req_reply(qmp_req_t *req, const void *payload, size_t payload_len, int32_t status) {
    if (req == NULL || (payload == NULL && payload_len != 0)) {
        return EINVAL;
    }
    qmp_req_impl_t *r = (qmp_req_impl_t *)(void *)req;
    qmp_server_impl_t *s = r->server;
    if (s == NULL || r->pooled == 0) {
        return EINVAL; /* only deferrable (pooled) requests can be replied late */
    }
    if (status == 0 ? payload_len != r->out_size : payload_len != 0) {
        return EINVAL;
    }
    (void)pthread_mutex_lock(&s->pool_mu);
    uint8_t prev = r->state;
    if (prev != QMP_REQ_ACTIVE && prev != QMP_REQ_PARKED) {
        (void)pthread_mutex_unlock(&s->pool_mu);
        return EINVAL; /* already completed or cancelled */
    }
    if (prev == QMP_REQ_ACTIVE && r->infl != NULL) {
        if (r->infl->state == QMP_INFL_CANCELED) {
            /* Cancelled while the handler runs: the error reply already went out (or the
             * client is gone). Leave the slot CANCELED; dispatch cleans up on return. */
            (void)pthread_mutex_unlock(&s->pool_mu);
            return EINVAL;
        }
        /* Claim the slot so a concurrent unblock pulse cannot race this reply. */
        r->infl->state = QMP_INFL_IDLE;
        r->infl->rcvid = 0;
        r->infl->scoid = 0;
        r->infl = NULL;
    }
    r->state = QMP_REQ_CLAIMED;
    (void)pthread_mutex_unlock(&s->pool_mu);

    qmp_hdr_t rh;
    qmp_hdr_init_reply(&rh, &r->h, status, (uint32_t)payload_len);
    send_reply(s, r->rcvid, &rh, payload, payload_len); /* dead client == delivered */
    /* ACTIVE: the handler still holds the slot; dispatch frees it after QMP_DEFERRED. */
    pool_set_state(s, r, prev == QMP_REQ_ACTIVE ? QMP_REQ_DONE : QMP_REQ_FREE);
    return EOK;
}

/* Claim one PARKED slot matching the predicate; NULL when none. pool_mu held inside. */
static qmp_req_impl_t *claim_parked(qmp_server_impl_t *s, int by_rcvid, int64_t rcvid,
                                    int32_t scoid) {
    if (s->pool == NULL) {
        return NULL;
    }
    qmp_req_impl_t *r = NULL;
    (void)pthread_mutex_lock(&s->pool_mu);
    for (size_t i = 0; i < s->pool_count; i++) {
        qmp_req_impl_t *p = &s->pool[i];
        if (p->state != QMP_REQ_PARKED) {
            continue;
        }
        if (by_rcvid ? p->rcvid == rcvid : p->info.scoid == scoid) {
            p->state = QMP_REQ_CLAIMED;
            r = p;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s->pool_mu);
    return r;
}

void qmp_server_on_unblock(qmp_server_impl_t *s, int64_t rcvid) {
    /* The kernel keeps the client blocked until the server acts (V-QNX-006). Before
     * cancelling anything, confirm the client is still asking to be unblocked so a stale
     * pulse cannot cancel a newer request that reuses the same rcvid (V-QNX-037). The
     * server cannot tell a timeout from a signal: it answers with neutral ECANCELED and
     * the client maps it to ETIMEDOUT only when its own deadline has passed. */
    if (s->tr->unblock_req != NULL && s->tr->unblock_req(&s->ep, rcvid) != 1) {
        if (s->verbose) {
            qmp_log("unblock pulse rcvid=%lld: client no longer unblocking; dropped",
                    (long long)rcvid);
        }
        return;
    }
    /* Parked deferred request first. */
    qmp_req_impl_t *r = claim_parked(s, 1, rcvid, 0);
    if (r != NULL) {
        if (s->verbose) {
            qmp_log("unblock pulse rcvid=%lld: cancelling parked request", (long long)rcvid);
        }
        if (s->on_cancel != NULL) {
            s->on_cancel(s->ctx, (qmp_req_t *)(void *)r);
        }
        send_error(s, rcvid, ECANCELED);
        pool_set_state(s, r, QMP_REQ_FREE);
        return;
    }
    /* A synchronous handler may still be running: claim the matching in-flight slot under
     * the mutex, then release the client outside the lock. The owning worker discards its
     * result when it sees CANCELED, so there is no second reply. */
    qmp_inflight_t *hit = NULL;
    (void)pthread_mutex_lock(&s->pool_mu);
    for (unsigned i = 0; i < s->nthreads; i++) {
        qmp_inflight_t *f = &s->inflight[i];
        if (f->state == QMP_INFL_RUNNING && f->rcvid == rcvid) {
            f->state = QMP_INFL_CANCELED;
            hit = f;
            break;
        }
    }
    (void)pthread_mutex_unlock(&s->pool_mu);
    if (hit != NULL) {
        if (s->verbose) {
            qmp_log("unblock pulse rcvid=%lld: cancelling in-flight handler", (long long)rcvid);
        }
        send_error(s, rcvid, ECANCELED);
        return;
    }
    if (s->verbose) {
        qmp_log("unblock pulse rcvid=%lld: no matching request; dropped", (long long)rcvid);
    }
}

void qmp_server_on_disconnect(qmp_server_impl_t *s, int32_t scoid, int32_t pid) {
    /* The client process is gone: its rcvids are dead, so cancel without MsgError. The
     * transport has already detached the connection (ConnectDetach on QNX). */
    for (;;) {
        qmp_req_impl_t *r = claim_parked(s, 0, 0, scoid);
        if (r == NULL) {
            break;
        }
        if (s->on_cancel != NULL) {
            s->on_cancel(s->ctx, (qmp_req_t *)(void *)r);
        }
        pool_set_state(s, r, QMP_REQ_FREE);
    }
    /* Running handlers for this process observe it through qmp_req_canceled(); their
     * results are discarded and no reply is attempted on the dead rcvids. */
    (void)pthread_mutex_lock(&s->pool_mu);
    for (unsigned i = 0; i < s->nthreads; i++) {
        qmp_inflight_t *f = &s->inflight[i];
        if (f->state == QMP_INFL_RUNNING && f->scoid == scoid) {
            f->state = QMP_INFL_CANCELED;
        }
    }
    (void)pthread_mutex_unlock(&s->pool_mu);
    if (s->on_client_gone != NULL) {
        s->on_client_gone(s->ctx, pid);
    }
}

int qmp_req_get_info(const qmp_req_t *req, qmp_req_info_t *info) {
    if (req == NULL || info == NULL) {
        return EINVAL;
    }
    const qmp_req_impl_t *r = (const qmp_req_impl_t *)(const void *)req;
    *info = r->info;
    return EOK;
}

int qmp_req_canceled(const qmp_req_t *req) {
    if (req == NULL) {
        return 0;
    }
    const qmp_req_impl_t *r = (const qmp_req_impl_t *)(const void *)req;
    qmp_server_impl_t *s = r->server;
    if (s == NULL || r->infl == NULL) {
        return 0;
    }
    (void)pthread_mutex_lock(&s->pool_mu);
    int cancelled = r->infl->state == QMP_INFL_CANCELED;
    (void)pthread_mutex_unlock(&s->pool_mu);
    return cancelled;
}
