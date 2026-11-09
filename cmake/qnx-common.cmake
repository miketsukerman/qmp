# SPDX-License-Identifier: Apache-2.0
# Shared by cmake/qnx-aarch64le.cmake and cmake/qnx-x86_64.cmake.
# Requires `source <sdp>/qnxsdp-env.sh` first (sets QNX_HOST / QNX_TARGET).

set(CMAKE_SYSTEM_NAME QNX)
set(CMAKE_SYSTEM_VERSION 8.0)

if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
  message(FATAL_ERROR "QNX_HOST/QNX_TARGET not set: source qnxsdp-env.sh first")
endif()
if(NOT DEFINED QMP_QNX_ARCH)
  message(FATAL_ERROR "QMP_QNX_ARCH must be set by the including toolchain file")
endif()

set(CMAKE_SYSROOT "$ENV{QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH "$ENV{QNX_TARGET}/${QMP_QNX_ARCH}" "$ENV{QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# C: qcc -Vgcc_nto<arch>; C++: q++ -Vgcc_nto<arch>_gpp. CMake's QNX platform module turns
# CMAKE_<LANG>_COMPILER_TARGET into the -V<target> flag.
# VERIFY(qnx8): V-QNX-019 — confirm CMake 3.20+ emits -V correctly with SDP 8.0 qcc/q++.
set(CMAKE_C_COMPILER "$ENV{QNX_HOST}/usr/bin/qcc")
set(CMAKE_C_COMPILER_TARGET "gcc_nto${QMP_QNX_ARCH}")
set(CMAKE_CXX_COMPILER "$ENV{QNX_HOST}/usr/bin/q++")
# VERIFY(qnx8): V-QNX-025 — does the _gpp variant exist on 8.0 and does it select libc++?
# Upstream uProtocol QNX 8.0 profiles use plain gcc_nto<arch>. Override with -DQMP_QNX_CXX_VARIANT.
set(QMP_QNX_CXX_VARIANT "gcc_nto${QMP_QNX_ARCH}_gpp" CACHE STRING "q++ -V variant")
set(CMAKE_CXX_COMPILER_TARGET "${QMP_QNX_CXX_VARIANT}")
