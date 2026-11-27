# SPDX-License-Identifier: Apache-2.0
# qmpctl tests. The codec is checked against the structs that protoc-gen-qmp generated
# for the same proto (the C compiler's layout is the oracle); the commands run in-process
# against real servers on the UDS transport (fake where UDS is not built).
qmp_add_service(NAME kitchen PROTO "${CMAKE_CURRENT_SOURCE_DIR}/kitchen.proto"
                IMPORT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}" SOURCES_VAR _kitchen_src)
add_library(qmpctl_test_kitchen STATIC "${_kitchen_src}")
target_link_libraries(qmpctl_test_kitchen PUBLIC qmp_gen_kitchen)
qmp_set_warnings(qmpctl_test_kitchen)

add_executable(qmpctl_tests
  test_reflect.cpp
  test_codec.cpp
  test_cli.cpp)
target_link_libraries(qmpctl_tests PRIVATE qmpctl_lib qmpctl_test_kitchen qmp_test_joint
                                           GTest::gtest_main)
qmp_set_warnings(qmpctl_tests)
if(CMAKE_CROSSCOMPILING AND NOT CMAKE_CROSSCOMPILING_EMULATOR)
  # Run on the board with scripts/target-run.sh tools/qmpctl/tests/qmpctl_tests.
  add_test(NAME qmpctl_tests COMMAND qmpctl_tests)
else()
  include(GoogleTest)
  gtest_discover_tests(qmpctl_tests DISCOVERY_TIMEOUT 30)
endif()
