# SPDX-License-Identifier: Apache-2.0
# Standalone server + client over the generated Joint bindings. On the host they use the
# fake transport (pass --fake) and the server has a --selftest ctest; on QNX they talk
# native message passing and are orchestrated by scripts/target-smoke.sh.
qmp_add_service(NAME joint_demo PROTO "${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto"
                SOURCES_VAR _joint_demo_src)

add_executable(qmp_joint_server joint_server.c "${_joint_demo_src}")
target_link_libraries(qmp_joint_server PRIVATE qmp_gen_joint_demo)
qmp_set_warnings(qmp_joint_server)

add_executable(qmp_joint_client joint_client.c "${_joint_demo_src}")
target_link_libraries(qmp_joint_client PRIVATE qmp_gen_joint_demo)
qmp_set_warnings(qmp_joint_client)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_compile_definitions(qmp_joint_server PRIVATE _POSIX_C_SOURCE=200809L)
  target_compile_definitions(qmp_joint_client PRIVATE _POSIX_C_SOURCE=200809L)
  if(QMP_BUILD_TESTS)
    add_test(NAME joint_server_selftest COMMAND qmp_joint_server --selftest)
  endif()
endif()
