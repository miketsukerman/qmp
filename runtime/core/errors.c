/* SPDX-License-Identifier: Apache-2.0 */
/* errno <-> UCode mapping (DESIGN §6). */
#include <errno.h>
#include <stddef.h>

#include <qmp/qmp_errors.h>

int32_t qmp_errno_to_ucode(int err) {
    switch (err) {
    case 0:
        return QMP_UCODE_OK;
    case ETIMEDOUT:
        return QMP_UCODE_DEADLINE_EXCEEDED;
    case EINTR:
    case ECANCELED:
        return QMP_UCODE_CANCELLED;
    case EINVAL:
    case EMSGSIZE:
    case E2BIG:
        return QMP_UCODE_INVALID_ARGUMENT;
    case ERANGE:
        return QMP_UCODE_OUT_OF_RANGE;
    case ENOENT:
        return QMP_UCODE_NOT_FOUND;
    case EEXIST:
        return QMP_UCODE_ALREADY_EXISTS;
    case EACCES:
    case EPERM:
        return QMP_UCODE_PERMISSION_DENIED;
    case EAGAIN:
    case ENOMEM:
    case ENOSPC:
    case EMFILE:
    case ENFILE:
        return QMP_UCODE_RESOURCE_EXHAUSTED;
    case ENOTSUP:
    case EPROTO:
        return QMP_UCODE_FAILED_PRECONDITION;
    case ESRCH:
    case EBADF:
    case ECONNRESET:
    case ENOTCONN:
        return QMP_UCODE_UNAVAILABLE;
    case ENOSYS:
        return QMP_UCODE_UNIMPLEMENTED;
    default:
        return QMP_UCODE_UNKNOWN;
    }
}

int qmp_ucode_to_errno(int32_t code) {
    switch (code) {
    case QMP_UCODE_OK:
        return 0;
    case QMP_UCODE_CANCELLED:
        return EINTR;
    case QMP_UCODE_INVALID_ARGUMENT:
        return EINVAL;
    case QMP_UCODE_DEADLINE_EXCEEDED:
        return ETIMEDOUT;
    case QMP_UCODE_NOT_FOUND:
        return ENOENT;
    case QMP_UCODE_ALREADY_EXISTS:
        return EEXIST;
    case QMP_UCODE_PERMISSION_DENIED:
    case QMP_UCODE_UNAUTHENTICATED:
        return EACCES;
    case QMP_UCODE_RESOURCE_EXHAUSTED:
        return EAGAIN;
    case QMP_UCODE_FAILED_PRECONDITION:
        return ENOTSUP;
    case QMP_UCODE_OUT_OF_RANGE:
        return ERANGE;
    case QMP_UCODE_UNIMPLEMENTED:
        return ENOSYS;
    case QMP_UCODE_UNAVAILABLE:
        return ESRCH;
    default:
        return EIO;
    }
}

const char *qmp_ucode_name(int32_t code) {
    static const char *const names[] = {
        "OK",
        "CANCELLED",
        "UNKNOWN",
        "INVALID_ARGUMENT",
        "DEADLINE_EXCEEDED",
        "NOT_FOUND",
        "ALREADY_EXISTS",
        "PERMISSION_DENIED",
        "RESOURCE_EXHAUSTED",
        "FAILED_PRECONDITION",
        "ABORTED",
        "OUT_OF_RANGE",
        "UNIMPLEMENTED",
        "INTERNAL",
        "UNAVAILABLE",
        "DATA_LOSS",
        "UNAUTHENTICATED",
    };
    if (code < 0 || (size_t)code >= sizeof(names) / sizeof(names[0])) {
        return "?";
    }
    return names[code];
}
