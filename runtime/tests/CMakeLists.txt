# SPDX-License-Identifier: Apache-2.0
# GoogleTest calls the C API through extern "C" headers (ADR-006 interop check).
qmp_add_service(NAME joint PROTO "${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto"
                SOURCES_VAR _joint_src)
add_library(qmp_test_joint STATIC "${_joint_src}")
target_link_libraries(qmp_test_joint PUBLIC qmp_gen_joint)
qmp_set_warnings(qmp_test_joint)

add_executable(qmp_core_tests
  test_hdr.cpp
  test_errors.cpp
  test_version.cpp
  test_dispatch.cpp
  test_e2e_fake.cpp
  test_lifecycle_fake.cpp
  test_cancel_fake.cpp
  test_topics_fake.cpp
  test_uds.cpp)
target_include_directories(qmp_core_tests PRIVATE "${PROJECT_SOURCE_DIR}/runtime")
target_link_libraries(qmp_core_tests PRIVATE qmp::qmp qmp_test_joint GTest::gtest_main)
qmp_set_warnings(qmp_core_tests)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  # Discovery runs the binary post-build, which cannot work on the build host for a target
  # executable. Register it as one test; run it on the board with scripts/target-run.sh.
  add_test(NAME qmp_core_tests COMMAND qmp_core_tests)
else()
  include(GoogleTest)
  gtest_discover_tests(qmp_core_tests DISCOVERY_TIMEOUT 30)
endif()
