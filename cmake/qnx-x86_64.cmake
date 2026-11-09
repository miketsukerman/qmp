# SPDX-License-Identifier: Apache-2.0
# x86_64 QEMU/VM / QNX SDP 8.0: qcc -Vgcc_ntox86_64, q++ -Vgcc_ntox86_64_gpp
set(QMP_QNX_ARCH x86_64)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
include("${CMAKE_CURRENT_LIST_DIR}/qnx-common.cmake")
