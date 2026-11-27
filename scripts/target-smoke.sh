#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Orchestrate the two-process smoke scenarios on a QNX target over SSH.
#
# Part 1 (qmp_smoke, hand-written service): timeout with a server that never replies,
# kill -9 of a blocked client, kill -9 of the server with restart + reconnect, a
# deferred reply racing a client timeout, and a timeout while every worker is busy.
# Part 2 (qmp_joint_server / qmp_joint_client, generated Joint bindings): latency loop
# against a quiet server, pidin/kernel-state capture during an in-flight call, client
# death, server death (mid-call with reconnect, and a call made after the death),
# priority inheritance with a spinning call, and in-flight cancellation.
# Part 3 (topics, ADR-003 notify-then-pull): normal delivery, a slow subscriber in
# LATEST and QUEUE mode with explicit loss accounting, kill -9 of a subscriber
# mid-stream, and a server restart with automatic resubscribe.
# Part 4 (qmpctl, when it was built): list/describe from the service's own reflection
# data, method calls with JSON arguments including an application error, a subscription
# printed as JSON lines, and a subscription that survives a server restart.
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
JSRV="$QMP_TARGET_DIR/qmp_joint_server"
JCLI="$QMP_TARGET_DIR/qmp_joint_client"
local_jsrv="$QMP_BUILD_DIR/examples/joint_demo/qmp_joint_server"
local_jcli="$QMP_BUILD_DIR/examples/joint_demo/qmp_joint_client"

QCTL="$QMP_TARGET_DIR/qmpctl"
local_qctl="$QMP_BUILD_DIR/tools/qmpctl/qmpctl"

for f in "$local_bin" "$local_jsrv" "$local_jcli"; do
    if [ ! -f "$f" ]; then
        echo "error: binary not found: $f (build the qnx preset with -DQMP_BUILD_EXAMPLES=ON)" >&2
        exit 2
    fi
done

# LogLevel=ERROR hides OpenSSH's per-connection warnings (e.g. the post-quantum key
# exchange notice) so they do not drown the test output; real errors still print.
SSH_OPTS="-o LogLevel=ERROR"

# Client-side expansion into remote commands is intentional (paths are repo-controlled).
# shellcheck disable=SC2029,SC2086
t() { ssh $SSH_OPTS "$QMP_TARGET" "$@"; }

# Every remote pid we start is recorded here and killed on exit, even after a failure.
CLEANUP_PIDS=""
cleanup() {
    if [ -n "$CLEANUP_PIDS" ]; then
        t "kill -9 $CLEANUP_PIDS 2>/dev/null; true" || true
    fi
}
trap cleanup EXIT INT TERM
track() { CLEANUP_PIDS="$CLEANUP_PIDS $1"; }

t "mkdir -p '$QMP_TARGET_DIR'"

# Best-effort: kill leftovers from an interrupted run so they neither hold the
# binaries' text segments (scp would fail with ETXTBSY) nor squat on the service
# name. slay may be missing on a minimal image; the rename below still protects us.
t "slay -f -9 qmp_smoke qmp_joint_server qmp_joint_client qmpctl 2>/dev/null; true" || true

# Upload to a temp name and rename into place: rename() succeeds even if a stale
# process is still executing the old binary.
push() { # $1 = local file, $2 = remote path
    # shellcheck disable=SC2086
    scp -q $SSH_OPTS "$1" "$QMP_TARGET:$2.new"
    t "mv -f '$2.new' '$2'"
}
push "$local_bin" "$BIN"
push "$local_jsrv" "$JSRV"
push "$local_jcli" "$JCLI"
t "chmod +x '$BIN' '$JSRV' '$JCLI'"
HAVE_QCTL=0
if [ -f "$local_qctl" ]; then
    push "$local_qctl" "$QCTL"
    t "chmod +x '$QCTL'"
    HAVE_QCTL=1
fi

# The target image is minimal: probe the utilities before relying on them.
have() { t "command -v $1 >/dev/null 2>&1"; }
HAVE_PIDIN=0; have pidin && HAVE_PIDIN=1
HAVE_ON=0;    have on    && HAVE_ON=1
echo "target utilities: pidin=$HAVE_PIDIN on=$HAVE_ON"

fails=0
note() { printf '\n=== %s ===\n' "$1"; }
result() { # $1 = name, $2 = rc
    if [ "$2" -eq 0 ]; then echo "--- PASS: $1"; else echo "--- FAIL: $1 (rc=$2)"; fails=$((fails + 1)); fi
}

# Runs in a $(...) subshell: callers must track the echoed pid themselves.
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
track "$SPID"
rc=0; t "'$BIN' timeout" || rc=$?
result timeout "$rc"
stop_server "$SPID"

note "scenario: kill -9 of a client blocked in a call"
SPID=$(start_server)
track "$SPID"
CPID=$(t "'$BIN' block > '$QMP_TARGET_DIR/block.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 1
t "kill -9 $CPID 2>/dev/null || true"
sleep 1
# The server must have absorbed the disconnect pulse and still answer.
rc=0; t "'$BIN' echo" || rc=$?
result kill-client "$rc"
stop_server "$SPID"

note "scenario: kill -9 of the server mid-call, restart, reconnect"
SPID=$(start_server)
track "$SPID"
CPID=$(t "'$BIN' reconnect > '$QMP_TARGET_DIR/reconnect.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
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
rc=0; t "grep -q '^PASS reconnect' '$QMP_TARGET_DIR/reconnect.log'" || rc=$?
t "cat '$QMP_TARGET_DIR/reconnect.log'" || true
result reconnect "$rc"
stop_server "$SPID"

note "scenario: deferred reply racing a client timeout"
SPID=$(start_server)
track "$SPID"
rc=0; t "'$BIN' race" || rc=$?
result race "$rc"
stop_server "$SPID"

note "scenario: both workers busy with slow calls; a third call times out at ~1000 ms"
SPID=$(start_server)
track "$SPID"
rc=0; t "'$BIN' saturate" || rc=$?
result saturate "$rc"
stop_server "$SPID"

# ---- joint scenarios: standalone qmp_joint_server / qmp_joint_client ----------------------

# Start qmp_joint_server in the background on the target; echoes its pid.
# Runs in a $(...) subshell: callers must track the echoed pid themselves.
# Pass --verbose for per-call logs; leave it off when measuring latency.
start_jsrv() { # $1 = log name, $2.. = extra args
    _log="$QMP_TARGET_DIR/$1"
    shift
    t "'$JSRV' $* > '$_log' 2>&1 & echo \$!" | tr -d '\r'
    sleep 1
}

# Poll a remote file for a pattern (ERE). $1 = pattern, $2 = file, $3 = max seconds.
wait_for() {
    _i=0
    while [ "$_i" -lt "$3" ]; do
        if t "grep -Eq '$1' '$QMP_TARGET_DIR/$2' 2>/dev/null"; then return 0; fi
        _i=$((_i + 1))
        sleep 1
    done
    return 1
}

note "joint 1: basic — quiet server, 10000 timed calls after 1000 warm-up, clean SIGTERM"
# No --verbose: per-call logging would be most of what the loop measures.
SPID=$(start_jsrv jsrv1.log)
track "$SPID"
rc=0; t "'$JCLI' loop 10000 1000" || rc=$?
t "kill -15 $SPID 2>/dev/null || true"
sleep 1
if [ "$rc" -eq 0 ]; then
    t "grep -q 'stopped' '$QMP_TARGET_DIR/jsrv1.log'" || rc=$?
fi
t "cat '$QMP_TARGET_DIR/jsrv1.log'" || true
result joint-basic "$rc"

note "joint 2: kernel states — pidin + /dev/name/local during an in-flight spin call"
SPID=$(start_jsrv jsrv2.log --verbose)
track "$SPID"
CPID=$(t "'$JCLI' --timeout 0 slow 6000 spin > '$QMP_TARGET_DIR/jcli2.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2
rc=0
if [ "$HAVE_PIDIN" -eq 1 ]; then
    echo "--- raw pidin, server (pid $SPID): expect worker REPLY/RECEIVE states"
    t "pidin -p $SPID" || rc=$?
    echo "--- raw pidin, client (pid $CPID): expect REPLY-blocked on the server"
    t "pidin -p $CPID" || rc=$?
else
    echo "SKIP: pidin not on the target; state capture skipped"
fi
echo "--- raw /dev/name/local listing"
t "ls -lR /dev/name/local/qmp" || rc=$?
wait_for '^PASS slow' jcli2.log 15 || rc=$?
t "cat '$QMP_TARGET_DIR/jcli2.log'" || true
t "kill -9 $SPID 2>/dev/null || true"
result joint-kernel-states "$rc"

note "joint 3: client death — kill -9 the client mid-call; server logs it and keeps serving"
SPID=$(start_jsrv jsrv3.log --verbose)
track "$SPID"
CPID=$(t "'$JCLI' --timeout 0 slow 5000 sleep > '$QMP_TARGET_DIR/jcli3.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2
t "kill -9 $CPID 2>/dev/null || true"
rc=0
# A killed REPLY-blocked client is not fully disconnected until its handler replies;
# keep the burn short enough that the disconnect pulse arrives within this wait.
wait_for 'client disconnected' jsrv3.log 10 || rc=$?
if [ "$rc" -eq 0 ]; then
    t "'$JCLI' once" || rc=$?
fi
t "cat '$QMP_TARGET_DIR/jsrv3.log'" || true
t "kill -9 $SPID 2>/dev/null || true"
result joint-client-death "$rc"

note "joint 4: server death — kill -9 mid-call; client reports errno, reconnects, new generation"
SPID=$(start_jsrv jsrv4a.log --verbose)
track "$SPID"
CPID=$(t "'$JCLI' --timeout 2000 reconnect 60 > '$QMP_TARGET_DIR/jcli4.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2                       # the client is connected and polling now
t "kill -9 $SPID"
sleep 2                       # let it observe the failure and start its backoff
SPID=$(start_jsrv jsrv4b.log --verbose)
track "$SPID"
rc=0
wait_for '^(PASS|FAIL) reconnect' jcli4.log 40 || rc=$?
if [ "$rc" -eq 0 ]; then
    t "grep -q '^PASS reconnect' '$QMP_TARGET_DIR/jcli4.log'" || rc=$?
fi
t "cat '$QMP_TARGET_DIR/jcli4.log'" || true
t "kill -9 $SPID 2>/dev/null || true"
result joint-server-death "$rc"

note "joint 4b: server death before the call — the first call after it must return ESRCH"
SPID=$(start_jsrv jsrv4c.log --verbose)
track "$SPID"
CPID=$(t "'$JCLI' --expect ESRCH late 3000 > '$QMP_TARGET_DIR/jcli4b.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 1                       # the client is connected and sleeping, not inside a call
t "kill -9 $SPID"
rc=0
wait_for '^(PASS|FAIL) late' jcli4b.log 15 || rc=$?
if [ "$rc" -eq 0 ]; then
    t "grep -q '^PASS late' '$QMP_TARGET_DIR/jcli4b.log'" || rc=$?
fi
t "cat '$QMP_TARGET_DIR/jcli4b.log'" || true
result joint-call-after-server-death "$rc"

note "joint 5: priority — low-prio server, high-prio client, spinning call, raw pidin capture"
if [ "$HAVE_ON" -eq 1 ] && [ "$HAVE_PIDIN" -eq 1 ]; then
    OPID=$(t "on -p 10 '$JSRV' --verbose > '$QMP_TARGET_DIR/jsrv5.log' 2>&1 & echo \$!" | tr -d '\r')
    track "$OPID"
    sleep 1
    # `on` may fork: take the server's real pid from its own start log.
    SPID=$(t "sed -n 's/.*started pid=\([0-9]*\).*/\1/p' '$QMP_TARGET_DIR/jsrv5.log'" | tr -d '\r')
    SPID=${SPID:-$OPID}
    track "$SPID"
    CPID=$(t "on -p 30 '$JCLI' --timeout 0 slow 5000 spin > '$QMP_TARGET_DIR/jcli5.log' 2>&1 & echo \$!" | tr -d '\r')
    track "$CPID"
    sleep 2
    rc=0
    echo "--- raw pidin, server (pid $SPID) DURING the spin call:"
    echo "--- (with inheritance one worker should run at the client's priority, 30)"
    t "pidin -p $SPID" || rc=$?
    wait_for '^PASS slow' jcli5.log 15 || rc=$?
    t "cat '$QMP_TARGET_DIR/jcli5.log'" || true
    t "cat '$QMP_TARGET_DIR/jsrv5.log'" || true
    t "kill -9 $SPID $OPID 2>/dev/null || true"
    result joint-priority "$rc"

    note "joint 5b: fixed-priority negative control — same experiment, no inheritance"
    OPID=$(t "on -p 10 '$JSRV' --verbose --fixed-priority > '$QMP_TARGET_DIR/jsrv5b.log' 2>&1 & echo \$!" | tr -d '\r')
    track "$OPID"
    sleep 1
    SPID=$(t "sed -n 's/.*started pid=\([0-9]*\).*/\1/p' '$QMP_TARGET_DIR/jsrv5b.log'" | tr -d '\r')
    SPID=${SPID:-$OPID}
    track "$SPID"
    CPID=$(t "on -p 30 '$JCLI' --timeout 0 slow 5000 spin > '$QMP_TARGET_DIR/jcli5b.log' 2>&1 & echo \$!" | tr -d '\r')
    track "$CPID"
    sleep 2
    rc=0
    echo "--- raw pidin, server (pid $SPID) DURING the spin call:"
    echo "--- (with _NTO_CHF_FIXED_PRIORITY every worker should STAY at 10; V-QNX-041)"
    t "pidin -p $SPID" || rc=$?
    wait_for '^PASS slow' jcli5b.log 15 || rc=$?
    t "cat '$QMP_TARGET_DIR/jcli5b.log'" || true
    t "cat '$QMP_TARGET_DIR/jsrv5b.log'" || true
    t "kill -9 $SPID $OPID 2>/dev/null || true"
    result joint-fixed-priority "$rc"
else
    echo "SKIP: joint-priority needs the 'on' and 'pidin' utilities on the target"
fi

note "joint 6: cancellation — 1 s client timeout against a 5 s Burn (sleep and spin)"
SPID=$(start_jsrv jsrv6.log --verbose)
track "$SPID"
rc=0
for variant in sleep spin; do
    crc=0
    t "'$JCLI' --timeout 1000 --expect ETIMEDOUT slow 5000 $variant > '$QMP_TARGET_DIR/jcli6-$variant.log' 2>&1" || crc=$?
    # The client must give up at ~1 s with ETIMEDOUT (PASS via --expect), NOT succeed
    # after 5 s; the server must log the unblock pulse and the early burn abort.
    [ "$crc" -eq 0 ] || rc=1
    t "grep -q '^PASS slow' '$QMP_TARGET_DIR/jcli6-$variant.log'" || rc=$?
    t "grep -Eq 'elapsed_ms=1[0-9]{3}([^0-9]|\$)' '$QMP_TARGET_DIR/jcli6-$variant.log'" || rc=$?
    wait_for 'burn canceled' jsrv6.log 10 || rc=$?
    t "cat '$QMP_TARGET_DIR/jcli6-$variant.log'" || true
done
if [ "$rc" -eq 0 ]; then
    t "'$JCLI' once" || rc=$? # the server must still serve after the cancellations
fi
t "cat '$QMP_TARGET_DIR/jsrv6.log'" || true
t "kill -9 $SPID 2>/dev/null || true"
result joint-cancel "$rc"

# ---- topic scenarios (ADR-003 notify-then-pull) --------------------------------------------

note "joint 7: topics, normal delivery — 50 ms publisher, LATEST subscriber prints 20 updates"
SPID=$(start_jsrv jsrv7.log --publish-ms 50)
track "$SPID"
rc=0; t "'$JCLI' subscribe state 20 30" || rc=$?
result joint-topic-normal "$rc"
t "kill -9 $SPID 2>/dev/null || true"

note "joint 7b: slow subscriber, LATEST — 5 ms publisher, 300 ms poll sleep: skips to newest"
SPID=$(start_jsrv jsrv7b.log --publish-ms 5)
track "$SPID"
rc=0
t "'$JCLI' subscribe state 10 30 300 > '$QMP_TARGET_DIR/jcli7b.log' 2>&1" || rc=$?
# LATEST mode must overwrite: with ~60 updates per poll interval the subscriber has to
# report dropped > 0 somewhere; silent loss would be a bug.
t "grep -q '^PASS subscribe' '$QMP_TARGET_DIR/jcli7b.log'" || rc=$?
t "grep -Eq 'dropped=[1-9]' '$QMP_TARGET_DIR/jcli7b.log'" || rc=$?
t "cat '$QMP_TARGET_DIR/jcli7b.log'" || true
result joint-topic-slow-latest "$rc"
t "kill -9 $SPID 2>/dev/null || true"

note "joint 7c: slow subscriber, QUEUE depth 8 — backlog replays in order, overflow is counted"
SPID=$(start_jsrv jsrv7c.log --publish-ms 5)
track "$SPID"
rc=0
t "'$JCLI' subscribe log 40 30 300 > '$QMP_TARGET_DIR/jcli7c.log' 2>&1" || rc=$?
t "grep -q '^PASS subscribe' '$QMP_TARGET_DIR/jcli7c.log'" || rc=$?
# With ~60 publishes per 300 ms sleep and a depth of 8, overflow must be reported.
t "grep -Eq 'dropped=[1-9]' '$QMP_TARGET_DIR/jcli7c.log'" || rc=$?
t "cat '$QMP_TARGET_DIR/jcli7c.log'" || true
result joint-topic-slow-queue "$rc"
t "kill -9 $SPID 2>/dev/null || true"

note "joint 7d: kill -9 of a subscriber mid-stream — publisher keeps serving the others"
SPID=$(start_jsrv jsrv7d.log --publish-ms 50 --verbose)
track "$SPID"
VPID=$(t "'$JCLI' subscribe state 1000 60 > '$QMP_TARGET_DIR/jcli7d-victim.log' 2>&1 & echo \$!" | tr -d '\r')
track "$VPID"
CPID=$(t "'$JCLI' subscribe state 100 60 > '$QMP_TARGET_DIR/jcli7d-survivor.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2
t "kill -9 $VPID 2>/dev/null || true"
rc=0
# The server must observe the dead subscriber's disconnect and free its slot...
wait_for 'client disconnected' jsrv7d.log 10 || rc=$?
# ...while the survivor keeps receiving until its 100 updates are in.
wait_for '^PASS subscribe' jcli7d-survivor.log 30 || rc=$?
t "tail -5 '$QMP_TARGET_DIR/jcli7d-survivor.log'" || true
result joint-topic-subscriber-death "$rc"
t "kill -9 $SPID 2>/dev/null || true"

note "joint 7e: server restart mid-stream — subscriber reconnects, resubscribes, updates resume"
SPID=$(start_jsrv jsrv7e-a.log --publish-ms 50)
track "$SPID"
CPID=$(t "'$JCLI' subscribe state 200 60 > '$QMP_TARGET_DIR/jcli7e.log' 2>&1 & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2                       # a few updates have arrived on the first session
t "kill -9 $SPID"
sleep 2                       # the subscriber sees ESRCH and starts reconnecting
SPID=$(start_jsrv jsrv7e-b.log --publish-ms 50)
track "$SPID"
rc=0
wait_for 'subscribe: qmp_client_reconnect -> 0' jcli7e.log 30 || rc=$?
wait_for '^PASS subscribe' jcli7e.log 40 || rc=$?
t "tail -8 '$QMP_TARGET_DIR/jcli7e.log'" || true
result joint-topic-server-restart "$rc"
t "kill -9 $SPID 2>/dev/null || true"

# ---- qmpctl scenarios (reflection-driven CLI, no generated code in the tool) ----------------

if [ "$HAVE_QCTL" -eq 1 ]; then

note "qmpctl 8a: list and describe a running service from its own reflection data"
SPID=$(start_jsrv jsrv8a.log --publish-ms 20)
track "$SPID"
rc=0
t "'$QCTL' --version" || rc=$?
t "'$QCTL' list > '$QMP_TARGET_DIR/qctl8a-list.log' 2>&1" || rc=$?
t "cat '$QMP_TARGET_DIR/qctl8a-list.log'" || true
t "grep -Eq '^qmp/0101-0000/v1 +demo\.joint\.Joint +1\.0 +4 +2 +/101/1' '$QMP_TARGET_DIR/qctl8a-list.log'" || rc=$?
t "'$QCTL' describe Joint > '$QMP_TARGET_DIR/qctl8a-desc.log' 2>&1" || rc=$?
t "cat '$QMP_TARGET_DIR/qctl8a-desc.log'" || true
t "grep -q 'endpoint  qmp/0101-0000/v1  (transport qnx)' '$QMP_TARGET_DIR/qctl8a-desc.log'" || rc=$?
t "grep -q '0x0003  GetState(demo.joint.v1.Empty) returns (demo.joint.v1.JointState)' '$QMP_TARGET_DIR/qctl8a-desc.log'" || rc=$?
t "grep -q '0x8002  StateLog  demo.joint.v1.JointState  QUEUE depth 8' '$QMP_TARGET_DIR/qctl8a-desc.log'" || rc=$?
result qmpctl-list-describe "$rc"

note "qmpctl 8b: call with JSON arguments — reply, state change and application error"
rc=0
t "'$QCTL' call Joint SetTarget '{\"position_rad\": 1.25, \"max_velocity\": 2}' > '$QMP_TARGET_DIR/qctl8b.log' 2>&1" || rc=$?
t "'$QCTL' call 0101 GetState >> '$QMP_TARGET_DIR/qctl8b.log' 2>&1" || rc=$?
# max_velocity 0 is rejected by the handler: status INVALID_ARGUMENT, exit code 1.
erc=0
t "'$QCTL' call Joint SetTarget position_rad=3 max_velocity=0 >> '$QMP_TARGET_DIR/qctl8b.log' 2>&1" || erc=$?
[ "$erc" -eq 1 ] || rc=1
t "cat '$QMP_TARGET_DIR/qctl8b.log'" || true
t "grep -q '^{\"result\":' '$QMP_TARGET_DIR/qctl8b.log'" || rc=$?
t "grep -q '\"position_rad\":1.25,' '$QMP_TARGET_DIR/qctl8b.log'" || rc=$?
t "grep -q 'Joint.SetTarget returned status 3 (INVALID_ARGUMENT)' '$QMP_TARGET_DIR/qctl8b.log'" || rc=$?
result qmpctl-call "$rc"

note "qmpctl 8c: subscribe prints one JSON line per update (QUEUE topic, nothing dropped)"
rc=0
t "'$QCTL' subscribe Joint StateLog --count 25 > '$QMP_TARGET_DIR/qctl8c.log' 2> '$QMP_TARGET_DIR/qctl8c.err'" || rc=$?
n=$(t "grep -c '^{\"topic\":\"StateLog\",\"seq\":[0-9]*,\"dropped\":0,\"rx_ns\":[0-9]*,\"sample\":{' '$QMP_TARGET_DIR/qctl8c.log'" | tr -d '\r') || true
[ "$n" = "25" ] || rc=1
t "head -3 '$QMP_TARGET_DIR/qctl8c.log'; cat '$QMP_TARGET_DIR/qctl8c.err'" || true
t "grep -q '25 updates received, 0 dropped' '$QMP_TARGET_DIR/qctl8c.err'" || rc=$?
result qmpctl-subscribe "$rc"
t "kill -9 $SPID 2>/dev/null || true"

note "qmpctl 8d: subscribe across a server restart — reconnects and keeps printing"
SPID=$(start_jsrv jsrv8d-a.log --publish-ms 50)
track "$SPID"
CPID=$(t "'$QCTL' subscribe Joint StateChanged --count 80 > '$QMP_TARGET_DIR/qctl8d.log' 2> '$QMP_TARGET_DIR/qctl8d.err' & echo \$!" | tr -d '\r')
track "$CPID"
sleep 2                       # a few updates have arrived on the first session
t "kill -9 $SPID"
sleep 2                       # qmpctl sees ESRCH and starts reconnecting
SPID=$(start_jsrv jsrv8d-b.log --publish-ms 50)
track "$SPID"
rc=0
wait_for 'reconnected \(server generation' qctl8d.err 30 || rc=$?
wait_for '80 updates received' qctl8d.err 40 || rc=$?
t "cat '$QMP_TARGET_DIR/qctl8d.err'" || true
n=$(t "grep -c '^{\"topic\":\"StateChanged\"' '$QMP_TARGET_DIR/qctl8d.log'" | tr -d '\r') || true
[ "$n" = "80" ] || rc=1
result qmpctl-subscribe-server-restart "$rc"
t "kill -9 $SPID 2>/dev/null || true"

else
    echo
    echo "qmpctl not built ($local_qctl missing): skipping the qmpctl scenarios"
fi

echo
if [ "$fails" -eq 0 ]; then
    echo "ALL SMOKE SCENARIOS PASSED"
else
    echo "$fails SMOKE SCENARIO(S) FAILED"
fi
exit "$fails"
