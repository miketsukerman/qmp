/* SPDX-License-Identifier: Apache-2.0 */
/*
 * qmp — service-oriented communication over QNX native message passing.
 * Core definitions shared by clients, servers, transports and generated code.
 *
 * C11, stable C ABI (ADR-006). Also compiles as C++17 (extern "C").
 */
#ifndef QMP_QMP_H
#define QMP_QMP_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define QMP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#define QMP_ALIGNOF(T)               alignof(T)
extern "C" {
#else
#define QMP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#define QMP_ALIGNOF(T)               _Alignof(T)
#endif

#define QMP_VERSION_MAJOR 0
#define QMP_VERSION_MINOR 1
#define QMP_VERSION_PATCH 0

#ifndef EOK
#define EOK 0
#endif

/* ---- message type ------------------------------------------------------- */
/* Types 0.._IO_MAX are reserved for resource-manager messages. */
#if defined(__QNX__)
#include <sys/iomsg.h>
#define QMP_IO_MAX _IO_MAX
#else
#define QMP_IO_MAX 0x1FF /* VERIFY(qnx8): V-QNX-001 — _IO_MAX is 511 on SDP 8.0 */
#endif
#define QMP_MSG_TYPE ((uint16_t)(QMP_IO_MAX + 0x100))
QMP_STATIC_ASSERT(QMP_MSG_TYPE > QMP_IO_MAX, "qmp message type must be above _IO_MAX");
QMP_STATIC_ASSERT(QMP_MSG_TYPE == 0x2FF, "wire protocol v1 fixes QMP_MSG_TYPE to 0x2FF");

/* ---- header flags ------------------------------------------------------- */
enum qmp_flags {
    QMP_F_ONEWAY = 1U << 0,       /* server replies before running the handler        */
    QMP_F_IS_REPLY = 1U << 1,     /* set by the server in every reply header          */
    QMP_F_SHM_ATTACHED = 1U << 2, /* payload is followed by qmp_shm_desc_t (stretch)   */
    QMP_F_APP_ERROR = 1U << 3,    /* status != OK; payload may be absent               */
    QMP_F_CONTROL = 1U << 4,      /* resource_id is a qmp control opcode, not a method */
};
#define QMP_F_KNOWN_MASK                                                                           \
    ((uint16_t)(QMP_F_ONEWAY | QMP_F_IS_REPLY | QMP_F_SHM_ATTACHED | QMP_F_APP_ERROR |             \
                QMP_F_CONTROL))

/* ---- control opcodes (hdr.resource_id when QMP_F_CONTROL is set) -------- */
enum qmp_control_op {
    QMP_OP_HELLO = 1,
    QMP_OP_SUBSCRIBE = 2,
    QMP_OP_UNSUBSCRIBE = 3,
    QMP_OP_PULL = 4,
};

/* ---- wire header (DESIGN §2) -------------------------------------------- */
typedef struct qmp_hdr {
    uint16_t type;        /* 0  QMP_MSG_TYPE                                  */
    uint16_t flags;       /* 2  enum qmp_flags                                */
    uint16_t service_id;  /* 4  uProtocol service id                          */
    uint16_t instance_id; /* 6  0x0000 = default instance                     */
    uint16_t resource_id; /* 8  method <0x8000 | topic >=0x8000 | control op  */
    uint8_t ver_major;    /* 10                                               */
    uint8_t ver_minor;    /* 11                                               */
    uint32_t payload_len; /* 12 bytes following the header                    */
    int32_t status;       /* 16 replies: UCode, 0 = OK                        */
    uint8_t up_priority;  /* 20 UPriority 1..7 from adapter; 0 = native call  */
    uint8_t reserved[3];  /* 21 sender zeroes, receiver ignores               */
} qmp_hdr_t;

#define QMP_HDR_SIZE 24U
QMP_STATIC_ASSERT(sizeof(qmp_hdr_t) == QMP_HDR_SIZE, "qmp_hdr_t must be 24 bytes");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, flags) == 2, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, service_id) == 4, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, instance_id) == 6, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, resource_id) == 8, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, ver_major) == 10, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, ver_minor) == 11, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, payload_len) == 12, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, status) == 16, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, up_priority) == 20, "layout");
QMP_STATIC_ASSERT(offsetof(qmp_hdr_t, reserved) == 21, "layout");

/* Largest payload accepted by the runtime (defensive bound on untrusted payload_len). */
#define QMP_MAX_PAYLOAD (1U << 20)

/* ---- control payloads --------------------------------------------------- */
typedef struct qmp_hello {
    uint16_t service_id;
    uint16_t instance_id;
    uint8_t ver_major;
    uint8_t ver_minor;
    uint8_t _pad0[2];
} qmp_hello_t;
QMP_STATIC_ASSERT(sizeof(qmp_hello_t) == 8, "layout");

typedef struct qmp_hello_reply {
    uint8_t ver_major;
    uint8_t ver_minor;
    uint8_t _pad0[2];
    uint32_t server_generation; /* changes when the server restarts */
    uint32_t max_msg_size;      /* header + largest payload the server accepts */
    uint32_t _pad1;
} qmp_hello_reply_t;
QMP_STATIC_ASSERT(sizeof(qmp_hello_reply_t) == 16, "layout");

/* ---- handler return values ---------------------------------------------- */
#define QMP_HANDLED  0    /* reply now with *out and *status                  */
#define QMP_DEFERRED (-1) /* keep qmp_req_t*, reply later (W2)                */
/* any positive value is an errno sent to the client with MsgError()         */

/* ---- per-call options --------------------------------------------------- */
typedef struct qmp_call_opts {
    uint64_t timeout_ns; /* 0 = client default (0 = block forever)           */
    uint8_t up_priority; /* 0 = native call; 1..7 = UPriority CS0..CS6       */
    uint8_t _pad0[7];
} qmp_call_opts_t;
QMP_STATIC_ASSERT(sizeof(qmp_call_opts_t) == 16, "layout");

/* ---- header codec (runtime/core/hdr.c) ---------------------------------- */
/* Fill a request header; reserved bytes are zeroed. */
void qmp_hdr_init(qmp_hdr_t *h, uint16_t service_id, uint16_t instance_id, uint16_t resource_id,
                  uint8_t ver_major, uint8_t ver_minor, uint16_t flags, uint32_t payload_len);
/* Fill a reply header from the request header. status != 0 sets QMP_F_APP_ERROR. */
void qmp_hdr_init_reply(qmp_hdr_t *rep, const qmp_hdr_t *req, int32_t status, uint32_t payload_len);
/* Validate an incoming request header against the received byte count.
 * Returns EOK, EINVAL (bad type/flags/reply bit), EMSGSIZE (length mismatch). */
int qmp_hdr_validate(const qmp_hdr_t *h, size_t total_len);
/* Validate a reply header against the request it answers and the buffer capacity.
 * Returns EOK, EPROTO (not a reply / wrong resource), EMSGSIZE. */
int qmp_hdr_validate_reply(const qmp_hdr_t *rep, const qmp_hdr_t *req, size_t total_len,
                           size_t payload_cap);

/* ---- naming (DESIGN §7) ------------------------------------------------- */
/* "qmp/<service hex4>-<instance hex4>/v<major>", e.g. "qmp/0101-0000/v1". */
#define QMP_NAME_MAX 32
int qmp_service_path(char *buf, size_t cap, uint16_t service_id, uint16_t instance_id,
                     uint8_t ver_major);
/* Returns EOK if client (major, minor) may talk to server (major, minor); else ENOTSUP. */
int qmp_version_compatible(uint8_t server_major, uint8_t server_minor, uint8_t client_major,
                           uint8_t client_minor);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_H */
