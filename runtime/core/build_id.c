/* SPDX-License-Identifier: Apache-2.0 */
/* Build identity (ADR-011). Kept apart from version.c because qmp_build_id.h changes with
 * every commit: only this file is recompiled then. */
#include <qmp/qmp.h>

#include "qmp_build_id.h"

const char *qmp_build_id(void) {
    return QMP_BUILD_ID;
}

const char *qmp_build_version(void) {
    return QMP_BUILD_VERSION;
}
