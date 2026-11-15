# SPDX-License-Identifier: Apache-2.0
# Target smoke tests for the native transport (two processes, kill -9, reconnect).
# Compiles on the host too (fake transport) so it cannot bit-rot, but the scenarios only
# make sense on a QNX target: run them with scripts/target-smoke.sh.
add_executable(qmp_smoke smoke.c)
target_link_libraries(qmp_smoke PRIVATE qmp::qmp)
qmp_set_warnings(qmp_smoke)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_compile_definitions(qmp_smoke PRIVATE _POSIX_C_SOURCE=200809L)
endif()
