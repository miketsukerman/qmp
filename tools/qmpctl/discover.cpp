// SPDX-License-Identifier: Apache-2.0
#include "discover.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <tuple>

#include <dirent.h>

#include <qmp/qmp.h>

namespace qmpctl {

namespace {

constexpr char kQnxRoot[] = "/dev/name/local/qmp";

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// "<svc hex4>-<inst hex4>" followed by exactly `rest` (may be empty).
bool parse_pair(const char *s, uint16_t &svc, uint16_t &inst, const char **rest) {
    if (!parse_hex4(s, svc) || s[4] != '-' || !parse_hex4(s + 5, inst)) {
        return false;
    }
    *rest = s + 9;
    return true;
}

// "v<N>" with N in 0..255, followed by exactly `tail`.
bool parse_major(const char *s, const char *tail, uint8_t &major) {
    if (s[0] != 'v' || s[1] < '0' || s[1] > '9') {
        return false;
    }
    char *end = nullptr;
    unsigned long v = std::strtoul(s + 1, &end, 10);
    if (v > UINT8_MAX || std::strcmp(end, tail) != 0) {
        return false;
    }
    major = static_cast<uint8_t>(v);
    return true;
}

template <typename Fn> int for_each_entry(const std::string &dir, Fn fn) {
    DIR *d = opendir(dir.c_str());
    if (d == nullptr) {
        return errno;
    }
    while (const dirent *e = readdir(d)) {
        fn(e->d_name);
    }
    (void)closedir(d);
    return 0;
}

} // namespace

bool parse_hex4(const char *s, uint16_t &out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(s[i]); // stops at the terminator: hex_digit('\0') is -1
        if (d < 0) {
            return false;
        }
        v = v << 4 | static_cast<unsigned>(d);
    }
    out = static_cast<uint16_t>(v);
    return true;
}

std::string Endpoint::path() const {
    char buf[QMP_NAME_MAX];
    if (qmp_service_path(buf, sizeof(buf), service_id, instance_id, ver_major) != EOK) {
        return "?";
    }
    return buf;
}

bool Endpoint::operator<(const Endpoint &o) const {
    return std::tie(service_id, instance_id, ver_major) <
           std::tie(o.service_id, o.instance_id, o.ver_major);
}

std::string discovery_root(const std::string &transport) {
    if (transport == "qnx") {
        return kQnxRoot;
    }
    if (transport == "uds") {
        const char *dir = std::getenv("QMP_UDS_DIR");
        return dir != nullptr && dir[0] != '\0' ? dir : "/tmp";
    }
    return "";
}

int discover(const std::string &transport, std::vector<Endpoint> &out) {
    out.clear();
    const std::string root = discovery_root(transport);
    int rc = 0;
    if (transport == "qnx") {
        std::vector<std::string> dirs;
        rc = for_each_entry(root, [&](const char *name) { dirs.emplace_back(name); });
        for (const std::string &dir : dirs) {
            Endpoint ep;
            const char *rest = nullptr;
            if (!parse_pair(dir.c_str(), ep.service_id, ep.instance_id, &rest) || *rest != '\0') {
                continue;
            }
            (void)for_each_entry(root + "/" + dir, [&](const char *name) {
                if (parse_major(name, "", ep.ver_major)) {
                    out.push_back(ep);
                }
            });
        }
    } else if (transport == "uds") {
        rc = for_each_entry(root, [&](const char *name) {
            Endpoint ep;
            const char *rest = nullptr;
            if (std::strncmp(name, "qmp.", 4) == 0 &&
                parse_pair(name + 4, ep.service_id, ep.instance_id, &rest) && *rest == '.' &&
                parse_major(rest + 1, ".sock", ep.ver_major)) {
                out.push_back(ep);
            }
        });
    }
    std::sort(out.begin(), out.end());
    return rc == ENOENT ? 0 : rc;
}

} // namespace qmpctl
