# SPDX-License-Identifier: Apache-2.0
# qmp_qnx_probe: QNX-only check of the kernel facts in docs/VERIFY.md, without libqmp.
add_executable(qmp_qnx_probe qnx_probe.c)
qmp_set_warnings(qmp_qnx_probe)
target_compile_definitions(qmp_qnx_probe PRIVATE _QNX_SOURCE)
