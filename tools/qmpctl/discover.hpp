// SPDX-License-Identifier: Apache-2.0
// Service discovery for `qmpctl list`: there is no registry process, the names that the
// servers attached ARE the registry (DESIGN §7).
#ifndef QMPCTL_DISCOVER_HPP
#define QMPCTL_DISCOVER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace qmpctl {

struct Endpoint {
    uint16_t service_id = 0;
    uint16_t instance_id = 0;
    uint8_t ver_major = 0;

    std::string path() const; // "qmp/0101-0000/v1"
    bool operator<(const Endpoint &o) const;
};

// Where the given transport keeps its names: "/dev/name/local/qmp" for qnx, $QMP_UDS_DIR
// (default /tmp) for uds, "" for transports without a namespace (fake).
std::string discovery_root(const std::string &transport);

// Enumerate registered endpoints, sorted. qnx walks /dev/name/local/qmp/<svc>-<inst>/v<N>;
// uds scans the socket directory for qmp.<svc>-<inst>.v<N>.sock (stale files of killed
// servers included: connecting tells). Returns 0 or an errno; a missing root is not an
// error (nothing registered yet).
int discover(const std::string &transport, std::vector<Endpoint> &out);

// Parse "0101-0000" (svc-inst, 4 hex digits each). Exposed for the service-spec parser.
bool parse_hex4(const char *s, uint16_t &out);

} // namespace qmpctl
#endif
