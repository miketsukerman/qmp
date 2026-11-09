// SPDX-License-Identifier: Apache-2.0
// Pure dispatch decisions on the generated Joint table, no transport involved.
#include <gtest/gtest.h>

#include <qmp/qmp.h>
#include <qmp/qmp_server.h>

#include "core/internal.h"
#include "joint.qmp.h"

namespace {

qmp_hdr_t hdr(uint16_t resource, uint32_t payload_len,
              uint16_t svc = DEMO_JOINT_V1_JOINT_SERVICE_ID, uint16_t inst = 0, uint8_t major = 1) {
    qmp_hdr_t h;
    qmp_hdr_init(&h, svc, inst, resource, major, 0, 0, payload_len);
    return h;
}

TEST(Dispatch, GeneratedTableIsSortedAndSized) {
    ASSERT_EQ(demo_joint_v1_Joint_desc.nmethods, 3u);
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[0].id, DEMO_JOINT_V1_JOINT_M_SETTARGET);
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[0].in_size, sizeof(demo_joint_v1_SetTargetRequest));
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[0].out_size, sizeof(demo_joint_v1_SetTargetReply));
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[1].flags, QMP_MF_ONEWAY);
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[2].in_size, 0u);
    EXPECT_EQ(demo_joint_v1_Joint_desc.methods[2].out_size, sizeof(demo_joint_v1_JointState));
    EXPECT_EQ(demo_joint_v1_Joint_desc.max_msg_size, QMP_HDR_SIZE + 48u);
}

TEST(Dispatch, LookupHappyPath) {
    const qmp_method_desc_t *m = nullptr;
    qmp_hdr_t h = hdr(DEMO_JOINT_V1_JOINT_M_SETTARGET, sizeof(demo_joint_v1_SetTargetRequest));
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), EOK);
    ASSERT_NE(m, nullptr);
    EXPECT_STREQ(m->name, "SetTarget");
}

TEST(Dispatch, LookupErrors) {
    const qmp_method_desc_t *m = nullptr;
    qmp_hdr_t h = hdr(DEMO_JOINT_V1_JOINT_M_GETSTATE, 0, 0x0202);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOENT);
    h = hdr(DEMO_JOINT_V1_JOINT_M_GETSTATE, 0, DEMO_JOINT_V1_JOINT_SERVICE_ID, 7);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOENT);
    h = hdr(DEMO_JOINT_V1_JOINT_M_GETSTATE, 0, DEMO_JOINT_V1_JOINT_SERVICE_ID, 0, 2);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOTSUP);
    h = hdr(0x0042, 0);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOENT);
    h = hdr(DEMO_JOINT_V1_JOINT_T_STATECHANGED, 0);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOENT);
    h = hdr(DEMO_JOINT_V1_JOINT_M_SETTARGET, 4);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), EINVAL);
    h = hdr(DEMO_JOINT_V1_JOINT_M_STOP, 0);
    EXPECT_EQ(qmp_dispatch_lookup(&demo_joint_v1_Joint_desc, 0, &h, &m), ENOSYS);
    EXPECT_EQ(m, nullptr);
}

TEST(Dispatch, GeneratedValidatorBoundsString) {
    demo_joint_v1_JointState st{};
    st.mode.len = 16;
    EXPECT_EQ(demo_joint_v1_JointState_validate(&st), EOK);
    st.mode.len = 17;
    EXPECT_EQ(demo_joint_v1_JointState_validate(&st), ERANGE);
}

TEST(Dispatch, RegisterRejectsUnsortedTable) {
    alignas(8) unsigned char buf[QMP_SERVER_WORKBUF_SIZE(72, 1)];
    qmp_server_t s;
    qmp_server_cfg_t cfg{qmp_transport_fake(), 1, buf, sizeof(buf)};
    ASSERT_EQ(qmp_server_init(&s, &cfg), EOK);
    qmp_method_desc_t bad[2] = {demo_joint_v1_Joint_desc.methods[2],
                                demo_joint_v1_Joint_desc.methods[0]};
    qmp_service_desc_t d = demo_joint_v1_Joint_desc;
    d.methods = bad;
    d.nmethods = 2;
    int dummy = 0;
    EXPECT_EQ(qmp_server_register(&s, &d, 0, &dummy, nullptr), EINVAL);
}

TEST(Dispatch, RegisterRejectsSmallWorkbuf) {
    alignas(8) unsigned char buf[16];
    qmp_server_t s;
    qmp_server_cfg_t cfg{qmp_transport_fake(), 1, buf, sizeof(buf)};
    ASSERT_EQ(qmp_server_init(&s, &cfg), EOK);
    demo_joint_v1_Joint_ops ops{};
    EXPECT_EQ(demo_joint_v1_Joint_register(&s, 0, &ops, nullptr), EMSGSIZE);
}

} // namespace
