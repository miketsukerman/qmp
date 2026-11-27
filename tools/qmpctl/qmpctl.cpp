// SPDX-License-Identifier: Apache-2.0
#include "qmpctl.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>

#include <qmp/qmp.h>
#include <qmp/qmp_client.h>
#include <qmp/qmp_errors.h>
#include <qmp/qmp_transport.h>

#include "codec.hpp"
#include "discover.hpp"
#include "qmp_build_id.h"
#include "reflect.hpp"

namespace qmpctl {

namespace {

constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitUsage = 2;
constexpr uint64_t kNsPerMs = 1000ULL * 1000ULL;
constexpr uint64_t kPollSliceNs = 200 * kNsPerMs; // how often subscribe looks at the stop flag

// Set from a signal handler (a lock-free atomic is async-signal-safe) or another thread.
std::atomic<int> g_stop{0};
static_assert(std::atomic<int>::is_always_lock_free, "g_stop is written from a signal handler");

const char kNotRegistered[] = "not registered";

const char kUsage[] =
    "usage: qmpctl [options] <command> [arguments]\n"
    "\n"
    "commands:\n"
    "  list                              services registered on this node\n"
    "  describe <service> [name]         interface of a service, or of one of its methods,\n"
    "                                    topics or messages\n"
    "  call <service> <method> [args]    call a method and print the reply as JSON\n"
    "  subscribe <service> <topic>       print every update as one JSON line\n"
    "\n"
    "<service> is a name (Joint, demo.joint.v1.Joint, demo.joint.Joint) or an id:\n"
    "  0101, 0101-0000, 0101-0000/v1 or qmp/0101-0000/v1 (service-instance, hex, /major).\n"
    "<method> and <topic> are names or resource ids (SetTarget, 1, 0x8001).\n"
    "[args] is one JSON object, key=value pairs (a.b=1 for nested fields), @file, or - to\n"
    "  read JSON from stdin. Fields that are left out keep their default (zero).\n"
    "\n"
    "options:\n"
    "  --transport qnx|uds|fake  transport to use (default: qnx on QNX, uds elsewhere)\n"
    "  --timeout MS              per-call timeout, 0 = wait forever (default 2000)\n"
    "  --reflect FILE            <svc>.reflect.json for services that do not serve their\n"
    "                            own reflection data (repeatable)\n"
    "  --json                    list/describe: machine-readable output\n"
    "  --pretty                  indent JSON output\n"
    "  --count N                 subscribe: stop after N updates\n"
    "  --duration MS             subscribe: stop after MS milliseconds\n"
    "  --version, --help\n";

struct Options {
    std::string transport_name;
    const qmp_transport_t *transport = nullptr;
    uint64_t timeout_ms = 2000;
    std::vector<std::string> reflect_files;
    bool json = false;
    bool pretty = false;
    uint64_t count = 0;
    uint64_t duration_ms = 0;
    std::vector<std::string> positional;
};

struct Io {
    std::istream &in;
    std::ostream &out;
    std::ostream &err;
};

std::string errno_text(int rc) {
    return std::string(std::strerror(rc)) + " (errno " + std::to_string(rc) + ")";
}

std::string hex4(unsigned v) {
    char buf[8];
    (void)std::snprintf(buf, sizeof(buf), "%04x", v & 0xFFFFU);
    return buf;
}

// Strings decoded from a payload are raw bytes: replace invalid UTF-8 instead of throwing.
std::string dump(const Json &j, bool pretty) {
    return j.dump(pretty ? 2 : -1, ' ', false, Json::error_handler_t::replace);
}

bool parse_u64(const std::string &s, uint64_t &out) {
    if (s.empty() || s[0] < '0' || s[0] > '9') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || *end != '\0') {
        return false;
    }
    out = v;
    return true;
}

// ---- service addressing ---------------------------------------------------------------

struct ServiceSpec {
    bool by_id = false;
    bool has_major = false;
    Endpoint ep; // by_id: service/instance (and major when has_major)
    std::string name;
};

// "[qmp/]SSSS[-IIII][/vN]" (hex) or "0xSSSS"; anything else is a name.
ServiceSpec parse_spec(const std::string &text) {
    ServiceSpec spec;
    spec.name = text;
    std::string s = text;
    if (s.rfind("qmp/", 0) == 0) {
        s = s.substr(4);
    }
    std::string major;
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        major = s.substr(slash + 1);
        s = s.substr(0, slash);
    }
    if (s.rfind("0x", 0) == 0 && s.size() > 2 && s.size() <= 6) {
        s = std::string(6 - s.size(), '0') + s.substr(2);
    }
    if (s.size() != 4 && s.size() != 9) {
        return spec;
    }
    if (!parse_hex4(s.c_str(), spec.ep.service_id)) {
        return spec;
    }
    if (s.size() == 9 && (s[4] != '-' || !parse_hex4(s.c_str() + 5, spec.ep.instance_id))) {
        return spec;
    }
    if (!major.empty()) {
        uint64_t v = 0;
        if (major[0] != 'v' || !parse_u64(major.substr(1), v) || v > UINT8_MAX) {
            return spec;
        }
        spec.ep.ver_major = static_cast<uint8_t>(v);
        spec.has_major = true;
    }
    spec.by_id = true;
    return spec;
}

// ---- session: one connected client plus the service's reflection ------------------------

struct Session {
    qmp_client_t client{};
    bool open = false;
    Endpoint ep;
    Doc doc;
    const Service *svc = nullptr;
    bool from_file = false; // reflection came from --reflect, not from the service

    Session() = default;
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    ~Session() { close(); }
    void close() {
        if (open) {
            (void)qmp_client_close(&client);
            open = false;
        }
    }
};

int fetch_reflection(qmp_client_t *c, std::string &text) {
    text.clear();
    char buf[QMP_REFLECT_CHUNK_MAX];
    uint32_t total = 0;
    do {
        uint32_t len = 0;
        int rc = qmp_client_reflect(c, static_cast<uint32_t>(text.size()), buf, sizeof(buf), &len,
                                    &total);
        if (rc != EOK) {
            return rc;
        }
        if (len == 0 && text.size() < total) {
            return EPROTO; // no progress: never loop forever on a broken server
        }
        text.append(buf, len);
    } while (text.size() < total);
    return EOK;
}

bool load_reflect_files(const Options &opt, std::vector<Doc> &docs, std::string &err) {
    for (const std::string &file : opt.reflect_files) {
        std::ifstream f(file, std::ios::binary);
        if (!f) {
            err = "cannot read " + file;
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        Doc doc;
        if (!parse_reflection(ss.str(), doc, err)) {
            err = file + ": " + err;
            return false;
        }
        docs.push_back(std::move(doc));
    }
    return true;
}

// Connect and load the reflection (from the service, else from a --reflect file). On
// failure returns false with a message; s.open tells whether the connection itself worked.
bool open_session(const Options &opt, const std::vector<Doc> &files, const Endpoint &ep, Session &s,
                  std::string &err) {
    s.close();
    s.ep = ep;
    s.svc = nullptr;
    qmp_client_cfg_t cfg{};
    cfg.transport = opt.transport;
    cfg.default_timeout_ns = opt.timeout_ms * kNsPerMs;
    int rc = qmp_client_init(&s.client, &cfg);
    if (rc == EOK) {
        // Minor 0: qmpctl only uses what the service's own reflection data describes.
        rc = qmp_client_connect(&s.client, ep.service_id, ep.instance_id, ep.ver_major, 0);
    }
    if (rc != EOK) {
        if (rc == ENOENT) {
            err = kNotRegistered;
        } else {
            err = "connect failed: " + errno_text(rc);
        }
        return false;
    }
    s.open = true;
    std::string text;
    rc = fetch_reflection(&s.client, text);
    if (rc == EOK) {
        if (!parse_reflection(text, s.doc, err)) {
            return false;
        }
        s.from_file = false;
    } else if (rc == ENOSYS) {
        const Doc *found = nullptr;
        for (const Doc &d : files) {
            if (d.find_service(ep.service_id, ep.ver_major) != nullptr) {
                found = &d;
            }
        }
        if (found == nullptr) {
            err = "no reflection data (pass --reflect <svc>.reflect.json)";
            return false;
        }
        s.doc = *found;
        s.from_file = true;
    } else {
        err = "reading reflection data failed: " + errno_text(rc);
        return false;
    }
    s.svc = s.doc.find_service(ep.service_id, ep.ver_major);
    if (s.svc == nullptr) {
        err = "reflection data does not describe service " + hex4(ep.service_id) + " v" +
              std::to_string(ep.ver_major);
        return false;
    }
    return true;
}

// Turn a <service> argument into an open session. Prints its own error.
bool resolve(const Options &opt, const std::string &text, Session &s, Io &io) {
    std::vector<Doc> files;
    std::string err;
    if (!load_reflect_files(opt, files, err)) {
        io.err << "qmpctl: " << err << "\n";
        return false;
    }
    const ServiceSpec spec = parse_spec(text);
    std::vector<Endpoint> found;
    if (!(spec.by_id && spec.has_major)) {
        int rc = discover(opt.transport_name, found);
        if (rc != 0) {
            io.err << "qmpctl: cannot read " << discovery_root(opt.transport_name) << ": "
                   << errno_text(rc) << "\n";
            return false;
        }
    }
    std::vector<Endpoint> candidates;
    if (spec.by_id) {
        if (spec.has_major) {
            candidates.push_back(spec.ep);
        } else {
            for (const Endpoint &ep : found) {
                if (ep.service_id == spec.ep.service_id && ep.instance_id == spec.ep.instance_id) {
                    candidates.push_back(ep);
                }
            }
            if (candidates.empty()) {
                // Not discoverable (the fake transport has no namespace): a --reflect file
                // still knows the major version.
                for (const Doc &d : files) {
                    for (const Service &svc : d.services) {
                        if (svc.service_id == spec.ep.service_id) {
                            Endpoint ep = spec.ep;
                            ep.ver_major = svc.ver_major;
                            candidates.push_back(ep);
                        }
                    }
                }
            }
        }
    } else {
        // By name: ask every registered service who it is.
        for (const Endpoint &ep : found) {
            Session probe;
            std::string ignored;
            if (open_session(opt, files, ep, probe, ignored) &&
                probe.svc->matches_name(spec.name)) {
                candidates.push_back(ep);
            }
        }
        if (candidates.empty()) {
            for (const Doc &d : files) {
                for (const Service &svc : d.services) {
                    if (svc.matches_name(spec.name)) {
                        Endpoint ep;
                        ep.service_id = svc.service_id;
                        ep.ver_major = svc.ver_major;
                        candidates.push_back(ep);
                    }
                }
            }
        }
    }
    if (candidates.empty()) {
        io.err << "qmpctl: no service '" << text << "' is registered (transport "
               << opt.transport_name << "); `qmpctl list` shows what is\n";
        return false;
    }
    if (candidates.size() > 1) {
        io.err << "qmpctl: '" << text << "' is ambiguous, use one of:";
        for (const Endpoint &ep : candidates) {
            io.err << " " << ep.path();
        }
        io.err << "\n";
        return false;
    }
    if (!open_session(opt, files, candidates[0], s, err)) {
        io.err << "qmpctl: " << candidates[0].path() << ": " << err << "\n";
        return false;
    }
    return true;
}

// ---- list ---------------------------------------------------------------------------------

int cmd_list(const Options &opt, Io &io) {
    if (!opt.positional.empty()) {
        io.err << "qmpctl: list takes no arguments\n";
        return kExitUsage;
    }
    std::vector<Doc> files;
    std::string err;
    if (!load_reflect_files(opt, files, err)) {
        io.err << "qmpctl: " << err << "\n";
        return kExitFailed;
    }
    std::vector<Endpoint> found;
    int rc = discover(opt.transport_name, found);
    if (rc != 0) {
        io.err << "qmpctl: cannot read " << discovery_root(opt.transport_name) << ": "
               << errno_text(rc) << "\n";
        return kExitFailed;
    }
    struct Row {
        std::string path, service, version, methods, topics, uuri, note;
    };
    std::vector<Row> rows;
    Json arr = Json::array();
    for (const Endpoint &ep : found) {
        Session s;
        Row row;
        row.path = ep.path();
        Json j = Json::object();
        j["name"] = row.path;
        j["service_id"] = ep.service_id;
        j["instance_id"] = ep.instance_id;
        const bool ok = open_session(opt, files, ep, s, err);
        if (!s.open && err == kNotRegistered) {
            // The name exists (it was just enumerated) but nobody answers behind it: what
            // a server killed with SIGKILL leaves on the uds transport.
            err = "not running (stale name)";
        }
        j["reachable"] = s.open;
        if (s.open) {
            row.version = std::to_string(ep.ver_major) + "." +
                          std::to_string(qmp_client_server_minor(&s.client));
            j["version"] = {{"major", ep.ver_major}, {"minor", qmp_client_server_minor(&s.client)}};
        } else {
            j["version"] = {{"major", ep.ver_major}};
        }
        if (ok) {
            row.service = s.svc->service_name.empty() ? s.svc->full_name : s.svc->service_name;
            row.methods = std::to_string(s.svc->methods.size());
            row.topics = std::to_string(s.svc->topics.size());
            row.uuri = s.svc->uuri;
            j["service"] = row.service;
            j["full_name"] = s.svc->full_name;
            j["methods"] = s.svc->methods.size();
            j["topics"] = s.svc->topics.size();
            j["uuri"] = row.uuri;
        } else {
            row.note = err;
            j["error"] = err;
        }
        rows.push_back(std::move(row));
        arr.push_back(std::move(j));
    }
    if (opt.json) {
        io.out << dump(arr, opt.pretty) << "\n";
        return kExitOk;
    }
    if (rows.empty()) {
        io.err << "qmpctl: no services registered under " << discovery_root(opt.transport_name)
               << " (transport " << opt.transport_name << ")\n";
        return kExitOk;
    }
    size_t wpath = 4;
    size_t wsvc = 7;
    for (const Row &r : rows) {
        wpath = std::max(wpath, r.path.size());
        wsvc = std::max(wsvc, r.service.size());
    }
    // Left-justified cell followed by the two-space column gap.
    auto cell = [](const std::string &text, size_t width) {
        return text + std::string(width > text.size() ? width - text.size() : 0, ' ') + "  ";
    };
    io.out << cell("NAME", wpath) << cell("SERVICE", wsvc) << cell("VERSION", 7)
           << cell("METHODS", 7) << cell("TOPICS", 6) << "UURI\n";
    for (const Row &r : rows) {
        io.out << cell(r.path, wpath);
        if (!r.note.empty()) {
            io.out << cell("-", wsvc) << cell(r.version.empty() ? "-" : r.version, 7);
            io.out << "(" << r.note << ")\n";
            continue;
        }
        io.out << cell(r.service, wsvc) << cell(r.version, 7) << cell(r.methods, 7)
               << cell(r.topics, 6) << r.uuri << "\n";
    }
    return kExitOk;
}

// ---- describe -----------------------------------------------------------------------------

std::string field_type(const Field &f) {
    std::string t = (f.kind == Kind::Enum || f.kind == Kind::Message) ? f.type_name : f.type;
    return f.repeated ? "repeated " + t : t;
}

void print_message(const Doc &doc, const std::string &name, std::ostream &out) {
    const Message *m = doc.find_message(name);
    if (m == nullptr) {
        return;
    }
    if (m->fields.empty()) {
        out << "message " << m->name << " {}  // no payload\n";
        return;
    }
    out << "message " << m->name << " {  // " << m->size << " bytes\n";
    size_t width = 0;
    std::vector<std::string> decls;
    for (const Field &f : m->fields) {
        decls.push_back(field_type(f) + " " + f.name + " = " + std::to_string(f.number) + ";");
        width = std::max(width, decls.back().size());
    }
    for (size_t i = 0; i < m->fields.size(); i++) {
        const Field &f = m->fields[i];
        out << "  " << std::left << std::setw(static_cast<int>(width)) << decls[i] << "  // offset "
            << f.offset;
        if (f.max_len != 0) {
            out << ", max_len " << f.max_len;
        }
        if (f.max_count != 0) {
            out << ", max_count " << f.max_count;
        }
        if (f.kind == Kind::Enum && !f.enum_values.empty()) {
            out << ", values";
            for (const auto &ev : f.enum_values) {
                out << " " << ev.first << "=" << ev.second;
            }
        }
        out << "\n";
    }
    out << "}\n";
}

// name plus every message it embeds, dependencies last, each once.
void collect_messages(const Doc &doc, const std::string &name, std::vector<std::string> &order,
                      std::set<std::string> &seen) {
    const Message *m = doc.find_message(name);
    if (m == nullptr || !seen.insert(name).second) {
        return;
    }
    order.push_back(name);
    for (const Field &f : m->fields) {
        if (f.kind == Kind::Message) {
            collect_messages(doc, f.type_name, order, seen);
        }
    }
}

std::string method_line(const Method &m) {
    return "0x" + hex4(m.id) + "  " + m.name + "(" + m.input + ") returns (" + m.output + ")" +
           (m.oneway ? "  [oneway]" : "");
}

std::string topic_line(const Topic &t) {
    std::string mode = t.mode == "QUEUE" ? "QUEUE depth " + std::to_string(t.depth) : t.mode;
    return "0x" + hex4(t.id) + "  " + t.name + "  " + t.message + "  " + mode +
           (t.notification ? "  [notification]" : "");
}

const Json *raw_entry(const Json &raw, const Service &svc, const char *group,
                      const std::string &name) {
    auto services = raw.find("services");
    if (services == raw.end()) {
        return nullptr;
    }
    for (const Json &s : *services) {
        auto id = s.find("service_id");
        auto list = s.find(group);
        if (id == s.end() || *id != svc.service_id || list == s.end()) {
            continue;
        }
        for (const Json &e : *list) {
            auto n = e.find("name");
            if (n != e.end() && *n == name) {
                return &e;
            }
        }
    }
    return nullptr;
}

Json raw_messages(const Doc &doc, const std::vector<std::string> &names) {
    Json out = Json::object();
    for (const std::string &n : names) {
        out[n] = doc.raw["messages"][n];
    }
    return out;
}

int cmd_describe(const Options &opt, Io &io) {
    if (opt.positional.empty() || opt.positional.size() > 2) {
        io.err << "usage: qmpctl describe <service> [method|topic|message]\n";
        return kExitUsage;
    }
    Session s;
    if (!resolve(opt, opt.positional[0], s, io)) {
        return kExitFailed;
    }
    const Service &svc = *s.svc;
    const Doc &doc = s.doc;
    if (opt.positional.size() == 1) {
        if (opt.json) {
            io.out << dump(doc.raw, true) << "\n";
            return kExitOk;
        }
        io.out << "service " << (svc.full_name.empty() ? svc.name : svc.full_name) << "\n"
               << "  name      " << svc.service_name << "\n"
               << "  id        0x" << hex4(svc.service_id) << "  instance 0x"
               << hex4(s.ep.instance_id) << "\n"
               << "  version   " << static_cast<unsigned>(svc.ver_major) << "."
               << static_cast<unsigned>(qmp_client_server_minor(&s.client)) << "\n"
               << "  uuri      " << svc.uuri << "\n"
               << "  endpoint  " << s.ep.path() << "  (transport " << opt.transport_name << ")\n";
        if (s.from_file) {
            io.out << "  note      described from a --reflect file, not by the service\n";
        }
        io.out << "\nmethods:\n";
        for (const Method &m : svc.methods) {
            io.out << "  " << method_line(m) << "\n";
        }
        if (svc.methods.empty()) {
            io.out << "  (none)\n";
        }
        io.out << "\ntopics:\n";
        for (const Topic &t : svc.topics) {
            io.out << "  " << topic_line(t) << "\n";
        }
        if (svc.topics.empty()) {
            io.out << "  (none)\n";
        }
        std::vector<std::string> order;
        std::set<std::string> seen;
        for (const Method &m : svc.methods) {
            collect_messages(doc, m.input, order, seen);
            collect_messages(doc, m.output, order, seen);
        }
        for (const Topic &t : svc.topics) {
            collect_messages(doc, t.message, order, seen);
        }
        io.out << "\n";
        for (const std::string &name : order) {
            print_message(doc, name, io.out);
        }
        return kExitOk;
    }

    const std::string &what = opt.positional[1];
    std::vector<std::string> order;
    std::set<std::string> seen;
    if (const Method *m = svc.find_method(what)) {
        collect_messages(doc, m->input, order, seen);
        collect_messages(doc, m->output, order, seen);
        const Json example = template_of(doc, m->input);
        if (opt.json) {
            Json j = Json::object();
            const Json *raw = raw_entry(doc.raw, svc, "methods", m->name);
            j["method"] = raw != nullptr ? *raw : Json::object();
            j["messages"] = raw_messages(doc, order);
            j["example"] = example;
            io.out << dump(j, true) << "\n";
            return kExitOk;
        }
        io.out << "method " << svc.name << "." << m->name << "\n"
               << "  " << method_line(*m) << "\n"
               << "  uuri  " << m->uuri << "\n\n";
        for (const std::string &name : order) {
            print_message(doc, name, io.out);
        }
        io.out << "\nexample:\n  qmpctl call " << s.ep.path().substr(4) << " " << m->name << " '"
               << dump(example, false) << "'\n";
        return kExitOk;
    }
    if (const Topic *t = svc.find_topic(what)) {
        collect_messages(doc, t->message, order, seen);
        if (opt.json) {
            Json j = Json::object();
            const Json *raw = raw_entry(doc.raw, svc, "topics", t->name);
            j["topic"] = raw != nullptr ? *raw : Json::object();
            j["messages"] = raw_messages(doc, order);
            io.out << dump(j, true) << "\n";
            return kExitOk;
        }
        io.out << "topic " << svc.name << "." << t->name << "\n"
               << "  " << topic_line(*t) << "\n"
               << "  uuri  " << t->uuri << "\n\n";
        for (const std::string &name : order) {
            print_message(doc, name, io.out);
        }
        io.out << "\nexample:\n  qmpctl subscribe " << s.ep.path().substr(4) << " " << t->name
               << "\n";
        return kExitOk;
    }
    // A message: full name, or an unambiguous last component ("JointState").
    std::vector<std::string> matches;
    for (const auto &kv : doc.messages) {
        const std::string &n = kv.first;
        if (n == what ||
            (n.size() > what.size() && n.compare(n.size() - what.size(), what.size(), what) == 0 &&
             n[n.size() - what.size() - 1] == '.')) {
            matches.push_back(n);
        }
    }
    if (matches.size() != 1) {
        io.err << "qmpctl: " << svc.name << " has no method, topic or message '" << what << "'\n";
        return kExitFailed;
    }
    collect_messages(doc, matches[0], order, seen);
    if (opt.json) {
        io.out << dump(raw_messages(doc, order), true) << "\n";
        return kExitOk;
    }
    for (const std::string &name : order) {
        print_message(doc, name, io.out);
    }
    return kExitOk;
}

// ---- call ---------------------------------------------------------------------------------

// Shell-friendly arguments: key=value, value parsed as JSON when it is JSON (1.5, true,
// [1,2], "x") and taken as a string otherwise; a.b=1 addresses a nested message.
bool args_from_pairs(const std::vector<std::string> &pairs, Json &out, std::string &err) {
    out = Json::object();
    for (const std::string &p : pairs) {
        size_t eq = p.find('=');
        if (eq == std::string::npos || eq == 0) {
            err = "'" + p + "': expected key=value, a JSON object, @file or -";
            return false;
        }
        Json value = Json::parse(p.substr(eq + 1), nullptr, false);
        if (value.is_discarded()) {
            value = p.substr(eq + 1);
        }
        Json *at = &out;
        std::string key = p.substr(0, eq);
        for (size_t dot = key.find('.'); dot != std::string::npos; dot = key.find('.')) {
            Json &child = (*at)[key.substr(0, dot)];
            if (child.is_null()) {
                child = Json::object();
            }
            if (!child.is_object()) {
                err = "'" + p + "': " + key.substr(0, dot) + " is already set to a value";
                return false;
            }
            at = &child;
            key = key.substr(dot + 1);
        }
        (*at)[key] = std::move(value);
    }
    return true;
}

bool read_call_args(const std::vector<std::string> &rest, Io &io, Json &out, std::string &err) {
    if (rest.empty()) {
        out = nullptr; // all defaults
        return true;
    }
    std::string text;
    if (rest.size() == 1 && rest[0] == "-") {
        std::stringstream ss;
        ss << io.in.rdbuf();
        text = ss.str();
    } else if (rest.size() == 1 && rest[0][0] == '@') {
        std::ifstream f(rest[0].substr(1), std::ios::binary);
        if (!f) {
            err = "cannot read " + rest[0].substr(1);
            return false;
        }
        std::stringstream ss;
        ss << f.rdbuf();
        text = ss.str();
    } else if (rest.size() == 1 && rest[0].find_first_not_of(" \t\r\n") != std::string::npos &&
               rest[0][rest[0].find_first_not_of(" \t\r\n")] == '{') {
        text = rest[0];
    } else {
        return args_from_pairs(rest, out, err);
    }
    out = Json::parse(text, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        err = "the arguments are not a JSON object";
        return false;
    }
    return true;
}

// ENOSYS from a method that reflection lists: declared in the IDL, no handler behind it.
const char *enosys_hint(int rc) {
    return rc == ENOSYS ? ": the service declares the method but does not implement it" : "";
}

int cmd_call(const Options &opt, Io &io) {
    if (opt.positional.size() < 2) {
        io.err << "usage: qmpctl call <service> <method> [json | key=value... | @file | -]\n";
        return kExitUsage;
    }
    Session s;
    if (!resolve(opt, opt.positional[0], s, io)) {
        return kExitFailed;
    }
    const Method *m = s.svc->find_method(opt.positional[1]);
    if (m == nullptr) {
        io.err << "qmpctl: " << s.svc->name << " has no method '" << opt.positional[1]
               << "' (methods:";
        for (const Method &cand : s.svc->methods) {
            io.err << " " << cand.name;
        }
        io.err << ")\n";
        return kExitFailed;
    }
    Json args;
    std::string err;
    std::vector<std::string> rest(opt.positional.begin() + 2, opt.positional.end());
    if (!read_call_args(rest, io, args, err)) {
        io.err << "qmpctl: " << err << "\n";
        return kExitUsage;
    }
    Payload req;
    if (!encode(s.doc, m->input, args, req, err)) {
        io.err << "qmpctl: " << m->name << ": " << err << "\n";
        return kExitUsage;
    }
    const std::string label = s.svc->name + "." + m->name;
    if (m->oneway) {
        int rc = qmp_client_oneway(&s.client, m->id, req.size() != 0 ? req.data() : nullptr,
                                   req.size(), nullptr);
        if (rc != EOK) {
            io.err << "qmpctl: " << label << " failed: " << errno_text(rc) << enosys_hint(rc)
                   << "\n";
            return kExitFailed;
        }
        return kExitOk; // one-way: delivered, and there is no reply to print
    }
    const Message *om = s.doc.find_message(m->output);
    Payload rep(om != nullptr ? om->size : 0);
    int32_t status = 0;
    int rc = qmp_client_call(&s.client, m->id, req.size() != 0 ? req.data() : nullptr, req.size(),
                             rep.size() != 0 ? rep.data() : nullptr, rep.size(), &status, nullptr);
    if (rc != EOK) {
        io.err << "qmpctl: " << label << " failed: " << errno_text(rc) << enosys_hint(rc) << "\n";
        return kExitFailed;
    }
    if (status != 0) {
        io.err << "qmpctl: " << label << " returned status " << status << " ("
               << qmp_ucode_name(status) << ")\n";
        return kExitFailed;
    }
    Json out;
    if (!decode(s.doc, m->output, rep.data(), rep.size(), out, err)) {
        io.err << "qmpctl: " << label << ": bad reply: " << err << "\n";
        return kExitFailed;
    }
    io.out << dump(out, opt.pretty) << "\n";
    return kExitOk;
}

// ---- subscribe ----------------------------------------------------------------------------

uint64_t realtime_ns() {
    timespec ts = {};
    (void)clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
}

struct SubCtx {
    const Doc *doc = nullptr;
    const Topic *topic = nullptr;
    const Options *opt = nullptr;
    std::ostream *out = nullptr;
    uint64_t received = 0;
    uint64_t dropped = 0;
    std::string error;
};

// qmp_sub_cb_t: runs on the thread inside qmp_client_poll(), once per update, in order.
void on_sample(void *ctx_, const void *sample, uint32_t len, uint64_t seq, uint64_t dropped) {
    auto *ctx = static_cast<SubCtx *>(ctx_);
    const uint64_t rx_ns = realtime_ns(); // taken first: closest to the pull
    if (!ctx->error.empty() || (ctx->opt->count != 0 && ctx->received >= ctx->opt->count)) {
        return;
    }
    Json value;
    if (!decode(*ctx->doc, ctx->topic->message, static_cast<const uint8_t *>(sample), len, value,
                ctx->error)) {
        return;
    }
    ctx->received++;
    ctx->dropped += dropped;
    Json line = Json::object();
    line["topic"] = ctx->topic->name;
    line["seq"] = seq;
    line["dropped"] = dropped;
    line["rx_ns"] = rx_ns;
    line["sample"] = std::move(value);
    *ctx->out << dump(line, false) << "\n" << std::flush;
}

int cmd_subscribe(const Options &opt, Io &io) {
    if (opt.positional.size() != 2) {
        io.err << "usage: qmpctl subscribe <service> <topic> [--count N] [--duration MS]\n";
        return kExitUsage;
    }
    Session s;
    if (!resolve(opt, opt.positional[0], s, io)) {
        return kExitFailed;
    }
    const Topic *t = s.svc->find_topic(opt.positional[1]);
    if (t == nullptr) {
        io.err << "qmpctl: " << s.svc->name << " has no topic '" << opt.positional[1]
               << "' (topics:";
        for (const Topic &cand : s.svc->topics) {
            io.err << " " << cand.name;
        }
        io.err << ")\n";
        return kExitFailed;
    }
    const Message *tm = s.doc.find_message(t->message);
    Payload sample(tm->size);
    SubCtx ctx;
    ctx.doc = &s.doc;
    ctx.topic = t;
    ctx.opt = &opt;
    ctx.out = &io.out;
    auto sub = std::make_unique<qmp_sub_t>();
    int rc = qmp_client_subscribe(&s.client, t->id, sample.data(), sample.size(), on_sample, &ctx,
                                  sub.get());
    if (rc != EOK) {
        io.err << "qmpctl: subscribing to " << s.svc->name << "." << t->name
               << " failed: " << errno_text(rc) << "\n";
        return kExitFailed;
    }
    const auto start = std::chrono::steady_clock::now();
    int exit_code = kExitOk;
    g_stop = 0; // a stop request is for the subscription that is running, not a later one
    while (g_stop == 0 && (opt.count == 0 || ctx.received < opt.count)) {
        uint64_t slice = kPollSliceNs;
        if (opt.duration_ms != 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - start)
                                     .count();
            const uint64_t limit = opt.duration_ms * kNsPerMs;
            if (static_cast<uint64_t>(elapsed) >= limit) {
                break;
            }
            slice = std::min(slice, limit - static_cast<uint64_t>(elapsed));
        }
        rc = qmp_client_poll(&s.client, slice);
        if (!ctx.error.empty()) {
            io.err << "qmpctl: bad sample: " << ctx.error << "\n";
            exit_code = kExitFailed;
            break;
        }
        if (!io.out) {
            break; // the reader went away
        }
        if (rc == EOK || rc == ETIMEDOUT || rc == EINTR) {
            continue;
        }
        if (rc == ESRCH) {
            // Server died. Reconnect re-registers the subscription; sequence numbers
            // restart with the new server generation.
            io.err << "qmpctl: " << s.ep.path() << " went away, reconnecting\n";
            rc = qmp_client_reconnect(&s.client);
            if (rc == EOK) {
                io.err << "qmpctl: reconnected (server generation "
                       << qmp_client_server_generation(&s.client) << ")\n";
                continue;
            }
        }
        io.err << "qmpctl: " << s.svc->name << "." << t->name << ": " << errno_text(rc) << "\n";
        exit_code = kExitFailed;
        break;
    }
    (void)qmp_client_unsubscribe(&s.client, sub.get());
    io.err << "qmpctl: " << ctx.received << " updates received, " << ctx.dropped << " dropped\n";
    return exit_code;
}

// ---- command line -------------------------------------------------------------------------

bool parse_args(const std::vector<std::string> &args, Options &opt, std::string &command,
                bool &help, bool &version, std::string &err) {
    bool options_done = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string &a = args[i];
        auto value = [&](std::string &out) {
            if (i + 1 >= args.size()) {
                err = a + " needs a value";
                return false;
            }
            out = args[++i];
            return true;
        };
        auto number = [&](uint64_t &out) {
            std::string v;
            if (!value(v)) {
                return false;
            }
            if (!parse_u64(v, out)) {
                err = a + ": '" + v + "' is not a number";
                return false;
            }
            return true;
        };
        if (options_done || a.size() < 2 || a[0] != '-') {
            if (command.empty()) {
                command = a;
            } else {
                opt.positional.push_back(a);
            }
            continue;
        }
        if (a == "--") {
            options_done = true;
        } else if (a == "--help" || a == "-h") {
            help = true;
        } else if (a == "--version") {
            version = true;
        } else if (a == "--json") {
            opt.json = true;
        } else if (a == "--pretty") {
            opt.pretty = true;
        } else if (a == "--transport") {
            if (!value(opt.transport_name)) {
                return false;
            }
        } else if (a == "--reflect") {
            std::string file;
            if (!value(file)) {
                return false;
            }
            opt.reflect_files.push_back(file);
        } else if (a == "--timeout") {
            if (!number(opt.timeout_ms)) {
                return false;
            }
        } else if (a == "--count") {
            if (!number(opt.count)) {
                return false;
            }
        } else if (a == "--duration") {
            if (!number(opt.duration_ms)) {
                return false;
            }
        } else {
            err = "unknown option " + a;
            return false;
        }
    }
    return true;
}

} // namespace

void request_stop() {
    g_stop = 1;
}

int run(const std::vector<std::string> &args, std::istream &in, std::ostream &out,
        std::ostream &err) {
    Io io{in, out, err};
    Options opt;
    std::string command;
    std::string perr;
    bool help = false;
    bool version = false;
    if (!parse_args(args, opt, command, help, version, perr)) {
        err << "qmpctl: " << perr << "\n" << kUsage;
        return kExitUsage;
    }
    if (help) {
        out << kUsage;
        return kExitOk;
    }
    if (opt.transport_name.empty()) {
        opt.transport = qmp_transport_default();
        opt.transport_name = opt.transport->name;
    } else {
        opt.transport = qmp_transport_by_name(opt.transport_name.c_str());
        if (opt.transport == nullptr) {
            err << "qmpctl: transport '" << opt.transport_name
                << "' is not available in this build\n";
            return kExitUsage;
        }
    }
    if (version) {
        out << "qmpctl " << QMP_VERSION_MAJOR << "." << QMP_VERSION_MINOR << "."
            << QMP_VERSION_PATCH << " build=" << QMP_BUILD_ID << " transport=" << opt.transport_name
            << "\n";
        return kExitOk;
    }
    if (opt.timeout_ms > UINT64_MAX / kNsPerMs || opt.duration_ms > UINT64_MAX / kNsPerMs) {
        err << "qmpctl: --timeout/--duration out of range\n";
        return kExitUsage;
    }
    if (command == "list") {
        return cmd_list(opt, io);
    }
    if (command == "describe") {
        return cmd_describe(opt, io);
    }
    if (command == "call") {
        return cmd_call(opt, io);
    }
    if (command == "subscribe") {
        return cmd_subscribe(opt, io);
    }
    if (command.empty()) {
        err << kUsage;
    } else {
        err << "qmpctl: unknown command '" << command << "'\n" << kUsage;
    }
    return kExitUsage;
}

} // namespace qmpctl
