# SPDX-License-Identifier: Apache-2.0
add_library(qmp STATIC
  core/hdr.c
  core/version.c
  core/errors.c
  core/client.c
  core/dispatch.c
  core/server.c
  transport/fake/fake_transport.c)
add_library(qmp::qmp ALIAS qmp)
target_include_directories(qmp PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
find_package(Threads REQUIRED)
target_link_libraries(qmp PUBLIC Threads::Threads)
qmp_set_warnings(qmp)
set_target_properties(qmp PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX")
  # clock_gettime / pthread_cond_timedwait need POSIX 2008 on glibc with -std=c11.
  target_compile_definitions(qmp PRIVATE _POSIX_C_SOURCE=200809L)
endif()

if(QMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
