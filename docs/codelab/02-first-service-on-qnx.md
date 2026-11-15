# Codelab 02 — First service on QNX: from a .proto to a running Pi

**You will:** take the joint service from Codelab 01, cross-compile it for QNX SDP 8.0
aarch64le, run it on a Raspberry Pi 5 over **real kernel message passing**, watch what the
kernel does for you (priority inheritance, unblock pulses, disconnect pulses), and run the
conformance and smoke suites on the target.

**Time:** ~45 min. **Needs:** Codelab 01 done on the host, QNX SDP 8.0 installed on your
Linux host, a Pi 5 running the QNX quick-start image reachable over SSH
(`docs/target/rpi5.md` covers flashing and networking).

The point of this codelab: **nothing in your service code changes.** The same `joint.proto`,
the same generated header, the same handler and client code from Codelab 01 now run over
`MsgSendv`/`MsgReceivev`/`MsgReplyv` instead of the in-process fake transport — because
`qmp_transport_default()` returns the native transport when compiled for QNX.

---

## 1. What actually changes on QNX

On the host, `qmp_transport_default()` returns the fake transport: an in-process pthread
rendezvous that *mimics* kernel semantics. On QNX it returns
`qmp_transport_qnx()` (`runtime/transport/qnx/qnx_transport.c`), and the pieces map like this:

| qmp concept | host (fake) | QNX (native) |
|---|---|---|
| `qmp_server_init` + start | registry slot + worker pthreads | `name_attach("qmp/0101-0000/v1")` → channel under `/dev/name/local/`, N threads blocked in `MsgReceivev` |
| `qmp_client_connect` | registry lookup | `name_open()` + the same QMP_HELLO handshake |
| method call | condvar rendezvous | `MsgSendv` — the server thread **inherits your priority** until it replies |
| call timeout | timed condvar wait | `TimerTimeout(CLOCK_MONOTONIC, _NTO_TIMEOUT_SEND \| _NTO_TIMEOUT_REPLY, …)` armed by the kernel |
| client died mid-call | UNBLOCK event in a queue | `_PULSE_CODE_UNBLOCK` / `_PULSE_CODE_DISCONNECT` pulses from the kernel |
| transport error | return code | `MsgError(rcvid, errno)` — distinct from application status in the header |

The service name is derived for you: service id `0x0101`, instance `0`, major version 1
becomes `/dev/name/local/qmp/0101-0000/v1`. You never type it.

## 2. Cross-compile

Source the SDP environment, then use the `qnx-aarch64le` preset (it selects
`qcc -Vgcc_ntoaarch64le` / `q++ -Vgcc_ntoaarch64le_gpp` via `cmake/qnx-aarch64le.cmake`):

```sh
source /path/to/qnx800/qnxsdp-env.sh      # adjust to your SDP install
cmake --preset qnx-aarch64le -DQMP_BUILD_TESTS=ON
cmake --build --preset qnx-aarch64le --parallel 2
```

You get, under `build/qnx-aarch64le/`:

- `examples/joint_demo/joint_service`, `joint_client` — Codelab 01's demo, now native
- `runtime/tests/qmp_core_tests` — the conformance suite (googletest)
- `examples/smoke/qmp_smoke` — target-only lifecycle scenarios

Everything is built with the same strict flags as the host build
(`-std=c11 -Wall -Wextra -Wpedantic -Werror`); the only QNX-specific define is `_QNX_SOURCE`.

## 3. Run the demo on the Pi

`scripts/target-run.sh` copies one binary to the target over SSH and runs it:

```sh
export QMP_TARGET=qnxuser@192.168.178.139      # your Pi
scripts/target-run.sh examples/joint_demo/joint_service
# joint_service: demo.joint.Joint listening on qmp/0101-0000/v1
```

While it runs, the registered name is visible from another shell:

```sh
ssh $QMP_TARGET ls -l /dev/name/local/qmp/
# 0101-0000/v1
```

(There is no standalone demo client binary yet — `qmpctl` is a later milestone. The calls
against a real server happen in the next two sections, and `qmp_smoke echo` is a minimal
client you can read as a template.)

What you can't see on the host but is happening here: the server worker thread runs at the
*client's* priority for the duration of each call (`pidin -p joint_service threads` while a
high-priority client hammers it), and the channel was created by `name_attach` with
`_NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT` so the kernel tells the server about impatient and
dead clients.

## 4. Run the conformance suite on the target

The same 46 tests that ran over the fake transport on your host are the contract the native
transport must honour:

```sh
scripts/target-run.sh runtime/tests/qmp_core_tests
```

Expected: `[  PASSED  ] 46 tests.` If a test passes on the host and fails here, the fake
transport is wrong, not the kernel — file it and the fake gets fixed (ADR-009).

## 5. Run the smoke scenarios (what the fake can never exercise)

Four things only a real kernel can prove, orchestrated over SSH by
`scripts/target-smoke.sh` (two OS processes, real `kill -9`):

```sh
scripts/target-smoke.sh
```

1. **timeout** — the server parks the request and never replies; the client's
   `TimerTimeout` fires, the server gets `_PULSE_CODE_UNBLOCK` and cancels the parked
   request with `MsgError(ETIMEDOUT)`. No double reply, no leaked table slot.
2. **kill-client** — a client is killed with `kill -9` while REPLY-blocked. The server gets
   `_PULSE_CODE_DISCONNECT`, calls `ConnectDetach`, drops that process's state — and still
   answers the next client.
3. **reconnect** — the *server* is killed mid-call and restarted. The client's call fails
   (`ESRCH`/`EBADF` — never silently replayed), `qmp_client_reconnect()` retries with
   exponential backoff, the new HELLO returns a different `server_generation`, and the client
   carries on against the new instance.
4. **race** — a deferred reply from another server thread races the client's timeout.
   Whichever wins, exactly one of {reply, ETIMEDOUT} reaches the client and the request slot
   is freed exactly once.

Each prints `PASS`/`FAIL`; the script exits non-zero on any failure.

## 6. Where to look when something is off

- `qmp/...` missing under `/dev/name/local/` → the server didn't start or `name_attach`
  failed (permissions: V-QNX-021).
- Client `ENOENT` from connect → name mismatch; both sides derive it from the same
  service id/instance/major, so check those three numbers.
- `ETIMEDOUT` instantly → you passed a timeout of 0 somewhere; `0` means "no timeout" only
  in `qmp_client_cfg_t`, not per call.
- Open kernel-behaviour questions live in `docs/VERIFY.md` (V-QNX-031…035 are the ones this
  milestone added) — if the Pi disagrees with a stated assumption, that register is where it
  gets recorded.

**Next:** Codelab 03 — topics: notify-then-pull with `MsgDeliverEvent` (later milestone).
