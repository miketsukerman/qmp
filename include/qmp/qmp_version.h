/* SPDX-License-Identifier: Apache-2.0 */
/*
 * qmp product and wire-protocol versions (ADR-011). This file is the single source of
 * truth: CMake, the generator's CI check and the release tag are all compared against it.
 *
 * The product version follows Semantic Versioning 2.0.0 and covers the runtime, the
 * generator and qmpctl together. The wire version is a separate integer: two processes
 * can talk if and only if their wire versions are equal.
 *
 * The macros say which headers a program was compiled against; qmp_version_string() and
 * friends (qmp.h) say which library it is linked to.
 */
#ifndef QMP_QMP_VERSION_H
#define QMP_QMP_VERSION_H

#define QMP_VERSION_MAJOR 0
#define QMP_VERSION_MINOR 3
#define QMP_VERSION_PATCH 7

#define QMP_VERSION_STR_(x)       #x
#define QMP_VERSION_STR(x)        QMP_VERSION_STR_(x)
#define QMP_VERSION_HEX_(a, b, c) (((a) << 16) | ((b) << 8) | (c))

/* "MAJOR.MINOR.PATCH" */
#define QMP_VERSION_STRING                                                                         \
    QMP_VERSION_STR(QMP_VERSION_MAJOR)                                                             \
    "." QMP_VERSION_STR(QMP_VERSION_MINOR) "." QMP_VERSION_STR(QMP_VERSION_PATCH)
/* (major << 16) | (minor << 8) | patch, comparable with < and >=. Usable in #if. */
#define QMP_VERSION_HEX QMP_VERSION_HEX_(QMP_VERSION_MAJOR, QMP_VERSION_MINOR, QMP_VERSION_PATCH)
#define QMP_VERSION_AT_LEAST(major, minor, patch)                                                  \
    (QMP_VERSION_HEX >= QMP_VERSION_HEX_(major, minor, patch))

/* Wire protocol version (DESIGN §2): header layout, flag bits, control opcodes and their
 * payload layouts, status codes and topic delivery semantics. Advertised in HELLO; a peer
 * that sends 0 predates the field and speaks version 1. */
#define QMP_WIRE_VERSION 1

#endif /* QMP_QMP_VERSION_H */
