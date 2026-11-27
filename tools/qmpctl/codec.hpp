// SPDX-License-Identifier: Apache-2.0
// JSON <-> qmp wire payload (the generated fixed-layout POD structs), driven only by the
// reflection model. JSON mapping:
//   bool            true/false
//   (u)int32/64     number (a decimal or 0x string is accepted on input)
//   float/double    number; "NaN", "Infinity", "-Infinity" for the non-finite values
//   enum            value name; a number on input, and on output when the value is unnamed
//   string          string (UTF-8 bytes, at most max_len)
//   bytes           base64 string (at most max_len decoded bytes)
//   message         object; repeated -> array (at most max_count items)
// Encoding is strict: unknown keys, wrong types and out-of-range values are errors, never
// silently truncated. Missing keys keep the proto3 default (zero). Padding is zeroed.
#ifndef QMPCTL_CODEC_HPP
#define QMPCTL_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "reflect.hpp"

namespace qmpctl {

// 8-byte aligned payload storage (generated structs are aligned to at most 8).
class Payload {
  public:
    explicit Payload(size_t size = 0) { resize(size); }
    void resize(size_t size) {
        size_ = size;
        words_.assign((size + 7U) / 8U + 1U, 0);
    }
    uint8_t *data() { return reinterpret_cast<uint8_t *>(words_.data()); }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(words_.data()); }
    size_t size() const { return size_; }

  private:
    std::vector<uint64_t> words_;
    size_t size_ = 0;
};

// JSON object -> payload of doc message `message`. A zero-field message accepts {} or null.
bool encode(const Doc &doc, const std::string &message, const Json &in, Payload &out,
            std::string &err);

// Payload -> JSON object. len must equal the message size; lengths and counts inside the
// payload are untrusted and checked against max_len / max_count.
bool decode(const Doc &doc, const std::string &message, const uint8_t *data, size_t len, Json &out,
            std::string &err);

// A JSON object with every field at its default: what `call` expects, for describe.
Json template_of(const Doc &doc, const std::string &message);

std::string base64_encode(const uint8_t *data, size_t len);
bool base64_decode(const std::string &text, std::vector<uint8_t> &out);

} // namespace qmpctl
#endif
