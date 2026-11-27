// SPDX-License-Identifier: Apache-2.0
#include "codec.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace qmpctl {

namespace {

constexpr uint32_t kArrayHeader = 8; // { uint32_t count; uint32_t _pad; }
constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

template <typename T> void put(uint8_t *p, T v) {
    std::memcpy(p, &v, sizeof(v));
}
template <typename T> T get(const uint8_t *p) {
    T v;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

bool fail(std::string &err, const std::string &path, const std::string &what) {
    err = (path.empty() ? std::string("argument") : path) + ": " + what;
    return false;
}

// Integers: JSON numbers, or strings (decimal / 0x hex) for values a shell or another JSON
// producer cannot write as a number.
bool to_i64(const Json &v, int64_t &out) {
    if (v.is_number_unsigned()) {
        uint64_t u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(INT64_MAX)) {
            return false;
        }
        out = static_cast<int64_t>(u);
        return true;
    }
    if (v.is_number_integer()) {
        out = v.get<int64_t>();
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        errno = 0;
        char *end = nullptr;
        long long n = std::strtoll(s.c_str(), &end, 0);
        if (s.empty() || errno != 0 || *end != '\0') {
            return false;
        }
        out = n;
        return true;
    }
    return false;
}

bool to_u64(const Json &v, uint64_t &out) {
    if (v.is_number_unsigned()) {
        out = v.get<uint64_t>();
        return true;
    }
    if (v.is_number_integer()) {
        return false; // negative
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        errno = 0;
        char *end = nullptr;
        if (s.empty() || s[0] == '-' || s[0] == '+' || s[0] == ' ') {
            return false;
        }
        unsigned long long n = std::strtoull(s.c_str(), &end, 0);
        if (errno != 0 || *end != '\0') {
            return false;
        }
        out = n;
        return true;
    }
    return false;
}

bool to_double(const Json &v, double &out) {
    if (v.is_number()) {
        out = v.get<double>();
        return true;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        if (s == "NaN") {
            out = std::numeric_limits<double>::quiet_NaN();
        } else if (s == "Infinity") {
            out = std::numeric_limits<double>::infinity();
        } else if (s == "-Infinity") {
            out = -std::numeric_limits<double>::infinity();
        } else {
            return false;
        }
        return true;
    }
    return false;
}

Json from_double(double d) {
    if (std::isnan(d)) {
        return "NaN";
    }
    if (std::isinf(d)) {
        return d > 0 ? "Infinity" : "-Infinity";
    }
    return d;
}

// A float printed through double shows conversion noise (0.1f -> 0.10000000149011612).
// Emit the shortest decimal that reads back as the same float instead.
Json from_float(float f) {
    if (std::isnan(f) || std::isinf(f)) {
        return from_double(static_cast<double>(f));
    }
    char buf[32];
    for (int prec = 1; prec <= 9; prec++) {
        (void)std::snprintf(buf, sizeof(buf), "%.*g", prec, static_cast<double>(f));
        if (std::strtof(buf, nullptr) == f) {
            break;
        }
    }
    return std::strtod(buf, nullptr);
}

bool encode_message(const Doc &doc, const Message &m, const Json &in, uint8_t *dst,
                    const std::string &path, std::string &err);

bool encode_elem(const Doc &doc, const Field &f, const Json &v, uint8_t *p, const std::string &path,
                 std::string &err) {
    switch (f.kind) {
    case Kind::Bool:
        if (!v.is_boolean()) {
            return fail(err, path, "expected true or false");
        }
        put<uint8_t>(p, v.get<bool>() ? 1 : 0);
        return true;
    case Kind::Int32: {
        int64_t n = 0;
        if (!to_i64(v, n) || n < INT32_MIN || n > INT32_MAX) {
            return fail(err, path, "expected an int32");
        }
        put<int32_t>(p, static_cast<int32_t>(n));
        return true;
    }
    case Kind::UInt32: {
        uint64_t n = 0;
        if (!to_u64(v, n) || n > UINT32_MAX) {
            return fail(err, path, "expected a uint32");
        }
        put<uint32_t>(p, static_cast<uint32_t>(n));
        return true;
    }
    case Kind::Int64: {
        int64_t n = 0;
        if (!to_i64(v, n)) {
            return fail(err, path, "expected an int64");
        }
        put<int64_t>(p, n);
        return true;
    }
    case Kind::UInt64: {
        uint64_t n = 0;
        if (!to_u64(v, n)) {
            return fail(err, path, "expected a uint64");
        }
        put<uint64_t>(p, n);
        return true;
    }
    case Kind::Float: {
        double d = 0;
        if (!to_double(v, d)) {
            return fail(err, path, "expected a number");
        }
        if (std::isfinite(d) &&
            std::fabs(d) > static_cast<double>(std::numeric_limits<float>::max())) {
            return fail(err, path, "out of range for float");
        }
        put<float>(p, static_cast<float>(d));
        return true;
    }
    case Kind::Double: {
        double d = 0;
        if (!to_double(v, d)) {
            return fail(err, path, "expected a number");
        }
        put<double>(p, d);
        return true;
    }
    case Kind::Enum: {
        if (v.is_string()) {
            const std::string name = v.get<std::string>();
            for (const auto &ev : f.enum_values) {
                if (ev.first == name) {
                    put<int32_t>(p, ev.second);
                    return true;
                }
            }
        }
        int64_t n = 0;
        if (!to_i64(v, n) || n < INT32_MIN || n > INT32_MAX) {
            std::string names;
            for (const auto &ev : f.enum_values) {
                names += (names.empty() ? "" : ", ") + ev.first;
            }
            return fail(err, path,
                        "expected one of " + names + " (or an int32) for enum " + f.type_name);
        }
        put<int32_t>(p, static_cast<int32_t>(n));
        return true;
    }
    case Kind::String: {
        if (!v.is_string()) {
            return fail(err, path, "expected a string");
        }
        const std::string s = v.get<std::string>();
        if (s.size() > f.max_len) {
            return fail(err, path,
                        "string is " + std::to_string(s.size()) + " bytes, max_len is " +
                            std::to_string(f.max_len));
        }
        put<uint16_t>(p, static_cast<uint16_t>(s.size()));
        std::memcpy(p + 2, s.data(), s.size());
        return true;
    }
    case Kind::Bytes: {
        std::vector<uint8_t> raw;
        if (!v.is_string() || !base64_decode(v.get<std::string>(), raw)) {
            return fail(err, path, "expected a base64 string");
        }
        if (raw.size() > f.max_len) {
            return fail(err, path,
                        "value is " + std::to_string(raw.size()) + " bytes, max_len is " +
                            std::to_string(f.max_len));
        }
        put<uint16_t>(p, static_cast<uint16_t>(raw.size()));
        if (!raw.empty()) {
            std::memcpy(p + 2, raw.data(), raw.size());
        }
        return true;
    }
    case Kind::Message: {
        const Message *sub = doc.find_message(f.type_name);
        return sub != nullptr && encode_message(doc, *sub, v, p, path, err);
    }
    }
    return fail(err, path, "unsupported field type");
}

bool encode_message(const Doc &doc, const Message &m, const Json &in, uint8_t *dst,
                    const std::string &path, std::string &err) {
    if (!in.is_object()) {
        return fail(err, path, "expected an object for " + m.name);
    }
    for (auto it = in.begin(); it != in.end(); ++it) {
        const Field *f = nullptr;
        for (const Field &cand : m.fields) {
            if (cand.name == it.key()) {
                f = &cand;
            }
        }
        const std::string fpath = path.empty() ? it.key() : path + "." + it.key();
        if (f == nullptr) {
            std::string names;
            for (const Field &cand : m.fields) {
                names += (names.empty() ? "" : ", ") + cand.name;
            }
            return fail(err, fpath,
                        "no such field in " + m.name +
                            (names.empty() ? " (it has no fields)" : " (fields: " + names + ")"));
        }
        uint8_t *p = dst + f->offset;
        if (!f->repeated) {
            if (!encode_elem(doc, *f, it.value(), p, fpath, err)) {
                return false;
            }
            continue;
        }
        if (!it.value().is_array()) {
            return fail(err, fpath, "expected an array");
        }
        if (it.value().size() > f->max_count) {
            return fail(err, fpath,
                        std::to_string(it.value().size()) + " items, max_count is " +
                            std::to_string(f->max_count));
        }
        put<uint32_t>(p, static_cast<uint32_t>(it.value().size()));
        size_t i = 0;
        for (const Json &item : it.value()) {
            if (!encode_elem(doc, *f, item, p + kArrayHeader + i * f->elem_size,
                             fpath + "[" + std::to_string(i) + "]", err)) {
                return false;
            }
            i++;
        }
    }
    return true;
}

bool decode_message(const Doc &doc, const Message &m, const uint8_t *src, Json &out,
                    const std::string &path, std::string &err);

bool decode_elem(const Doc &doc, const Field &f, const uint8_t *p, Json &out,
                 const std::string &path, std::string &err) {
    switch (f.kind) {
    case Kind::Bool:
        out = get<uint8_t>(p) != 0;
        return true;
    case Kind::Int32:
        out = get<int32_t>(p);
        return true;
    case Kind::UInt32:
        out = get<uint32_t>(p);
        return true;
    case Kind::Int64:
        out = get<int64_t>(p);
        return true;
    case Kind::UInt64:
        out = get<uint64_t>(p);
        return true;
    case Kind::Float:
        out = from_float(get<float>(p));
        return true;
    case Kind::Double:
        out = from_double(get<double>(p));
        return true;
    case Kind::Enum: {
        int32_t n = get<int32_t>(p);
        out = n;
        for (const auto &ev : f.enum_values) {
            if (ev.second == n) {
                out = ev.first;
                break;
            }
        }
        return true;
    }
    case Kind::String:
    case Kind::Bytes: {
        uint16_t len = get<uint16_t>(p);
        if (len > f.max_len) {
            return fail(err, path,
                        "length " + std::to_string(len) + " exceeds max_len " +
                            std::to_string(f.max_len) + " (corrupt payload)");
        }
        if (f.kind == Kind::Bytes) {
            out = base64_encode(p + 2, len);
        } else {
            // Not necessarily valid UTF-8: dump() replaces invalid sequences later.
            out = std::string(reinterpret_cast<const char *>(p + 2), len);
        }
        return true;
    }
    case Kind::Message: {
        const Message *sub = doc.find_message(f.type_name);
        return sub != nullptr && decode_message(doc, *sub, p, out, path, err);
    }
    }
    return fail(err, path, "unsupported field type");
}

bool decode_message(const Doc &doc, const Message &m, const uint8_t *src, Json &out,
                    const std::string &path, std::string &err) {
    out = Json::object();
    for (const Field &f : m.fields) {
        const std::string fpath = path.empty() ? f.name : path + "." + f.name;
        const uint8_t *p = src + f.offset;
        if (!f.repeated) {
            Json v;
            if (!decode_elem(doc, f, p, v, fpath, err)) {
                return false;
            }
            out[f.name] = std::move(v);
            continue;
        }
        uint32_t count = get<uint32_t>(p);
        if (count > f.max_count) {
            return fail(err, fpath,
                        "count " + std::to_string(count) + " exceeds max_count " +
                            std::to_string(f.max_count) + " (corrupt payload)");
        }
        Json arr = Json::array();
        for (uint32_t i = 0; i < count; i++) {
            Json v;
            if (!decode_elem(doc, f, p + kArrayHeader + static_cast<size_t>(i) * f.elem_size, v,
                             fpath + "[" + std::to_string(i) + "]", err)) {
                return false;
            }
            arr.push_back(std::move(v));
        }
        out[f.name] = std::move(arr);
    }
    return true;
}

Json template_elem(const Doc &doc, const Field &f) {
    switch (f.kind) {
    case Kind::Bool:
        return false;
    case Kind::Float:
    case Kind::Double:
        return 0.0;
    case Kind::Enum:
        for (const auto &ev : f.enum_values) {
            if (ev.second == 0) {
                return ev.first;
            }
        }
        return 0;
    case Kind::String:
    case Kind::Bytes:
        return "";
    case Kind::Message:
        return template_of(doc, f.type_name);
    default:
        return 0;
    }
}

} // namespace

bool encode(const Doc &doc, const std::string &message, const Json &in, Payload &out,
            std::string &err) {
    const Message *m = doc.find_message(message);
    if (m == nullptr) {
        return fail(err, "", "unknown message '" + message + "'");
    }
    out.resize(m->size);
    if (in.is_null()) {
        return true; // every field at its default
    }
    return encode_message(doc, *m, in, out.data(), "", err);
}

bool decode(const Doc &doc, const std::string &message, const uint8_t *data, size_t len, Json &out,
            std::string &err) {
    const Message *m = doc.find_message(message);
    if (m == nullptr) {
        return fail(err, "", "unknown message '" + message + "'");
    }
    if (len != m->size) {
        return fail(err, message,
                    "payload is " + std::to_string(len) + " bytes, expected " +
                        std::to_string(m->size));
    }
    return decode_message(doc, *m, data, out, "", err);
}

Json template_of(const Doc &doc, const std::string &message) {
    Json out = Json::object();
    const Message *m = doc.find_message(message);
    if (m == nullptr) {
        return out;
    }
    for (const Field &f : m->fields) {
        if (f.repeated) {
            out[f.name] = Json::array();
        } else {
            out[f.name] = template_elem(doc, f);
        }
    }
    return out;
}

std::string base64_encode(const uint8_t *data, size_t len) {
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < len) {
            v |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        if (i + 2 < len) {
            v |= data[i + 2];
        }
        out += kB64[(v >> 18) & 63U];
        out += kB64[(v >> 12) & 63U];
        out += i + 1 < len ? kB64[(v >> 6) & 63U] : '=';
        out += i + 2 < len ? kB64[v & 63U] : '=';
    }
    return out;
}

bool base64_decode(const std::string &text, std::vector<uint8_t> &out) {
    out.clear();
    uint32_t acc = 0;
    int bits = 0;
    size_t pad = 0;
    size_t digits = 0;
    for (char c : text) {
        if (c == '=') {
            pad++;
            continue;
        }
        const char *pos = c != '\0' ? std::strchr(kB64, c) : nullptr;
        if (pos == nullptr || pad != 0) {
            return false; // foreign character, or data after the padding
        }
        digits++;
        acc = (acc << 6) | static_cast<uint32_t>(pos - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFU));
        }
    }
    // Padding is optional, but when present the total must be a multiple of four.
    if (digits % 4 == 1 || pad > 2 || (pad != 0 && (digits + pad) % 4 != 0)) {
        return false;
    }
    return true;
}

} // namespace qmpctl
