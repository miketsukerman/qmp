// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>

#include <qmp/qmp_errors.h>

#include <cerrno>
#include <cstring>

namespace {

TEST(Errors, DesignTableRows) {
    EXPECT_EQ(qmp_errno_to_ucode(0), QMP_UCODE_OK);
    EXPECT_EQ(qmp_errno_to_ucode(ETIMEDOUT), QMP_UCODE_DEADLINE_EXCEEDED);
    EXPECT_EQ(qmp_errno_to_ucode(EINTR), QMP_UCODE_CANCELLED);
    EXPECT_EQ(qmp_errno_to_ucode(ECANCELED), QMP_UCODE_CANCELLED);
    EXPECT_EQ(qmp_errno_to_ucode(EMSGSIZE), QMP_UCODE_INVALID_ARGUMENT);
    EXPECT_EQ(qmp_errno_to_ucode(ERANGE), QMP_UCODE_OUT_OF_RANGE);
    EXPECT_EQ(qmp_errno_to_ucode(ENOENT), QMP_UCODE_NOT_FOUND);
    EXPECT_EQ(qmp_errno_to_ucode(EEXIST), QMP_UCODE_ALREADY_EXISTS);
    EXPECT_EQ(qmp_errno_to_ucode(EPERM), QMP_UCODE_PERMISSION_DENIED);
    EXPECT_EQ(qmp_errno_to_ucode(ENOMEM), QMP_UCODE_RESOURCE_EXHAUSTED);
    EXPECT_EQ(qmp_errno_to_ucode(ENOTSUP), QMP_UCODE_FAILED_PRECONDITION);
    EXPECT_EQ(qmp_errno_to_ucode(ESRCH), QMP_UCODE_UNAVAILABLE);
    EXPECT_EQ(qmp_errno_to_ucode(EBADF), QMP_UCODE_UNAVAILABLE);
    EXPECT_EQ(qmp_errno_to_ucode(ENOSYS), QMP_UCODE_UNIMPLEMENTED);
    EXPECT_EQ(qmp_errno_to_ucode(EIO), QMP_UCODE_UNKNOWN);
}

TEST(Errors, ReverseUsesFirstErrnoOfEachRow) {
    EXPECT_EQ(qmp_ucode_to_errno(QMP_UCODE_DEADLINE_EXCEEDED), ETIMEDOUT);
    EXPECT_EQ(qmp_ucode_to_errno(QMP_UCODE_CANCELLED), EINTR);
    EXPECT_EQ(qmp_ucode_to_errno(QMP_UCODE_UNAVAILABLE), ESRCH);
    EXPECT_EQ(qmp_ucode_to_errno(QMP_UCODE_UNAUTHENTICATED), EACCES);
    EXPECT_EQ(qmp_ucode_to_errno(QMP_UCODE_INTERNAL), EIO);
    EXPECT_EQ(qmp_ucode_to_errno(99), EIO);
}

TEST(Errors, RoundTripIsStableForMappedRows) {
    const int errs[] = {ETIMEDOUT, EINTR,  EINVAL,  ERANGE, ENOENT, EEXIST,
                        EACCES,    EAGAIN, ENOTSUP, ESRCH,  ENOSYS};
    for (int e : errs) {
        EXPECT_EQ(qmp_ucode_to_errno(qmp_errno_to_ucode(e)), e) << strerror(e);
    }
}

TEST(Errors, Names) {
    EXPECT_STREQ(qmp_ucode_name(QMP_UCODE_OK), "OK");
    EXPECT_STREQ(qmp_ucode_name(QMP_UCODE_DEADLINE_EXCEEDED), "DEADLINE_EXCEEDED");
    EXPECT_STREQ(qmp_ucode_name(QMP_UCODE_UNAUTHENTICATED), "UNAUTHENTICATED");
    EXPECT_STREQ(qmp_ucode_name(17), "?");
    EXPECT_STREQ(qmp_ucode_name(-1), "?");
}

} // namespace
