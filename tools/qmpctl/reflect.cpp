// SPDX-License-Identifier: Apache-2.0
#include "reflect.hpp"

#include <cerrno>
#include <cstdlib>
#include <set>

#include <qmp/qmp.h>

namespace qmpctl {

namespace {

constexpr int kMaxNesting = 16;      // generous: the generator stops much earlier
constexpr uint32_t kArrayHeader = 8; // { uint32_t count; uint32_t _pad; } before items[]

bool fail(std::string &err, const std::string &what) {
    err = "reflection data: " + what;
    return false;
}

bool get_str(const Json &o, const char *key, std::string &out) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

bool get_u32(const Json &o, const char *key, uint32_t &out) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_number_integer() || it->is_number_float()) {
        return false;
    }
    if (!it->is_number_unsigned() && it->get<int64_t>() < 0) {
        return false;
    }
    uint64_t v = it->get<uint64_t>();
    if (v > UINT32_MAX) {
        return false;
    }
    out = static_cast<uint32_t>(v);
    return true;
}

bool get_bool(const Json &o, const char *key, bool &out) {
    auto it = o.find(key);
    if (it == o.end() || !it->is_boolean()) {
        return false;
    }
    out = it->get<bool>();
    return true;
}

struct KindInfo {
    const char *word;
    Kind kind;
    uint32_t size; // 0 = computed
};
const KindInfo kKinds[] = {
    {"bool", Kind::Bool, 1},     {"int32", Kind::Int32, 4},     {"uint32", Kind::UInt32, 4},
    {"int64", Kind::Int64, 8},   {"uint64", Kind::UInt64, 8},   {"float", Kind::Float, 4},
    {"double", Kind::Double, 8}, {"enum", Kind::Enum, 4},       {"string", Kind::String, 0},
    {"bytes", Kind::Bytes, 0},   {"message", Kind::Message, 0},
};

bool parse_field(const Json &jf, Field &f, std::string &err) {
    if (!jf.is_object() || !get_str(jf, "name", f.name) || f.name.empty()) {
        return fail(err, "field without a name");
    }
    const std::string where = "field '" + f.name + "'";
    uint32_t number = 0;
    if (!get_str(jf, "type", f.type) || !get_u32(jf, "number", number) ||
        !get_bool(jf, "repeated", f.repeated) || !get_u32(jf, "offset", f.offset) ||
        !get_u32(jf, "size", f.size)) {
        return fail(err, where + ": missing type, number, repeated, offset or size");
    }
    f.number = static_cast<int>(number);
    const KindInfo *ki = nullptr;
    for (const KindInfo &k : kKinds) {
        if (f.type == k.word) {
            ki = &k;
        }
    }
    if (ki == nullptr) {
        return fail(err, where + ": unsupported type '" + f.type + "'");
    }
    f.kind = ki->kind;
    f.elem_size = ki->size;
    if (f.kind == Kind::String || f.kind == Kind::Bytes) {
        if (!get_u32(jf, "max_len", f.max_len) || f.max_len == 0 || f.max_len > UINT16_MAX) {
            return fail(err, where + ": max_len must be 1..65535");
        }
        f.elem_size = (2U + f.max_len + 1U) & ~1U; // { uint16_t len; data[max_len]; } padded to 2
    }
    if (f.kind == Kind::Enum || f.kind == Kind::Message) {
        if (!get_str(jf, "type_name", f.type_name)) {
            return fail(err, where + ": missing type_name");
        }
    }
    if (f.kind == Kind::Enum) {
        auto it = jf.find("values");
        if (it != jf.end() && it->is_object()) {
            for (auto v = it->begin(); v != it->end(); ++v) {
                if (!v.value().is_number_integer()) {
                    return fail(err, where + ": enum value is not an integer");
                }
                int64_t n = v.value().get<int64_t>();
                if (n < INT32_MIN || n > INT32_MAX) {
                    return fail(err, where + ": enum value out of range");
                }
                f.enum_values.emplace_back(v.key(), static_cast<int32_t>(n));
            }
        }
    }
    if (f.repeated) {
        if (!get_u32(jf, "max_count", f.max_count) || f.max_count == 0) {
            return fail(err, where + ": repeated field without max_count");
        }
    }
    return true;
}

// Resolve nested sizes and check that every field lies inside its message.
bool check_message(const Doc &doc, Message &m, std::set<std::string> &path,
                   std::set<std::string> &done, std::string &err) {
    if (path.size() >= static_cast<size_t>(kMaxNesting) || path.count(m.name) != 0) {
        return fail(err, "message '" + m.name + "' is recursive or nested too deeply");
    }
    path.insert(m.name);
    for (Field &f : m.fields) {
        const std::string where = "message '" + m.name + "' field '" + f.name + "'";
        if (f.kind == Kind::Message) {
            auto it = doc.messages.find(f.type_name);
            if (it == doc.messages.end() || it->second.size == 0) {
                return fail(err, where + ": unknown or empty message type '" + f.type_name + "'");
            }
            // Each message is walked once (done), so a wide document cannot blow up the
            // check; a cycle is still caught because its members are never done.
            if (path.count(f.type_name) != 0) {
                return fail(err, where + ": recursive message type '" + f.type_name + "'");
            }
            if (done.count(f.type_name) == 0) {
                Message sub = it->second;
                if (!check_message(doc, sub, path, done, err)) {
                    return false;
                }
            }
            f.elem_size = it->second.size;
        }
        uint64_t need = f.elem_size;
        if (f.repeated) {
            need = kArrayHeader + static_cast<uint64_t>(f.elem_size) * f.max_count;
        }
        if (f.elem_size == 0 || need > f.size ||
            static_cast<uint64_t>(f.offset) + f.size > m.size) {
            return fail(err, where + ": layout does not fit (offset " + std::to_string(f.offset) +
                                 ", size " + std::to_string(f.size) + ", message size " +
                                 std::to_string(m.size) + ")");
        }
    }
    path.erase(m.name);
    done.insert(m.name);
    return true;
}

bool check_payload_type(const Doc &doc, const std::string &name, const std::string &where,
                        std::string &err) {
    if (doc.find_message(name) == nullptr) {
        return fail(err, where + ": unknown message '" + name + "'");
    }
    return true;
}

} // namespace

bool parse_resource_id(const std::string &s, uint16_t &id) {
    if (s.empty() || s[0] < '0' || s[0] > '9') {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (errno != 0 || end == s.c_str() || *end != '\0' || v > UINT16_MAX) {
        return false;
    }
    id = static_cast<uint16_t>(v);
    return true;
}

const Method *Service::find_method(const std::string &name_or_id) const {
    uint16_t id = 0;
    const bool by_id = parse_resource_id(name_or_id, id);
    for (const Method &m : methods) {
        if (by_id ? m.id == id : m.name == name_or_id) {
            return &m;
        }
    }
    return nullptr;
}

const Topic *Service::find_topic(const std::string &name_or_id) const {
    uint16_t id = 0;
    const bool by_id = parse_resource_id(name_or_id, id);
    for (const Topic &t : topics) {
        if (by_id ? t.id == id : t.name == name_or_id) {
            return &t;
        }
    }
    return nullptr;
}

bool Service::matches_name(const std::string &n) const {
    return n == name || n == full_name || n == service_name;
}

const Message *Doc::find_message(const std::string &full_name) const {
    auto it = messages.find(full_name);
    return it == messages.end() ? nullptr : &it->second;
}

const Service *Doc::find_service(uint16_t service_id, uint8_t ver_major) const {
    for (const Service &s : services) {
        if (s.service_id == service_id && s.ver_major == ver_major) {
            return &s;
        }
    }
    return nullptr;
}

bool parse_reflection(const std::string &text, Doc &out, std::string &err) {
    Doc doc;
    doc.raw = Json::parse(text, nullptr, false);
    if (doc.raw.is_discarded() || !doc.raw.is_object()) {
        return fail(err, "not a JSON object");
    }
    uint32_t version = 0;
    if (!get_u32(doc.raw, "qmp_reflect_version", version) || version != kReflectVersion) {
        return fail(err, "unsupported qmp_reflect_version (this qmpctl understands " +
                             std::to_string(kReflectVersion) + ")");
    }
    auto jm = doc.raw.find("messages");
    auto js = doc.raw.find("services");
    if (jm == doc.raw.end() || !jm->is_object() || js == doc.raw.end() || !js->is_array()) {
        return fail(err, "missing 'messages' or 'services'");
    }
    for (auto it = jm->begin(); it != jm->end(); ++it) {
        Message m;
        m.name = it.key();
        auto jf = it.value().find("fields");
        if (!it.value().is_object() || !get_u32(it.value(), "size", m.size) ||
            m.size > QMP_MAX_PAYLOAD || jf == it.value().end() || !jf->is_array()) {
            return fail(err, "message '" + m.name + "': missing or invalid size/fields");
        }
        for (const Json &f : *jf) {
            Field field;
            if (!parse_field(f, field, err)) {
                err += " (in message '" + m.name + "')";
                return false;
            }
            m.fields.push_back(std::move(field));
        }
        if ((m.size == 0) != m.fields.empty()) {
            return fail(err, "message '" + m.name + "': size and field count disagree");
        }
        doc.messages.emplace(m.name, std::move(m));
    }
    std::set<std::string> done;
    for (auto &kv : doc.messages) {
        std::set<std::string> path;
        if (!check_message(doc, kv.second, path, done, err)) {
            return false;
        }
    }
    for (const Json &s : *js) {
        Service svc;
        uint32_t id = 0;
        uint32_t major = 0;
        uint32_t minor = 0;
        auto jv = s.is_object() ? s.find("version") : s.end();
        if (!s.is_object() || !get_str(s, "name", svc.name) || !get_u32(s, "service_id", id) ||
            id > UINT16_MAX || jv == s.end() || !jv->is_object() || !get_u32(*jv, "major", major) ||
            !get_u32(*jv, "minor", minor) || major > UINT8_MAX || minor > UINT8_MAX) {
            return fail(err, "service without name, service_id or version");
        }
        svc.service_id = static_cast<uint16_t>(id);
        svc.ver_major = static_cast<uint8_t>(major);
        svc.ver_minor = static_cast<uint8_t>(minor);
        (void)get_str(s, "full_name", svc.full_name);
        (void)get_str(s, "service_name", svc.service_name);
        (void)get_str(s, "uuri", svc.uuri);
        (void)get_str(s, "qnx_path", svc.qnx_path);
        auto jmeth = s.find("methods");
        if (jmeth != s.end() && jmeth->is_array()) {
            for (const Json &jmd : *jmeth) {
                Method m;
                uint32_t mid = 0;
                if (!jmd.is_object() || !get_str(jmd, "name", m.name) || !get_u32(jmd, "id", mid) ||
                    mid > 0x7FFFU || !get_str(jmd, "input", m.input) ||
                    !get_str(jmd, "output", m.output)) {
                    return fail(err, "service '" + svc.name + "': malformed method");
                }
                m.id = static_cast<uint16_t>(mid);
                (void)get_bool(jmd, "oneway", m.oneway);
                (void)get_str(jmd, "uuri", m.uuri);
                const std::string where = "method '" + svc.name + "." + m.name + "'";
                if (!check_payload_type(doc, m.input, where, err) ||
                    !check_payload_type(doc, m.output, where, err)) {
                    return false;
                }
                svc.methods.push_back(std::move(m));
            }
        }
        auto jtop = s.find("topics");
        if (jtop != s.end() && jtop->is_array()) {
            for (const Json &jt : *jtop) {
                Topic t;
                uint32_t tid = 0;
                if (!jt.is_object() || !get_str(jt, "name", t.name) || !get_u32(jt, "id", tid) ||
                    tid < 0x8000U || tid > UINT16_MAX || !get_str(jt, "message", t.message)) {
                    return fail(err, "service '" + svc.name + "': malformed topic");
                }
                t.id = static_cast<uint16_t>(tid);
                (void)get_str(jt, "mode", t.mode);
                (void)get_u32(jt, "depth", t.depth);
                (void)get_bool(jt, "notification", t.notification);
                (void)get_str(jt, "uuri", t.uuri);
                const Message *tm = doc.find_message(t.message);
                if (tm == nullptr || tm->size == 0) {
                    return fail(err, "topic '" + svc.name + "." + t.name +
                                         "': unknown or empty message '" + t.message + "'");
                }
                svc.topics.push_back(std::move(t));
            }
        }
        doc.services.push_back(std::move(svc));
    }
    out = std::move(doc);
    return true;
}

} // namespace qmpctl
