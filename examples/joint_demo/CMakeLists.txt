# SPDX-License-Identifier: Apache-2.0
# W1: the C actuator service built against the generated bindings. On the host it uses the
# fake transport and runs a self-test (client in the same process); on QNX (W2) it attaches
# to qmp/0101-0000/v1.
qmp_add_service(NAME joint_demo PROTO "${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto"
                SOURCES_VAR _joint_demo_src)
add_executable(joint_service joint_service.c "${_joint_demo_src}")
target_link_libraries(joint_service PRIVATE qmp_gen_joint_demo)
qmp_set_warnings(joint_service)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_compile_definitions(joint_service PRIVATE _POSIX_C_SOURCE=200809L)
  if(QMP_BUILD_TESTS)
    add_test(NAME joint_service_selftest COMMAND joint_service --selftest)
  endif()
endif()
