# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)

# The version is defined once, in include/qmp/qmp_version.h (ADR-011).
set(QMP_VERSION_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/include/qmp/qmp_version.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${QMP_VERSION_HEADER}")
foreach(_part MAJOR MINOR PATCH)
  file(STRINGS "${QMP_VERSION_HEADER}" _line REGEX "^#define QMP_VERSION_${_part} [0-9]+$")
  if(NOT _line MATCHES "^#define QMP_VERSION_${_part} ([0-9]+)$")
    message(FATAL_ERROR "QMP_VERSION_${_part} not found in ${QMP_VERSION_HEADER}")
  endif()
  set(_qmp_version_${_part} "${CMAKE_MATCH_1}")
endforeach()

project(qmp VERSION ${_qmp_version_MAJOR}.${_qmp_version_MINOR}.${_qmp_version_PATCH}
        LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QMP_BUILD_TESTS "Build unit and end-to-end tests" ON)
option(QMP_BUILD_TOOLS "Build qmpctl" ON)
option(QMP_BUILD_BENCH "Build benchmarks (W4)" OFF)
option(QMP_BUILD_ADAPTER "Build the uProtocol UTransport adapter (W4, stretch)" OFF)
option(QMP_BUILD_EXAMPLES "Build the joint example (host: fake transport only)" ON)
set(QMP_SANITIZER "none" CACHE STRING "none|asan|ubsan|tsan")
set_property(CACHE QMP_SANITIZER PROPERTY STRINGS none asan ubsan tsan)

if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  set(QMP_ON_QNX ON)
else()
  set(QMP_ON_QNX OFF)
endif()
# ADR-004: UDS is a bench and host-test transport with NO priority inheritance. It is
# built on QNX too (io-sock) so W4 can compare both transports on the same board; native
# message passing stays the default there.
option(QMP_TRANSPORT_UDS "Build the UDS bench-only transport (no priority inheritance)" ON)

include(QmpWarnings)
include(QmpCodegen)

# Build identifier (git hash + dirty flag) and build version (release version plus the
# distance from its tag) regenerated on every build; the script only rewrites the header
# when either changes, so dependents rebuild exactly then.
set(QMP_BUILD_ID_DIR "${CMAKE_BINARY_DIR}/gen/buildid")
set(QMP_BUILD_ID_HEADER "${QMP_BUILD_ID_DIR}/qmp_build_id.h")
add_custom_target(qmp_build_id
  COMMAND ${CMAKE_COMMAND} -DOUT=${QMP_BUILD_ID_HEADER} -DSRC=${PROJECT_SOURCE_DIR}
          -DVERSION=${PROJECT_VERSION}
          -P ${PROJECT_SOURCE_DIR}/cmake/QmpBuildId.cmake
  BYPRODUCTS ${QMP_BUILD_ID_HEADER}
  COMMENT "Refreshing qmp_build_id.h")

if(NOT QMP_SANITIZER STREQUAL "none")
  if(QMP_ON_QNX AND QMP_SANITIZER STREQUAL "tsan")
    message(FATAL_ERROR "tsan is not available on QNX; use the host build (ADR-004)")
  endif()
  if(QMP_SANITIZER STREQUAL "asan")
    set(_qmp_san_flags -fsanitize=address -fno-omit-frame-pointer)
  elseif(QMP_SANITIZER STREQUAL "tsan")
    set(_qmp_san_flags -fsanitize=thread)
  elseif(QMP_SANITIZER STREQUAL "ubsan")
    set(_qmp_san_flags -fsanitize=undefined -fno-sanitize-recover=all)
  else()
    message(FATAL_ERROR "QMP_SANITIZER must be none|asan|ubsan|tsan")
  endif()
  add_compile_options(${_qmp_san_flags} -g)
  add_link_options(${_qmp_san_flags})
endif()

if(QMP_BUILD_TESTS)
  enable_testing()
  include(FetchContent)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
    target_compile_definitions(gtest PUBLIC _QNX_SOURCE)
  endif()
endif()

add_subdirectory(runtime)

if(QMP_BUILD_TOOLS)
  add_subdirectory(tools/qmpctl)
endif()

if(QMP_BUILD_EXAMPLES)
  add_subdirectory(examples/joint_demo)
  add_subdirectory(examples/smoke)
endif()

if(QMP_BUILD_BENCH)
  add_subdirectory(bench)
endif()
