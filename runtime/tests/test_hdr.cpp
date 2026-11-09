// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <qmp/qmp.h>

namespace {

qmp_hdr_t request(uint32_t payload_len = 8) {
    qmp_hdr_t h;
    qmp_hdr_init(&h, 0x0101, 0, 1, 1, 0, 0, payload_len);
    return h;
}

TEST(Hdr, InitZeroesReservedAndSetsType) {
    qmp_hdr_t h;
    memset(&h, 0xAA, sizeof(h));
    qmp_hdr_init(&h, 0x0101, 2, 3, 1, 4, QMP_F_ONEWAY, 16);
    EXPECT_EQ(h.type, QMP_MSG_TYPE);
    EXPECT_EQ(h.flags, QMP_F_ONEWAY);
    EXPECT_EQ(h.service_id, 0x0101);
    EXPECT_EQ(h.instance_id, 2);
    EXPECT_EQ(h.resource_id, 3);
    EXPECT_EQ(h.ver_major, 1);
    EXPECT_EQ(h.ver_minor, 4);
    EXPECT_EQ(h.payload_len, 16u);
    EXPECT_EQ(h.status, 0);
    EXPECT_EQ(h.up_priority, 0);
    EXPECT_EQ(h.reserved[0], 0);
    EXPECT_EQ(h.reserved[1], 0);
    EXPECT_EQ(h.reserved[2], 0);
}

TEST(Hdr, ValidateAcceptsExactLength) {
    qmp_hdr_t h = request(8);
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE + 8), EOK);
}

TEST(Hdr, ValidateRejectsLengthMismatch) {
    qmp_hdr_t h = request(8);
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE + 7), EMSGSIZE);
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE + 9), EMSGSIZE);
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE - 1), EMSGSIZE);
}

TEST(Hdr, ValidateRejectsHugePayloadBeforeArithmetic) {
    qmp_hdr_t h = request(QMP_MAX_PAYLOAD + 1);
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE), EMSGSIZE);
}

TEST(Hdr, ValidateRejectsWrongTypeUnknownFlagsAndReplyBit) {
    qmp_hdr_t h = request(0);
    h.type = 0x100; /* an _IO_* message */
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE), EINVAL);
    h = request(0);
    h.flags = 0x8000;
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE), EINVAL);
    h = request(0);
    h.flags = QMP_F_IS_REPLY;
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE), EINVAL);
}

TEST(Hdr, ValidateIgnoresReservedBytes) {
    qmp_hdr_t h = request(0);
    h.reserved[0] = 0xFF;
    h.reserved[2] = 0x7F;
    EXPECT_EQ(qmp_hdr_validate(&h, QMP_HDR_SIZE), EOK);
}

TEST(Hdr, ReplyMirrorsRequestAndFlagsAppError) {
    qmp_hdr_t req = request(8);
    req.up_priority = 5;
    qmp_hdr_t rep;
    qmp_hdr_init_reply(&rep, &req, 0, 48);
    EXPECT_EQ(rep.flags, QMP_F_IS_REPLY);
    EXPECT_EQ(rep.status, 0);
    EXPECT_EQ(rep.payload_len, 48u);
    EXPECT_EQ(rep.up_priority, 5);
    EXPECT_EQ(rep.resource_id, req.resource_id);
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE + 48, 48), EOK);

    qmp_hdr_init_reply(&rep, &req, 11, 0);
    EXPECT_EQ(rep.flags, QMP_F_IS_REPLY | QMP_F_APP_ERROR);
    EXPECT_EQ(rep.status, 11);
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE, 48), EOK);
}

TEST(Hdr, ReplyValidationCatchesMismatchAndOverflow) {
    qmp_hdr_t req = request(8);
    qmp_hdr_t rep;
    qmp_hdr_init_reply(&rep, &req, 0, 8);
    rep.resource_id = 2;
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE + 8, 8), EPROTO);
    qmp_hdr_init_reply(&rep, &req, 0, 16);
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE + 16, 8), EMSGSIZE);
    qmp_hdr_init_reply(&rep, &req, 0, 8);
    rep.flags = 0; /* not a reply */
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE + 8, 8), EPROTO);
    qmp_hdr_init_reply(&rep, &req, 0, 8);
    EXPECT_EQ(qmp_hdr_validate_reply(&rep, &req, QMP_HDR_SIZE + 4, 8), EMSGSIZE);
}

} // namespace
