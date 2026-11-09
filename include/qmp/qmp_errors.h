/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Error model (DESIGN §6): transport/OS errors are errno return values; application status
 * is a uProtocol UCode carried in qmp_hdr_t.status. The two are never mixed; this header
 * provides the documented mapping used at the adapter boundary.
 */
#ifndef QMP_QMP_ERRORS_H
#define QMP_QMP_ERRORS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* uProtocol UCode, up-spec v1.5.8 up-core-api/uprotocol/v1/ustatus.proto (V-UP-003). */
typedef enum qmp_ucode {
    QMP_UCODE_OK = 0,
    QMP_UCODE_CANCELLED = 1,
    QMP_UCODE_UNKNOWN = 2,
    QMP_UCODE_INVALID_ARGUMENT = 3,
    QMP_UCODE_DEADLINE_EXCEEDED = 4,
    QMP_UCODE_NOT_FOUND = 5,
    QMP_UCODE_ALREADY_EXISTS = 6,
    QMP_UCODE_PERMISSION_DENIED = 7,
    QMP_UCODE_RESOURCE_EXHAUSTED = 8,
    QMP_UCODE_FAILED_PRECONDITION = 9,
    QMP_UCODE_ABORTED = 10,
    QMP_UCODE_OUT_OF_RANGE = 11,
    QMP_UCODE_UNIMPLEMENTED = 12,
    QMP_UCODE_INTERNAL = 13,
    QMP_UCODE_UNAVAILABLE = 14,
    QMP_UCODE_DATA_LOSS = 15,
    QMP_UCODE_UNAUTHENTICATED = 16,
} qmp_ucode_t;

/* errno -> UCode (DESIGN §6 table). Unmapped errno values become QMP_UCODE_UNKNOWN. */
int32_t qmp_errno_to_ucode(int err);
/* UCode -> representative errno (first errno in each table row). Unknown -> EIO. */
int qmp_ucode_to_errno(int32_t code);
/* Stable upper-case name, e.g. "DEADLINE_EXCEEDED"; "?" for out-of-range values. */
const char *qmp_ucode_name(int32_t code);

#ifdef __cplusplus
}
#endif
#endif /* QMP_QMP_ERRORS_H */
