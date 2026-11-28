#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# V-QNX-015: does a QNX security policy keep an unlisted process type away from a qmp
# service? Compiles tools/probe/secpol/joint.secpol on the host, loads it into procnto on
# the target and runs the Joint server under type joint_svc_t with three clients:
#
#   joint_ctrl_t    allowed by the policy            expected: PASS once
#   joint_other_t   not mentioned for this channel   expected: connect fails (EACCES)
#   no type         as started over SSH              reported, no expectation
#
# THIS CHANGES SYSTEM STATE ON THE TARGET: it needs root (passwordless sudo) and loads a
# security policy into the running kernel, where it stays until the board reboots. While
# it is loaded, processes without a type can no longer name_attach: qmp servers started
# the normal way fail with EACCES, so REBOOT THE BOARD AFTERWARDS. Run it on a development
# board only. It is not part of scripts/target-smoke.sh for that reason.
#
# Usage: QMP_TARGET=qnxuser@192.168.1.50 scripts/target-secpol-check.sh --i-understand
#
# Environment: QMP_TARGET (required), QMP_BUILD_DIR, QMP_TARGET_DIR as in target-smoke.sh;
# QNX_HOST must be set (source qnxsdp-env.sh) for secpolcompile.

set -eu

if [ "${1:-}" != "--i-understand" ]; then
    sed -n '3,20p' "$0" >&2
    exit 2
fi
if [ -z "${QMP_TARGET:-}" ]; then
    echo "error: QMP_TARGET is not set" >&2
    exit 2
fi
QMP_BUILD_DIR="${QMP_BUILD_DIR:-build/qnx-aarch64le}"
QMP_TARGET_DIR="${QMP_TARGET_DIR:-/tmp/qmp}"
JSRV="$QMP_TARGET_DIR/qmp_joint_server"
JCLI="$QMP_TARGET_DIR/qmp_joint_client"
SSH_OPTS="-o LogLevel=ERROR"
# shellcheck disable=SC2029,SC2086
t() { ssh $SSH_OPTS "$QMP_TARGET" "$@"; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"; t "sudo -n slay -f -9 qmp_joint_server 2>/dev/null; true" || true' EXIT INT TERM
secpolcompile -m -o "$tmp/joint.bin" tools/probe/secpol/joint.secpol
echo "policy compiled: $(wc -c < "$tmp/joint.bin") bytes"

t "mkdir -p '$QMP_TARGET_DIR'; slay -f -9 qmp_joint_server qmp_joint_client 2>/dev/null; true" || true
for f in qmp_joint_server qmp_joint_client; do
    # shellcheck disable=SC2086
    scp -q $SSH_OPTS "$QMP_BUILD_DIR/examples/joint_demo/$f" "$QMP_TARGET:$QMP_TARGET_DIR/$f.new"
    t "mv -f '$QMP_TARGET_DIR/$f.new' '$QMP_TARGET_DIR/$f' && chmod +x '$QMP_TARGET_DIR/$f'"
done
# shellcheck disable=SC2086
scp -q $SSH_OPTS "$tmp/joint.bin" "$QMP_TARGET:$QMP_TARGET_DIR/joint-secpol.bin"

echo "--- policy id before: $(t 'sudo -n secpol -Q' | tr -d '\r')"
t "sudo -n secpolpush '$QMP_TARGET_DIR/joint-secpol.bin'"
echo "--- policy id after:  $(t 'sudo -n secpol -Q' | tr -d '\r')"

t "sudo -n on -S '$QMP_TARGET_DIR/joint-secpol.bin' -T joint_svc_t '$JSRV' --verbose > '$QMP_TARGET_DIR/secpol-srv.log' 2>&1 &"
sleep 1

rc=0
echo "--- client of type joint_ctrl_t (allowed):"
t "sudo -n on -S '$QMP_TARGET_DIR/joint-secpol.bin' -T joint_ctrl_t '$JCLI' once" || rc=1
echo "--- client of type joint_other_t (not allowed): expect a refused connect"
other=$(t "sudo -n on -S '$QMP_TARGET_DIR/joint-secpol.bin' -T joint_other_t '$JCLI' once 2>&1" || true)
echo "$other"
case "$other" in
*"connect to "*" failed"*) echo "ok:   the unlisted type was refused" ;;
*) echo "MISS: expected the client to start and its connect to be refused"; rc=1 ;;
esac
echo "--- client without a type (as started over SSH): informational"
t "'$JCLI' once" || true
echo "--- server log:"
t "cat '$QMP_TARGET_DIR/secpol-srv.log'" || true
if [ "$rc" -eq 0 ]; then echo "SECPOL CHECK PASSED"; else echo "SECPOL CHECK FAILED"; fi
exit "$rc"
