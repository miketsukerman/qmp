# SPDX-License-Identifier: Apache-2.0
# Raspberry Pi 5 (primary) / Raspberry Pi 4 (secondary) / QNX SDP 8.0:
# qcc -Vgcc_ntoaarch64le, q++ -Vgcc_ntoaarch64le_gpp. No CPU tuning flags (V-QNX-029).
set(QMP_QNX_ARCH aarch64le)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
include("${CMAKE_CURRENT_LIST_DIR}/qnx-common.cmake")
