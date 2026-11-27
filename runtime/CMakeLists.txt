# SPDX-License-Identifier: Apache-2.0
add_library(qmp STATIC
  core/hdr.c
  core/version.c
  core/build_id.c
  core/errors.c
  core/client.c
  core/dispatch.c
  core/topics.c
  core/server.c
  core/transport.c
  transport/fake/fake_transport.c)
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  # Native message passing is the default transport on QNX; the fake stays available
  # (and selectable) for tests.
  target_sources(qmp PRIVATE transport/qnx/qnx_transport.c)
endif()
if(QMP_TRANSPORT_UDS)
  # ADR-004: bench and host-test transport, NO priority inheritance. The PUBLIC macro is
  # what makes qmp_transport_uds() visible to consumers, and names what it is for.
  target_sources(qmp PRIVATE transport/uds/uds_transport.c)
  target_compile_definitions(qmp PUBLIC QMP_TRANSPORT_UDS_BENCH_ONLY=1)
  if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    target_link_libraries(qmp PUBLIC socket)
  endif()
endif()
add_library(qmp::qmp ALIAS qmp)
target_include_directories(qmp PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
# qmp_build_id() and qmp_build_version() read the per-build header (see the top-level
# qmp_build_id target); only core/build_id.c includes it.
target_include_directories(qmp PRIVATE "${QMP_BUILD_ID_DIR}")
add_dependencies(qmp qmp_build_id)
find_package(Threads REQUIRED)
target_link_libraries(qmp PUBLIC Threads::Threads)
qmp_set_warnings(qmp)
set_target_properties(qmp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_compile_definitions(qmp PRIVATE _QNX_SOURCE)
else()
  # clock_gettime / pthread_cond_timedwait need POSIX 2008 on glibc with -std=c11.
  target_compile_definitions(qmp PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(QMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
