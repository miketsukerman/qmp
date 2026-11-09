// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <qmp/qmp.h>

namespace {

TEST(Version, ServicePathFormat) {
    char buf[QMP_NAME_MAX];
    ASSERT_EQ(qmp_service_path(buf, sizeof(buf), 0x0101, 0, 1), EOK);
    EXPECT_STREQ(buf, "qmp/0101-0000/v1");
    ASSERT_EQ(qmp_service_path(buf, sizeof(buf), 0xFFFE, 0xBEEF, 254), EOK);
    EXPECT_STREQ(buf, "qmp/fffe-beef/v254");
}

TEST(Version, ServicePathBounds) {
    char buf[8];
    EXPECT_EQ(qmp_service_path(buf, sizeof(buf), 1, 1, 1), ENAMETOOLONG);
    EXPECT_EQ(qmp_service_path(nullptr, 0, 1, 1, 1), EINVAL);
}

TEST(Version, MajorMustMatchMinorIsAdditive) {
    EXPECT_EQ(qmp_version_compatible(1, 0, 1, 0), EOK);
    EXPECT_EQ(qmp_version_compatible(1, 3, 1, 0), EOK); /* newer server */
    EXPECT_EQ(qmp_version_compatible(1, 0, 1, 3),
              EOK); /* newer client: unknown methods -> ENOENT */
    EXPECT_EQ(qmp_version_compatible(2, 0, 1, 0), ENOTSUP);
    EXPECT_EQ(qmp_version_compatible(1, 0, 2, 0), ENOTSUP);
}

} // namespace
