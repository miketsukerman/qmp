// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

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

TEST(Version, MacrosAreConsistent) {
    char want[32];
    std::snprintf(want, sizeof(want), "%d.%d.%d", QMP_VERSION_MAJOR, QMP_VERSION_MINOR,
                  QMP_VERSION_PATCH);
    EXPECT_STREQ(QMP_VERSION_STRING, want);
    EXPECT_EQ(QMP_VERSION_HEX,
              (QMP_VERSION_MAJOR << 16) | (QMP_VERSION_MINOR << 8) | QMP_VERSION_PATCH);
    EXPECT_TRUE(QMP_VERSION_AT_LEAST(0, 3, 0));
    EXPECT_TRUE(QMP_VERSION_AT_LEAST(QMP_VERSION_MAJOR, QMP_VERSION_MINOR, QMP_VERSION_PATCH));
    EXPECT_FALSE(QMP_VERSION_AT_LEAST(QMP_VERSION_MAJOR + 1, 0, 0));
    EXPECT_FALSE(QMP_VERSION_AT_LEAST(QMP_VERSION_MAJOR, QMP_VERSION_MINOR, QMP_VERSION_PATCH + 1));
#if !QMP_VERSION_AT_LEAST(0, 3, 0)
#error "QMP_VERSION_AT_LEAST must work in the preprocessor"
#endif
}

TEST(Version, LinkedLibraryMatchesHeaders) {
    EXPECT_STREQ(qmp_version_string(), QMP_VERSION_STRING);
    EXPECT_EQ(qmp_version_number(), static_cast<uint32_t>(QMP_VERSION_HEX));
    EXPECT_EQ(qmp_wire_version(), QMP_WIRE_VERSION);
}

TEST(Version, WireVersionZeroMeansOne) {
    EXPECT_EQ(QMP_WIRE_VERSION, 1);
    EXPECT_EQ(qmp_wire_version_compatible(0), EOK); /* pre-0.3.0 peer: zeroed padding */
    EXPECT_EQ(qmp_wire_version_compatible(QMP_WIRE_VERSION), EOK);
    EXPECT_EQ(qmp_wire_version_compatible(QMP_WIRE_VERSION + 1), ENOTSUP);
    EXPECT_EQ(qmp_wire_version_compatible(255), ENOTSUP);
}

TEST(Version, BuildIdentity) {
    ASSERT_NE(qmp_build_id(), nullptr);
    EXPECT_GT(std::strlen(qmp_build_id()), 0u);
    /* The build version is the release version, alone or followed by "+<build metadata>". */
    const std::string bv = qmp_build_version();
    const std::string v = QMP_VERSION_STRING;
    ASSERT_EQ(bv.compare(0, v.size(), v), 0) << bv;
    if (bv.size() > v.size()) {
        EXPECT_EQ(bv[v.size()], '+') << bv;
        EXPECT_GT(bv.size(), v.size() + 1) << bv;
    }
}

} // namespace
