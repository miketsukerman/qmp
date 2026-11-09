/* SPDX-License-Identifier: Apache-2.0 */
/* Naming and version compatibility (DESIGN §7, ADR-004). */
#include <stdio.h>

#include <qmp/qmp.h>

int qmp_service_path(char *buf, size_t cap, uint16_t service_id, uint16_t instance_id,
                     uint8_t ver_major) {
    if (buf == NULL || cap == 0) {
        return EINVAL;
    }
    int n = snprintf(buf, cap, "qmp/%04x-%04x/v%u", (unsigned)service_id, (unsigned)instance_id,
                     (unsigned)ver_major);
    if (n < 0 || (size_t)n >= cap) {
        return ENAMETOOLONG;
    }
    return EOK;
}

int qmp_version_compatible(uint8_t server_major, uint8_t server_minor, uint8_t client_major,
                           uint8_t client_minor) {
    /* Minor versions are additive only: a newer client may call an older server as long as
     * it only uses methods the server knows; unknown methods come back as ENOENT. */
    (void)server_minor;
    (void)client_minor;
    return server_major == client_major ? EOK : ENOTSUP;
}
