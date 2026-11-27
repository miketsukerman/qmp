#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Run the W4 priority-inversion benchmark on a QNX target over SSH and collect the results.
#
# Six runs of qmp_bench_prio_inversion, every one pinned to a single CPU with `on -C`:
#   qnx-quiet, uds-quiet        no hog: the plain round-trip cost of each transport
#   qnx-hog                     native message passing, priority-15 hog: the server inherits
#                               the caller's priority 30 and the hog cannot delay the call
#   qnx-fixed-hog               negative control: same kernel path without inheritance
#                               (_NTO_CHF_FIXED_PRIORITY)
#   uds-hog                     AF_UNIX transport: no inheritance, the call waits for the hog
#   qnx-fixed-quiet             negative control without the hog
#
# Usage:   QMP_TARGET=qnxuser@192.168.1.50 bench/run.sh [extra benchmark arguments]
# Example: QMP_TARGET=qnxuser@192.168.1.50 bench/run.sh --calls 5000
#
# Environment:
#   QMP_TARGET      required  SSH destination
#   QMP_BUILD_DIR   optional  build tree root (default: build/qnx-aarch64le), configured
#                             with -DQMP_BUILD_BENCH=ON
#   QMP_TARGET_DIR  optional  directory on the target (default: /tmp/qmp)
#   QMP_BENCH_CPU   optional  CPU all threads are pinned to (default: 3)
#   QMP_BENCH_OUT   optional  local results directory (default: bench/results/<build id>)
#
# Output: one "result:" line per run on stdout; summary.txt and the raw per-call latencies
# (<label>.ns, nanoseconds, one per line) in the results directory. bench/plot.py turns
# that directory into a table and a chart. bench/results/ is not tracked by git.

set -eu

if [ -z "${QMP_TARGET:-}" ]; then
    echo "error: QMP_TARGET is not set (e.g. QMP_TARGET=qnxuser@192.168.1.50)" >&2
    exit 2
fi
QMP_BUILD_DIR="${QMP_BUILD_DIR:-build/qnx-aarch64le}"
QMP_TARGET_DIR="${QMP_TARGET_DIR:-/tmp/qmp}"
CPU="${QMP_BENCH_CPU:-3}"
local_bin="$QMP_BUILD_DIR/bench/qmp_bench_prio_inversion"
BIN="$QMP_TARGET_DIR/qmp_bench_prio_inversion"
if [ ! -f "$local_bin" ]; then
    echo "error: binary not found: $local_bin (configure with -DQMP_BUILD_BENCH=ON)" >&2
    exit 2
fi

SSH_OPTS="-o LogLevel=ERROR"
# Client-side expansion into remote commands is intentional (paths are repo-controlled).
# shellcheck disable=SC2029,SC2086
t() { ssh $SSH_OPTS "$QMP_TARGET" "$@"; }

t "mkdir -p '$QMP_TARGET_DIR'; slay -f -9 qmp_bench_prio_inversion 2>/dev/null; true" || true
# shellcheck disable=SC2086
scp -q $SSH_OPTS "$local_bin" "$QMP_TARGET:$BIN.new"
t "mv -f '$BIN.new' '$BIN' && chmod +x '$BIN'"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
fails=0
run_once() { # $1 = label, $2.. = benchmark arguments
    _label=$1
    shift
    t "on -C $CPU '$BIN' --label $_label --dump '$QMP_TARGET_DIR/bench-$_label.ns' $*" \
        > "$tmp/$_label.out" || return 1
    # shellcheck disable=SC2086
    scp -q $SSH_OPTS "$QMP_TARGET:$QMP_TARGET_DIR/bench-$_label.ns" "$tmp/$_label.ns"
}

# A run that fails (the board dropped off the network or restarted) is repeated once, after
# the target answers again; the restart is reported so it does not go unnoticed.
run() {
    if ! run_once "$@"; then
        echo "RETRY: $1 failed at $(date -u +%H:%M:%SZ); waiting for the target" >&2
        _i=0
        while [ "$_i" -lt 30 ] && ! t true 2>/dev/null; do
            _i=$((_i + 1))
            sleep 2
        done
        if ! run_once "$@"; then
            echo "FAILED: $1" >&2
            fails=$((fails + 1))
            return 0
        fi
        retries=$((retries + 1))
    fi
    cat "$tmp/$1.out"
}
retries=0

echo "target: $(t 'uname -srm')  cpu=$CPU"
run qnx-quiet       --transport qnx --hog-burst-ms 0 "$@"
run qnx-fixed-quiet --transport qnx --fixed-priority --hog-burst-ms 0 "$@"
run uds-quiet       --transport uds --hog-burst-ms 0 "$@"
run qnx-hog         --transport qnx "$@"
run qnx-fixed-hog   --transport qnx --fixed-priority "$@"
run uds-hog         --transport uds "$@"

build=$(sed -n 's/.* build=\([^ ]*\) .*/\1/p' "$tmp/qnx-quiet.out" | head -n 1)
out="${QMP_BENCH_OUT:-bench/results/${build:-unknown}}"
mkdir -p "$out"
cat "$tmp"/*.out > "$out/summary.txt"
cp "$tmp"/*.ns "$out/" 2>/dev/null || true
echo "results: $out"
[ "$retries" -eq 0 ] || echo "note: $retries run(s) had to be repeated after the target stopped answering" >&2
exit "$fails"
