# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(qmp VERSION 0.1.0 LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(QMP_BUILD_TESTS "Build unit and end-to-end tests" ON)
option(QMP_BUILD_TOOLS "Build qmpctl (W3)" OFF)
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
include(CMakeDependentOption)
# ADR-004: UDS is a host-only bench/test transport (W3).
cmake_dependent_option(QMP_TRANSPORT_UDS "Build the UDS bench-only transport" ON
                       "NOT QMP_ON_QNX" OFF)

include(QmpWarnings)
include(QmpCodegen)

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
endif()

add_subdirectory(runtime)

if(QMP_BUILD_EXAMPLES)
  add_subdirectory(examples/joint_demo)
endif()
