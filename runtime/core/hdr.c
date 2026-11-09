/* SPDX-License-Identifier: Apache-2.0 */
/* Wire header codec (DESIGN §2). All inputs are untrusted. */
#include <string.h>

#include <qmp/qmp.h>

void qmp_hdr_init(qmp_hdr_t *h, uint16_t service_id, uint16_t instance_id, uint16_t resource_id,
                  uint8_t ver_major, uint8_t ver_minor, uint16_t flags, uint32_t payload_len) {
    memset(h, 0, sizeof(*h));
    h->type = QMP_MSG_TYPE;
    h->flags = flags;
    h->service_id = service_id;
    h->instance_id = instance_id;
    h->resource_id = resource_id;
    h->ver_major = ver_major;
    h->ver_minor = ver_minor;
    h->payload_len = payload_len;
}

void qmp_hdr_init_reply(qmp_hdr_t *rep, const qmp_hdr_t *req, int32_t status,
                        uint32_t payload_len) {
    uint16_t flags = QMP_F_IS_REPLY | (uint16_t)(req->flags & QMP_F_CONTROL);
    if (status != 0) {
        flags |= QMP_F_APP_ERROR;
    }
    qmp_hdr_init(rep, req->service_id, req->instance_id, req->resource_id, req->ver_major,
                 req->ver_minor, flags, payload_len);
    rep->status = status;
    rep->up_priority = req->up_priority;
}

int qmp_hdr_validate(const qmp_hdr_t *h, size_t total_len) {
    if (total_len < QMP_HDR_SIZE) {
        return EMSGSIZE;
    }
    if (h->type != QMP_MSG_TYPE) {
        return EINVAL;
    }
    if ((h->flags & ~QMP_F_KNOWN_MASK) != 0 || (h->flags & QMP_F_IS_REPLY) != 0) {
        return EINVAL;
    }
    if (h->payload_len > QMP_MAX_PAYLOAD) {
        return EMSGSIZE;
    }
    if ((size_t)h->payload_len + QMP_HDR_SIZE != total_len) {
        return EMSGSIZE;
    }
    return EOK;
}

int qmp_hdr_validate_reply(const qmp_hdr_t *rep, const qmp_hdr_t *req, size_t total_len,
                           size_t payload_cap) {
    if (total_len < QMP_HDR_SIZE) {
        return EMSGSIZE;
    }
    if (rep->type != QMP_MSG_TYPE || (rep->flags & QMP_F_IS_REPLY) == 0) {
        return EPROTO;
    }
    if (rep->service_id != req->service_id || rep->instance_id != req->instance_id ||
        rep->resource_id != req->resource_id ||
        (rep->flags & QMP_F_CONTROL) != (req->flags & QMP_F_CONTROL)) {
        return EPROTO;
    }
    if (rep->payload_len > payload_cap) {
        return EMSGSIZE;
    }
    if ((size_t)rep->payload_len + QMP_HDR_SIZE > total_len) {
        return EMSGSIZE;
    }
    return EOK;
}
