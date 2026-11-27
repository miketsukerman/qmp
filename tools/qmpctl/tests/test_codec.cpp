// SPDX-License-Identifier: Apache-2.0
// JSON <-> payload codec, checked against the structs protoc-gen-qmp generated for the
// same proto: the bytes qmpctl builds from reflection data alone must be exactly what a
// compiled client would send.
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>

#include "codec.hpp"
#include "kitchen.qmp.h"

namespace {

using qmpctl::Doc;
using qmpctl::Json;
using qmpctl::Payload;

const char kSink[] = "qmp.test.kitchen.v1.Sink";
const char kInner[] = "qmp.test.kitchen.v1.Inner";

const char kFull[] = R"({
  "flag": true, "i32": -5, "u32": 4000000000, "i64": -9000000000,
  "u64": 18446744073709551615, "f": 0.1, "d": 2.5, "color": "COLOR_BLUE",
  "name": "hello", "blob": "AQID", "inner": {"a": 7, "tag": "in"},
  "nums": [1, 2, 3], "tags": ["a", "bcd"],
  "inners": [{"a": 1, "tag": "x"}, {"a": 2, "tag": "yyyyy"}],
  "bits": [true], "colors": ["COLOR_RED", 7], "chunks": ["/w==", "AAE="],
  "reals": [1.5, -0.25]
})";

class Codec : public ::testing::Test {
  protected:
    Doc doc_;
    void SetUp() override {
        std::string err;
        ASSERT_TRUE(qmpctl::parse_reflection(
            std::string(reinterpret_cast<const char *>(qmp_test_kitchen_v1_Kitchen_reflect_json),
                        qmp_test_kitchen_v1_Kitchen_reflect_json_len),
            doc_, err))
            << err;
    }

    static void set(qmp_test_kitchen_v1_Inner &in, int32_t a, const char *tag) {
        in.a = a;
        in.tag.len = static_cast<uint16_t>(std::strlen(tag));
        std::memcpy(in.tag.data, tag, in.tag.len);
    }

    static qmp_test_kitchen_v1_Sink full_struct() {
        qmp_test_kitchen_v1_Sink s;
        std::memset(&s, 0, sizeof(s));
        s.flag = 1;
        s.i32 = -5;
        s.u32 = 4000000000U;
        s.i64 = -9000000000LL;
        s.u64 = UINT64_MAX;
        s.f = 0.1F;
        s.d = 2.5;
        s.color = 5; // COLOR_BLUE
        s.name.len = 5;
        std::memcpy(s.name.data, "hello", 5);
        s.blob.len = 3;
        s.blob.data[0] = 1;
        s.blob.data[1] = 2;
        s.blob.data[2] = 3;
        set(s.inner, 7, "in");
        s.nums.count = 3;
        s.nums.items[0] = 1;
        s.nums.items[1] = 2;
        s.nums.items[2] = 3;
        s.tags.count = 2;
        s.tags.items[0].len = 1;
        s.tags.items[0].data[0] = 'a';
        s.tags.items[1].len = 3;
        std::memcpy(s.tags.items[1].data, "bcd", 3);
        s.inners.count = 2;
        set(s.inners.items[0], 1, "x");
        set(s.inners.items[1], 2, "yyyyy");
        s.bits.count = 1;
        s.bits.items[0] = 1;
        s.colors.count = 2;
        s.colors.items[0] = 1; // COLOR_RED
        s.colors.items[1] = 7; // a value the enum does not name
        s.chunks.count = 2;
        s.chunks.items[0].len = 1;
        s.chunks.items[0].data[0] = 0xFF;
        s.chunks.items[1].len = 2;
        s.chunks.items[1].data[1] = 1;
        s.reals.count = 2;
        s.reals.items[0] = 1.5;
        s.reals.items[1] = -0.25;
        return s;
    }

    // Encode `json` as a Sink and expect failure; returns the message.
    std::string bad(const char *json) {
        Payload p;
        std::string err;
        EXPECT_FALSE(qmpctl::encode(doc_, kSink, Json::parse(json), p, err)) << json;
        return err;
    }
};

TEST_F(Codec, EncodeProducesExactlyTheGeneratedStruct) {
    Payload p;
    std::string err;
    ASSERT_TRUE(qmpctl::encode(doc_, kSink, Json::parse(kFull), p, err)) << err;
    const qmp_test_kitchen_v1_Sink want = full_struct();
    ASSERT_EQ(p.size(), sizeof(want));
    EXPECT_EQ(std::memcmp(p.data(), &want, sizeof(want)), 0);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p.data()) % 8, 0u);
    // What qmpctl builds passes the server's generated bounds check.
    EXPECT_EQ(qmp_test_kitchen_v1_Sink_validate(p.data()), EOK);
}

TEST_F(Codec, DecodeReadsTheGeneratedStruct) {
    const qmp_test_kitchen_v1_Sink s = full_struct();
    Json got;
    std::string err;
    ASSERT_TRUE(
        qmpctl::decode(doc_, kSink, reinterpret_cast<const uint8_t *>(&s), sizeof(s), got, err))
        << err;
    EXPECT_EQ(got, Json::parse(kFull)) << got.dump(2);
    // 0.1f prints as 0.1, not as the double nearest to the float.
    EXPECT_NE(got.dump().find("\"f\":0.1,"), std::string::npos) << got.dump();
}

TEST_F(Codec, MissingFieldsKeepDefaultsAndNullMeansAllDefaults) {
    qmp_test_kitchen_v1_Sink zero;
    std::memset(&zero, 0, sizeof(zero));
    Payload p;
    std::string err;
    ASSERT_TRUE(qmpctl::encode(doc_, kSink, Json::object(), p, err)) << err;
    EXPECT_EQ(std::memcmp(p.data(), &zero, sizeof(zero)), 0);
    ASSERT_TRUE(qmpctl::encode(doc_, kSink, nullptr, p, err)) << err;
    EXPECT_EQ(std::memcmp(p.data(), &zero, sizeof(zero)), 0);
    zero.i32 = 9;
    ASSERT_TRUE(qmpctl::encode(doc_, kSink, Json::parse(R"({"i32": 9})"), p, err)) << err;
    EXPECT_EQ(std::memcmp(p.data(), &zero, sizeof(zero)), 0);

    Json got;
    std::memset(&zero, 0, sizeof(zero));
    ASSERT_TRUE(qmpctl::decode(doc_, kSink, reinterpret_cast<const uint8_t *>(&zero), sizeof(zero),
                               got, err));
    EXPECT_EQ(got, qmpctl::template_of(doc_, kSink));
    EXPECT_EQ(got["color"], "COLOR_UNSPECIFIED");
    EXPECT_EQ(got["nums"], Json::array());
}

TEST_F(Codec, ZeroFieldMessageHasNoPayload) {
    Payload p(8);
    std::string err;
    ASSERT_TRUE(qmpctl::encode(doc_, "qmp.test.kitchen.v1.Empty", Json::object(), p, err)) << err;
    EXPECT_EQ(p.size(), 0u);
    EXPECT_FALSE(
        qmpctl::encode(doc_, "qmp.test.kitchen.v1.Empty", Json::parse(R"({"x": 1})"), p, err));
    Json got;
    ASSERT_TRUE(qmpctl::decode(doc_, "qmp.test.kitchen.v1.Empty", nullptr, 0, got, err)) << err;
    EXPECT_EQ(got, Json::object());
}

TEST_F(Codec, IntegersAcceptStringsAndRejectOverflow) {
    Payload p;
    std::string err;
    ASSERT_TRUE(qmpctl::encode(
        doc_, kSink,
        Json::parse(R"({"u64": "0xffffffffffffffff", "i64": "-12", "u32": "7", "i32": "-0x10"})"),
        p, err))
        << err;
    qmp_test_kitchen_v1_Sink s;
    std::memcpy(&s, p.data(), sizeof(s));
    EXPECT_EQ(s.u64, UINT64_MAX);
    EXPECT_EQ(s.i64, -12);
    EXPECT_EQ(s.u32, 7u);
    EXPECT_EQ(s.i32, -16);

    EXPECT_NE(bad(R"({"i32": 2147483648})").find("i32: expected an int32"), std::string::npos);
    bad(R"({"i32": -2147483649})");
    bad(R"({"i32": 1.5})");
    bad(R"({"i32": true})");
    bad(R"({"i32": "seven"})");
    bad(R"({"i32": ""})");
    bad(R"({"u32": -1})");
    bad(R"({"u32": 4294967296})");
    bad(R"({"u32": "-1"})");
    bad(R"({"u64": -1})");
    bad(R"({"u64": "18446744073709551616"})");
    bad(R"({"i64": 9223372036854775808})");
    bad(R"({"flag": 1})");
    bad(R"({"f": "fast"})");
    bad(R"({"f": 1e39})");
}

TEST_F(Codec, NonFiniteFloatsRoundTripAsStrings) {
    Payload p;
    std::string err;
    ASSERT_TRUE(qmpctl::encode(
        doc_, kSink, Json::parse(R"({"f": "NaN", "d": "-Infinity", "reals": ["Infinity", 1e308]})"),
        p, err))
        << err;
    qmp_test_kitchen_v1_Sink s;
    std::memcpy(&s, p.data(), sizeof(s));
    EXPECT_TRUE(std::isnan(s.f));
    EXPECT_TRUE(std::isinf(s.d) && s.d < 0);
    EXPECT_TRUE(std::isinf(s.reals.items[0]));
    Json got;
    ASSERT_TRUE(qmpctl::decode(doc_, kSink, p.data(), p.size(), got, err));
    EXPECT_EQ(got["f"], "NaN");
    EXPECT_EQ(got["d"], "-Infinity");
    EXPECT_EQ(got["reals"][0], "Infinity");
    EXPECT_EQ(got["reals"][1], 1e308);
}

TEST_F(Codec, BoundsAreErrorsNeverTruncation) {
    EXPECT_NE(bad(R"({"name": "thirteen char"})").find("max_len is 12"), std::string::npos);
    EXPECT_NE(bad(R"({"blob": "AAAAAAAAAAA="})").find("max_len is 6"), std::string::npos);
    EXPECT_NE(bad(R"({"nums": [1, 2, 3, 4, 5]})").find("max_count is 4"), std::string::npos);
    EXPECT_NE(bad(R"({"tags": ["ok", "long"]})").find("tags[1]"), std::string::npos);
    EXPECT_NE(bad(R"({"inners": [{"a": 1}, {"tag": "sixsix"}]})").find("inners[1].tag"),
              std::string::npos);
    EXPECT_NE(bad(R"({"inner": {"b": 1}})").find("inner.b: no such field"), std::string::npos);
    EXPECT_NE(bad(R"({"nope": 1})").find("fields: flag, i32"), std::string::npos);
    bad(R"({"nums": 1})");
    bad(R"({"inner": 5})");
    bad(R"({"blob": "not base64!"})");
    EXPECT_NE(bad(R"({"color": "COLOR_GREEN"})").find("COLOR_RED"), std::string::npos);
    bad(R"({"color": 2147483648})");
    Payload p;
    std::string err;
    EXPECT_FALSE(qmpctl::encode(doc_, kSink, Json::parse("[1]"), p, err));
    EXPECT_FALSE(qmpctl::encode(doc_, "qmp.test.Nope", Json::object(), p, err));
    // Exactly at the bound is fine.
    EXPECT_TRUE(qmpctl::encode(
        doc_, kSink, Json::parse(R"({"name": "twelve chars", "blob": "AAAAAAAA"})"), p, err))
        << err;
}

TEST_F(Codec, DecodeDistrustsLengthsInsideThePayload) {
    qmp_test_kitchen_v1_Sink s = full_struct();
    Json got;
    std::string err;
    const auto *raw = reinterpret_cast<const uint8_t *>(&s);
    s.name.len = 13;
    EXPECT_FALSE(qmpctl::decode(doc_, kSink, raw, sizeof(s), got, err));
    EXPECT_NE(err.find("name"), std::string::npos) << err;
    s = full_struct();
    s.nums.count = 5;
    EXPECT_FALSE(qmpctl::decode(doc_, kSink, raw, sizeof(s), got, err));
    s = full_struct();
    s.inners.items[1].tag.len = 6;
    EXPECT_FALSE(qmpctl::decode(doc_, kSink, raw, sizeof(s), got, err));
    EXPECT_NE(err.find("inners[1].tag"), std::string::npos) << err;
    s = full_struct();
    EXPECT_FALSE(qmpctl::decode(doc_, kSink, raw, sizeof(s) - 8, got, err));
    EXPECT_FALSE(qmpctl::decode(doc_, kInner, raw, sizeof(s), got, err));
}

TEST_F(Codec, StringsAreBytesAndSurviveInvalidUtf8) {
    qmp_test_kitchen_v1_Inner in;
    std::memset(&in, 0, sizeof(in));
    in.tag.len = 3;
    in.tag.data[0] = 'a';
    in.tag.data[1] = static_cast<char>(0xFF);
    in.tag.data[2] = '\0';
    Json got;
    std::string err;
    ASSERT_TRUE(
        qmpctl::decode(doc_, kInner, reinterpret_cast<const uint8_t *>(&in), sizeof(in), got, err));
    EXPECT_EQ(got["tag"].get<std::string>().size(), 3u);
    // Printing must not throw on the invalid byte.
    EXPECT_NO_THROW((void)got.dump(-1, ' ', false, Json::error_handler_t::replace));
}

TEST(Base64, RoundTripAndRejects) {
    const uint8_t data[] = {0x00, 0xFF, 0x10, 0x20, 0x7F};
    for (size_t n = 0; n <= sizeof(data); n++) {
        const std::string text = qmpctl::base64_encode(data, n);
        EXPECT_EQ(text.size() % 4, 0u);
        std::vector<uint8_t> back;
        ASSERT_TRUE(qmpctl::base64_decode(text, back)) << text;
        EXPECT_EQ(back, std::vector<uint8_t>(data, data + n));
    }
    std::vector<uint8_t> out;
    EXPECT_EQ(qmpctl::base64_encode(data, 2), "AP8=");
    EXPECT_TRUE(qmpctl::base64_decode("AP8", out)); // padding is optional
    EXPECT_EQ(out.size(), 2u);
    EXPECT_FALSE(qmpctl::base64_decode("A", out));
    EXPECT_FALSE(qmpctl::base64_decode("AP8=A", out));
    EXPECT_FALSE(qmpctl::base64_decode("AP=", out));
    EXPECT_FALSE(qmpctl::base64_decode("A P8", out));
    EXPECT_FALSE(qmpctl::base64_decode("AP8===", out));
}

} // namespace
