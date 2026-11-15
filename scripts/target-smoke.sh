#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Orchestrate the two-process smoke scenarios on a QNX target over SSH: timeout with a
# server that never replies, kill -9 of a blocked client, kill -9 of the server with
# restart + reconnect, and a deferred reply racing a client timeout.
#
# Usage:   scripts/target-smoke.sh
# Example: QMP_TARGET=qnxuser@192.168.1.50 scripts/target-smoke.sh
#
# Environment:
#   QMP_TARGET      required  SSH destination, e.g. qnxuser@192.168.1.50
#   QMP_BUILD_DIR   optional  build tree root (default: build/qnx-aarch64le)
#   QMP_TARGET_DIR  optional  directory on the target (default: /tmp/qmp)

set -eu

if [ -z "${QMP_TARGET:-}" ]; then
    echo "error: QMP_TARGET is not set (e.g. QMP_TARGET=qnxuser@192.168.1.50)" >&2
    exit 2
fi

QMP_BUILD_DIR="${QMP_BUILD_DIR:-build/qnx-aarch64le}"
QMP_TARGET_DIR="${QMP_TARGET_DIR:-/tmp/qmp}"
BIN="$QMP_TARGET_DIR/qmp_smoke"
local_bin="$QMP_BUILD_DIR/examples/smoke/qmp_smoke"

if [ ! -f "$local_bin" ]; then
    echo "error: binary not found: $local_bin (build the qnx preset first)" >&2
    exit 2
fi

# Client-side expansion into remote commands is intentional (paths are repo-controlled).
# shellcheck disable=SC2029
t() { ssh "$QMP_TARGET" "$@"; }

t "mkdir -p '$QMP_TARGET_DIR'"
scp -q "$local_bin" "$QMP_TARGET:$BIN"
t "chmod +x '$BIN'"

fails=0
note() { printf '\n=== %s ===\n' "$1"; }
result() { # $1 = name, $2 = rc
    if [ "$2" -eq 0 ]; then echo "--- PASS: $1"; else echo "--- FAIL: $1 (rc=$2)"; fails=$((fails + 1)); fi
}

start_server() {
    t "'$BIN' server > '$QMP_TARGET_DIR/server.log' 2>&1 & echo \$!" | tr -d '\r'
    # Give name_attach a moment.
    sleep 1
}

stop_server() { # $1 = pid
    t "kill -9 $1 2>/dev/null || true"
    sleep 1
}

note "scenario: timeout (server never replies; parked request cancelled)"
SPID=$(start_server)
rc=0; t "'$BIN' timeout" || rc=$?
result timeout "$rc"
stop_server "$SPID"

note "scenario: kill -9 of a client blocked in a call"
SPID=$(start_server)
CPID=$(t "'$BIN' block > '$QMP_TARGET_DIR/block.log' 2>&1 & echo \$!" | tr -d '\r')
sleep 1
t "kill -9 $CPID 2>/dev/null || true"
sleep 1
# The server must have absorbed the disconnect pulse and still answer.
rc=0; t "'$BIN' echo" || rc=$?
result kill-client "$rc"
stop_server "$SPID"

note "scenario: kill -9 of the server mid-call, restart, reconnect"
SPID=$(start_server)
t "'$BIN' reconnect > '$QMP_TARGET_DIR/reconnect.log' 2>&1 &" &
sleep 2                       # the client is REPLY-blocked in the slow call now
t "kill -9 $SPID"
sleep 1
SPID=$(start_server)          # restart; the client's backoff should find it
# Each ssh is a new shell, so poll the log instead of wait(1).
i=0
while [ "$i" -lt 30 ] && ! t "grep -qE '^(PASS|FAIL) reconnect' '$QMP_TARGET_DIR/reconnect.log'"; do
    i=$((i + 1))
    sleep 1
done
wait || true
rc=0; t "grep -q '^PASS reconnect' '$QMP_TARGET_DIR/reconnect.log'" || rc=$?
t "cat '$QMP_TARGET_DIR/reconnect.log'" || true
result reconnect "$rc"
stop_server "$SPID"

note "scenario: deferred reply racing a client timeout"
SPID=$(start_server)
rc=0; t "'$BIN' race" || rc=$?
result race "$rc"
stop_server "$SPID"

echo
if [ "$fails" -eq 0 ]; then
    echo "ALL SMOKE SCENARIOS PASSED"
else
    echo "$fails SMOKE SCENARIO(S) FAILED"
fi
exit "$fails"
