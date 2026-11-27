# SPDX-License-Identifier: Apache-2.0
# Standalone server + client over the generated Joint bindings. On the host they talk over
# the UDS transport (two processes) and the server has an in-process --selftest ctest on
# the fake transport; on QNX they talk native message passing (or --transport uds for the
# benchmark comparison) and are orchestrated by scripts/target-smoke.sh.
qmp_add_service(NAME joint_demo PROTO "${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto"
                SOURCES_VAR _joint_demo_src)

add_executable(qmp_joint_server joint_server.c "${_joint_demo_src}")
target_link_libraries(qmp_joint_server PRIVATE qmp_gen_joint_demo)
qmp_set_warnings(qmp_joint_server)

add_executable(qmp_joint_client joint_client.c "${_joint_demo_src}")
target_link_libraries(qmp_joint_client PRIVATE qmp_gen_joint_demo)
qmp_set_warnings(qmp_joint_client)

target_compile_definitions(qmp_joint_server PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_definitions(qmp_joint_client PRIVATE _POSIX_C_SOURCE=200809L)

# Both binaries print qmp_build_version() and qmp_build_id() at startup so the target run can
# be matched to a release and a commit.

if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  if(QMP_BUILD_TESTS)
    add_test(NAME joint_server_selftest COMMAND qmp_joint_server --selftest)
  endif()
endif()
