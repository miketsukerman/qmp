#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Copy a cross-compiled binary to the QNX target, run it, and propagate the exit code.
#
# Usage:   scripts/target-run.sh <relative-binary-path> [args...]
# Example: QMP_TARGET=qnxuser@192.168.1.50 scripts/target-run.sh runtime/tests/qmp_core_tests
#
# Environment:
#   QMP_TARGET      required  SSH destination, e.g. qnxuser@192.168.1.50
#   QMP_BUILD_DIR   optional  build tree root (default: build/qnx-aarch64le)
#   QMP_TARGET_DIR  optional  directory on the target (default: /tmp/qmp)

set -eu

if [ "$#" -lt 1 ]; then
    echo "usage: $0 <relative-binary-path> [args...]" >&2
    exit 2
fi

if [ -z "${QMP_TARGET:-}" ]; then
    echo "error: QMP_TARGET is not set (e.g. QMP_TARGET=qnxuser@192.168.1.50)" >&2
    exit 2
fi

QMP_BUILD_DIR="${QMP_BUILD_DIR:-build/qnx-aarch64le}"
QMP_TARGET_DIR="${QMP_TARGET_DIR:-/tmp/qmp}"

rel="$1"
shift

local_bin="$QMP_BUILD_DIR/$rel"
if [ ! -f "$local_bin" ]; then
    echo "error: binary not found: $local_bin (build it first, or set QMP_BUILD_DIR)" >&2
    exit 2
fi

name=$(basename "$rel")
remote_bin="$QMP_TARGET_DIR/$name"

# Client-side expansion into the remote command is intentional (paths are repo-controlled).
# shellcheck disable=SC2029
ssh "$QMP_TARGET" "mkdir -p '$QMP_TARGET_DIR'"
scp -q "$local_bin" "$QMP_TARGET:$remote_bin"
# shellcheck disable=SC2029
ssh "$QMP_TARGET" "chmod +x '$remote_bin' && '$remote_bin' $*"
