#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Joint demo on a QNX target: a 500 Hz StateChanged stream that survives kill -9 of the
# server.
#
#   t = 0 s   qmp_joint_server --publish-ms 2 starts; qmp_joint_client monitor prints one
#             line per second: update rate, updates dropped, longest gap between updates
#   t = 4 s   the server is killed with -9 mid-stream
#   t = 6 s   a new server starts; the client reconnects and resubscribes by itself
#   t = 12 s  the monitor ends
#
# PASS needs all of: a steady second at 500 Hz (+-1 %) before the kill, the client noticing
# the server's death, a reconnect to a new server generation, and 500 Hz (+-1 %) again in
# the last second.
#
# Usage:   QMP_TARGET=qnxuser@192.168.1.50 scripts/target-demo.sh [qnx|uds]
#
# Environment:
#   QMP_TARGET      required  SSH destination
#   QMP_BUILD_DIR   optional  build tree root (default: build/qnx-aarch64le)
#   QMP_TARGET_DIR  optional  directory on the target (default: /tmp/qmp)

set -eu

if [ -z "${QMP_TARGET:-}" ]; then
    echo "error: QMP_TARGET is not set (e.g. QMP_TARGET=qnxuser@192.168.1.50)" >&2
    exit 2
fi
TRANSPORT="${1:-qnx}"
QMP_BUILD_DIR="${QMP_BUILD_DIR:-build/qnx-aarch64le}"
QMP_TARGET_DIR="${QMP_TARGET_DIR:-/tmp/qmp}"
JSRV="$QMP_TARGET_DIR/qmp_joint_server"
JCLI="$QMP_TARGET_DIR/qmp_joint_client"
local_jsrv="$QMP_BUILD_DIR/examples/joint_demo/qmp_joint_server"
local_jcli="$QMP_BUILD_DIR/examples/joint_demo/qmp_joint_client"
for f in "$local_jsrv" "$local_jcli"; do
    if [ ! -f "$f" ]; then
        echo "error: binary not found: $f (build the qnx preset with -DQMP_BUILD_EXAMPLES=ON)" >&2
        exit 2
    fi
done

SSH_OPTS="-o LogLevel=ERROR"
# Client-side expansion into remote commands is intentional (paths are repo-controlled).
# shellcheck disable=SC2029,SC2086
t() { ssh $SSH_OPTS "$QMP_TARGET" "$@"; }

PIDS=""
cleanup() {
    if [ -n "$PIDS" ]; then
        t "kill -9 $PIDS 2>/dev/null; true" || true
    fi
}
trap cleanup EXIT INT TERM

t "mkdir -p '$QMP_TARGET_DIR'; slay -f -9 qmp_joint_server qmp_joint_client 2>/dev/null; true" || true
for pair in "$local_jsrv:$JSRV" "$local_jcli:$JCLI"; do
    # shellcheck disable=SC2086
    scp -q $SSH_OPTS "${pair%%:*}" "$QMP_TARGET:${pair##*:}.new"
    t "mv -f '${pair##*:}.new' '${pair##*:}' && chmod +x '${pair##*:}'"
done

start_server() { # $1 = log name
    t "'$JSRV' --transport $TRANSPORT --publish-ms 2 > '$QMP_TARGET_DIR/$1' 2>&1 & echo \$!" | tr -d '\r'
}

echo "=== joint demo: 500 Hz StateChanged over transport=$TRANSPORT, kill -9 at 4 s, restart at 6 s ==="
SPID=$(start_server demo-srv-a.log)
PIDS="$PIDS $SPID"
sleep 1
log="$QMP_TARGET_DIR/demo-monitor.log"
CPID=$(t "'$JCLI' --transport $TRANSPORT monitor state 12 495 > '$log' 2>&1 & echo \$!" | tr -d '\r')
PIDS="$PIDS $CPID"
sleep 4
echo "--- kill -9 server (pid $SPID)"
t "kill -9 $SPID"
sleep 2
SPID=$(start_server demo-srv-b.log)
PIDS="$PIDS $SPID"
echo "--- server restarted (pid $SPID)"

i=0
while [ "$i" -lt 20 ] && ! t "grep -Eq '^(PASS|FAIL) monitor' '$log'"; do
    i=$((i + 1))
    sleep 1
done
t "cat '$log'" || true
t "kill -15 $SPID 2>/dev/null; true" || true

rc=0
check() { # $1 = description, $2 = ERE that must match the monitor log
    if t "grep -Eq '$2' '$log'"; then echo "ok:   $1"; else echo "MISS: $1"; rc=1; fi
}
# 495..505 updates in a one-second window
HZ='rate=(49[5-9]|50[0-5]) Hz'
check "500 Hz before the kill"            "^monitor: t=[23]s $HZ"
check "server death noticed"              'monitor: server gone'
check "reconnected to a new generation"   'qmp_client_reconnect -> 0'
check "500 Hz again in the last second"   "^monitor: t=12s $HZ"
check "monitor passed"                    '^PASS monitor'
if [ "$rc" -eq 0 ]; then echo "DEMO PASSED"; else echo "DEMO FAILED"; fi
exit "$rc"
