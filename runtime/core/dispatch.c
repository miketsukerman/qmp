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
    if ((m->flags & QMP_MF_ONEWAY) != 0 || m->handler == NULL) {
        return ENOSYS; /* one-way dispatch arrives in W3 */
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

void qmp_server_dispatch(qmp_server_impl_t *s, const qmp_rcv_info_t *info, uint8_t *rcv, size_t len,
                         uint8_t *rep, size_t rep_cap) {
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
    if (QMP_HDR_SIZE + (size_t)m->out_size > rep_cap) {
        send_error(s, info->rcvid, EMSGSIZE); /* cannot happen with a sized work buffer */
        return;
    }
    const void *in = m->in_size > 0 ? rcv + QMP_HDR_SIZE : NULL;
    if (m->validate_in != NULL && m->validate_in(in) != EOK) {
        send_error(s, info->rcvid, ERANGE);
        return;
    }
    qmp_req_t req_storage;
    qmp_req_impl_t *req = (qmp_req_impl_t *)(void *)&req_storage;
    memset(req, 0, sizeof(*req));
    req->server = s;
    req->rcvid = info->rcvid;
    req->resource_id = h.resource_id;
    req->info.pid = info->pid;
    req->info.scoid = info->scoid;
    req->info.priority = info->priority;
    req->info.up_priority = h.up_priority;
    req->info.ver_minor = h.ver_minor;

    qmp_hdr_t *rh = (qmp_hdr_t *)(void *)rep;
    void *out = m->out_size > 0 ? rep + QMP_HDR_SIZE : NULL;
    if (out != NULL) {
        memset(out, 0, m->out_size); /* padding bytes are zero on the wire */
    }
    int32_t status = 0;
    rc = m->handler(s->ops, s->ctx, &req_storage, in, out, &status);
    if (rc == QMP_DEFERRED) {
        send_error(s, info->rcvid, ENOSYS); /* deferred replies arrive in W2 */
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

int qmp_req_get_info(const qmp_req_t *req, qmp_req_info_t *info) {
    if (req == NULL || info == NULL) {
        return EINVAL;
    }
    const qmp_req_impl_t *r = (const qmp_req_impl_t *)(const void *)req;
    *info = r->info;
    return EOK;
}
