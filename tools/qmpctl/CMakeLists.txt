# SPDX-License-Identifier: Apache-2.0
# qmpctl: C++17 command-line tool. Links only the libqmp C ABI (ADR-006) and is driven by
# reflection data alone: no generated code is compiled into it.
include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW) # extracted files get the extraction time: rebuild on URL change
endif()
# nlohmann/json (MIT), header-only, the release's include-only archive pinned by hash.
FetchContent_Declare(nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_library(qmpctl_lib STATIC
  reflect.cpp
  codec.cpp
  discover.cpp
  qmpctl.cpp)
target_include_directories(qmpctl_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
# SYSTEM: the project's -Werror set is for our code, not for a third-party header.
target_include_directories(qmpctl_lib SYSTEM PUBLIC "${nlohmann_json_SOURCE_DIR}/include")
target_link_libraries(qmpctl_lib PUBLIC qmp::qmp)
qmp_set_warnings(qmpctl_lib)
if(CMAKE_SYSTEM_NAME STREQUAL "QNX")
  target_compile_definitions(qmpctl_lib PUBLIC _QNX_SOURCE)
endif()

add_executable(qmpctl main.cpp)
target_link_libraries(qmpctl PRIVATE qmpctl_lib)
qmp_set_warnings(qmpctl)

if(QMP_BUILD_TESTS)
  add_subdirectory(tests)
endif()
