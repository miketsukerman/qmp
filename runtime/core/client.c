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
    int rc = ci->tr->call(&ci->conn, siov, sparts, riov, rparts, timeout_ns, &reply_len);
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
 * and qmp_client_reconnect() can run. Never retries the call itself (DESIGN §3.7). */
static int mark_dead_on(int rc, qmp_client_impl_t *ci) {
    if (rc == ESRCH || rc == EBADF) {
        (void)ci->tr->disconnect(&ci->conn);
        ci->connected = 0;
    }
    return rc;
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

int qmp_client_close(qmp_client_t *c) {
    if (c == NULL) {
        return EINVAL;
    }
    qmp_client_impl_t *ci = impl(c);
    int rc = EOK;
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
