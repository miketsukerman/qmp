// SPDX-License-Identifier: Apache-2.0
// Reflection model: the generated documents parse, and inconsistent ones are rejected
// before the codec could act on them (a document may come from another process).
#include <gtest/gtest.h>

#include <string>

#include "joint.qmp.h"
#include "kitchen.qmp.h"
#include "reflect.hpp"

namespace {

using qmpctl::Doc;
using qmpctl::Json;

std::string joint_json() {
    return std::string(reinterpret_cast<const char *>(demo_joint_v1_Joint_reflect_json),
                       demo_joint_v1_Joint_reflect_json_len);
}

std::string kitchen_json() {
    return std::string(reinterpret_cast<const char *>(qmp_test_kitchen_v1_Kitchen_reflect_json),
                       qmp_test_kitchen_v1_Kitchen_reflect_json_len);
}

// Parse the Joint document, let the test break one thing, and expect a rejection.
template <typename Fn> std::string broken(Fn mutate) {
    Json j = Json::parse(joint_json());
    mutate(j);
    Doc doc;
    std::string err;
    EXPECT_FALSE(qmpctl::parse_reflection(j.dump(), doc, err));
    return err;
}

TEST(Reflect, JointDocumentMatchesTheGeneratedHeader) {
    Doc doc;
    std::string err;
    ASSERT_TRUE(qmpctl::parse_reflection(joint_json(), doc, err)) << err;
    const qmpctl::Service *svc = doc.find_service(DEMO_JOINT_V1_JOINT_SERVICE_ID, 1);
    ASSERT_NE(svc, nullptr);
    EXPECT_EQ(svc->name, "Joint");
    EXPECT_EQ(svc->service_name, DEMO_JOINT_V1_JOINT_SERVICE_NAME);
    EXPECT_TRUE(svc->matches_name("demo.joint.v1.Joint"));
    EXPECT_FALSE(svc->matches_name("joint"));
    EXPECT_EQ(doc.find_service(DEMO_JOINT_V1_JOINT_SERVICE_ID, 2), nullptr);

    const qmpctl::Method *get = svc->find_method("GetState");
    ASSERT_NE(get, nullptr);
    EXPECT_EQ(get->id, DEMO_JOINT_V1_JOINT_M_GETSTATE);
    EXPECT_EQ(svc->find_method("3"), get);
    EXPECT_EQ(svc->find_method("0x3"), get);
    EXPECT_EQ(svc->find_method("getstate"), nullptr);
    EXPECT_TRUE(svc->find_method("Stop")->oneway);

    const qmpctl::Topic *log = svc->find_topic("StateLog");
    ASSERT_NE(log, nullptr);
    EXPECT_EQ(log->id, DEMO_JOINT_V1_JOINT_T_STATELOG);
    EXPECT_EQ(svc->find_topic("0x8002"), log);
    EXPECT_EQ(log->mode, "QUEUE");
    EXPECT_EQ(log->depth, 8u);

    const qmpctl::Message *st = doc.find_message("demo.joint.v1.JointState");
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(st->size, sizeof(demo_joint_v1_JointState));
    EXPECT_EQ(st->fields.back().name, "mode");
    EXPECT_EQ(st->fields.back().offset, offsetof(demo_joint_v1_JointState, mode));
    EXPECT_EQ(doc.find_message("demo.joint.v1.Empty")->size, 0u);
}

TEST(Reflect, ElementSizesComeFromTheTypeNotFromTheArraySize) {
    Doc doc;
    std::string err;
    ASSERT_TRUE(qmpctl::parse_reflection(kitchen_json(), doc, err)) << err;
    const qmpctl::Message *sink = doc.find_message("qmp.test.kitchen.v1.Sink");
    ASSERT_NE(sink, nullptr);
    ASSERT_EQ(sink->size, sizeof(qmp_test_kitchen_v1_Sink));
    auto field = [&](const char *name) -> const qmpctl::Field & {
        for (const qmpctl::Field &f : sink->fields) {
            if (f.name == name) {
                return f;
            }
        }
        ADD_FAILURE() << "no field " << name;
        return sink->fields[0];
    };
    qmp_test_kitchen_v1_Sink s;
    EXPECT_EQ(field("bits").elem_size, sizeof(s.bits.items[0])); // 1, array padded to 12
    EXPECT_EQ(field("tags").elem_size, sizeof(s.tags.items[0]));
    EXPECT_EQ(field("chunks").elem_size, sizeof(s.chunks.items[0]));
    EXPECT_EQ(field("inners").elem_size, sizeof(s.inners.items[0]));
    EXPECT_EQ(field("reals").elem_size, sizeof(double));
    EXPECT_EQ(field("name").size, sizeof(s.name));
    EXPECT_EQ(field("colors").enum_values.size(), 3u);
}

TEST(Reflect, RejectsMalformedDocuments) {
    Doc doc;
    std::string err;
    EXPECT_FALSE(qmpctl::parse_reflection("", doc, err));
    EXPECT_FALSE(qmpctl::parse_reflection("{", doc, err));
    EXPECT_FALSE(qmpctl::parse_reflection("[]", doc, err));
    EXPECT_FALSE(qmpctl::parse_reflection("{}", doc, err));
    EXPECT_NE(broken([](Json &j) { j["qmp_reflect_version"] = 2; }).find("qmp_reflect_version"),
              std::string::npos);
    broken([](Json &j) { j.erase("messages"); });
    broken([](Json &j) { j["services"][0].erase("service_id"); });
    broken([](Json &j) { j["services"][0]["service_id"] = 70000; });
    broken([](Json &j) { j["services"][0]["methods"][0]["input"] = "demo.Nope"; });
    broken([](Json &j) { j["services"][0]["methods"][0]["id"] = 0x8000; });
    broken([](Json &j) { j["services"][0]["topics"][0]["id"] = 5; });
    broken([](Json &j) { j["services"][0]["topics"][0]["message"] = "demo.joint.v1.Empty"; });
}

TEST(Reflect, RejectsLayoutsThatWouldReadOrWriteOutOfBounds) {
    const char *state = "demo.joint.v1.JointState";
    // A field that ends past its message.
    EXPECT_NE(broken([&](Json &j) {
                  j["messages"][state]["fields"][0]["offset"] = 44;
              }).find("does not fit"),
              std::string::npos);
    EXPECT_NE(broken([&](Json &j) {
                  j["messages"][state]["fields"][0]["offset"] = 4294967295U;
              }).find("does not fit"),
              std::string::npos);
    // A string whose max_len does not fit the space the field claims.
    broken([&](Json &j) { j["messages"][state]["fields"][4]["max_len"] = 17; });
    broken([&](Json &j) { j["messages"][state]["fields"][4]["max_len"] = 0; });
    broken([&](Json &j) { j["messages"][state]["fields"][4].erase("max_len"); });
    // A repeated field whose items overflow the field.
    broken([&](Json &j) {
        Json &f = j["messages"][state]["fields"][0];
        f["repeated"] = true;
        f["max_count"] = 1;
    });
    // Unknown types, and a message that contains itself.
    broken([&](Json &j) { j["messages"][state]["fields"][0]["type"] = "sint32"; });
    broken([&](Json &j) {
        Json &f = j["messages"][state]["fields"][0];
        f["type"] = "message";
        f["type_name"] = "demo.Missing";
    });
    EXPECT_NE(broken([&](Json &j) {
                  Json &f = j["messages"][state]["fields"][0];
                  f["type"] = "message";
                  f["type_name"] = state;
              }).find("recursive"),
              std::string::npos);
    broken([&](Json &j) { j["messages"][state]["size"] = 0; });
    broken([&](Json &j) { j["messages"][state]["size"] = (1U << 20) + 8U; });
}

TEST(Reflect, ResourceIds) {
    uint16_t id = 0;
    EXPECT_TRUE(qmpctl::parse_resource_id("0x8001", id));
    EXPECT_EQ(id, 0x8001);
    EXPECT_TRUE(qmpctl::parse_resource_id("3", id));
    EXPECT_EQ(id, 3);
    EXPECT_FALSE(qmpctl::parse_resource_id("65536", id));
    EXPECT_FALSE(qmpctl::parse_resource_id("-1", id));
    EXPECT_FALSE(qmpctl::parse_resource_id("3x", id));
    EXPECT_FALSE(qmpctl::parse_resource_id("", id));
    EXPECT_FALSE(qmpctl::parse_resource_id("GetState", id));
}

} // namespace
