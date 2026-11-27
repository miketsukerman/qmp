# SPDX-License-Identifier: Apache-2.0
# Benchmarks (W4). Built with -DQMP_BUILD_BENCH=ON; run on the target with bench/run.sh.
qmp_add_service(NAME bench_joint PROTO "${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto"
                SOURCES_VAR _bench_joint_src)

add_executable(qmp_bench_prio_inversion prio_inversion.c "${_bench_joint_src}")
target_link_libraries(qmp_bench_prio_inversion PRIVATE qmp_gen_bench_joint)
target_compile_definitions(qmp_bench_prio_inversion PRIVATE _POSIX_C_SOURCE=200809L)
qmp_set_warnings(qmp_bench_prio_inversion)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "QNX" AND QMP_BUILD_TESTS)
  # Functional check only (--no-rt: no realtime priorities, no pinning): the numbers mean
  # something on the QNX target alone.
  add_test(NAME bench_prio_inversion_smoke
           COMMAND qmp_bench_prio_inversion --transport uds --calls 50 --warmup 5 --period-us 200
                   --hog-burst-ms 0 --no-rt)
endif()
