# qmp — service-oriented communication over QNX native message passing

[![host](https://github.com/miketsukerman/qmp/actions/workflows/host.yml/badge.svg)](https://github.com/miketsukerman/qmp/actions/workflows/host.yml)

qmp lets you describe a service in Protobuf (proto3, used as a **schema language only**),
annotated with [Eclipse uProtocol](https://github.com/eclipse-uprotocol/up-spec) service options,
and generates C11 bindings (plus optional header-only C++17 wrappers) whose transport is QNX
native message passing: `MsgSend`/`MsgReceive`/`MsgReply`, registered events and pulses.
No sockets, no varint encoding on the hot path — payloads are fixed-layout PODs, so the
synchronous semantics, priority inheritance and determinism of QNX IPC are kept and showcased.

An individual entry for the QNX Global Developer Hackathon 2026.

* Design: [docs/DESIGN.md](docs/DESIGN.md) · Decisions: [docs/DECISIONS.md](docs/DECISIONS.md)
* Open verification items against QNX SDP 8.0 / uProtocol: [docs/VERIFY.md](docs/VERIFY.md)
* Tutorial: [docs/codelab/](docs/codelab/) · Spikes: [docs/spikes/](docs/spikes/)

## Architecture

```mermaid
flowchart TB
  subgraph HOST["Host: code generation"]
    direction LR
    proto["joint.proto<br/>uProtocol options<br/>qmp/options.proto"]
    gen["protoc + protoc-gen-qmp<br/>(Python, Jinja2)"]
    art["joint.qmp.h / joint.qmp.c (C11)<br/>joint.qmp.hpp (C++17, optional)<br/>joint.reflect.json"]
    proto --> gen --> art
  end

  subgraph TARGET["Target: QNX SDP 8.0 (aarch64le / x86_64)"]
    subgraph CLIENT["Client process (C or C++)"]
      c_user["Application code<br/>calls, qmp_client_poll()"]
      c_gen["Generated proxy<br/>typed stubs, topic callbacks"]
      c_core["libqmp core<br/>header codec, HELLO, reconnect"]
      c_tx["transport/qnx<br/>MsgSendv, TimerTimeout, MsgRegisterEvent"]
      c_user --- c_gen --- c_core --- c_tx
    end

    subgraph SERVER["Server process (joint_service)"]
      s_user["Handlers, publish loop"]
      s_gen["Generated skeleton<br/>ops table, bounds validators"]
      s_core["libqmp core<br/>dispatch, subscriptions, deferred pool"]
      s_tx["transport/qnx<br/>N workers RECEIVE-blocked on one channel"]
      s_user --- s_gen --- s_core --- s_tx
    end

    kernel["QNX microkernel (procnto)<br/>channels, connections, pulses,<br/>priority inheritance, registered events"]

    c_tx <-->|"MsgSendv: 24-byte header IOV + POD payload IOV"| kernel
    s_tx <-->|"MsgReceivev / MsgReplyv / MsgError"| kernel
    s_tx -.->|"MsgDeliverEvent: topic pulse, client then pulls"| c_tx
  end

  art -.->|compiled into| c_gen
  art -.->|compiled into| s_gen
```

Each layer only calls the one below it. Handlers run on a libqmp worker at the
caller's priority (inherited through `MsgSend`); the server never sends to a
client, so topics use notify-then-pull over registered events. The same core
also runs over `transport/fake` (unit tests) and `transport/uds` (benchmarks
only). Details and sequence diagrams: [docs/DESIGN.md](docs/DESIGN.md).

## Status (W3 in progress)

| Area | State |
|------|-------|
| `protoc-gen-qmp` | sync methods: validated schema → C11 layouts with static asserts, ids, UUri constants, client stubs, ops table, reflection JSON. C++17 wrappers (`*.qmp.hpp`) and secpol emitter: not yet |
| `libqmp` core | header codec, UCode mapping, dispatch, version handshake, caller-provided storage; deferred replies (fixed pool), one-way calls, client timeouts with in-flight cancellation, disconnect/server-death lifecycle with reconnect, `acl_check` hook on kernel credentials; topics (notify-then-pull, LATEST/QUEUE with loss accounting, resubscribe on restart) |
| transports | `fake` (in-process, host tests, mirrors documented 8.0 kernel semantics); QNX native (`name_attach`, `MsgSendv`, unblock/disconnect pulses, registered events); `uds` (`AF_UNIX`/`SOCK_SEQPACKET`, host default, also built on QNX) for benchmarks and host integration tests only — **no priority inheritance** (ADR-004) |
| tools | `qmpctl list \| describe \| call \| subscribe`: C++17, links only the C ABI, no generated code — every generated service serves its own reflection JSON (`QMP_OP_REFLECT`, ADR-010) and qmpctl converts JSON ⇄ fixed-layout payloads from it ([Codelab 04](docs/codelab/04-inspect-with-qmpctl.md)) |
| examples | `qmp_joint_server` / `qmp_joint_client` (generated Joint bindings), `qmp_smoke` (hand-written service); two-process target scenarios in `scripts/target-smoke.sh` |
| tests | 122 GoogleTest cases: 88 in `qmp_core_tests` (22 over UDS, including two-process kill -9 scenarios) and 34 in `qmpctl_tests` (JSON codec checked against generated structs; commands run against live servers) + `joint_server --selftest` (host) + 55 codegen pytest cases, asan/ubsan/tsan clean |
| on target | RPi5, build `3cd249234123` (2026-11-27): 88/88 `qmp_core_tests` cases (22 over UDS), `qmpctl_tests` 33/33 runnable on the native transport and 34/34 over UDS, 22/22 `scripts/target-smoke.sh` scenarios (four of them through `qmpctl`). Build `a4230da3880c`: joint demo over `--transport uds` and `qnx`; results recorded in [docs/VERIFY.md](docs/VERIFY.md). Same-board round trip, `loop 5000` with a 5 s timeout: native median 4.0 µs / p99 5.7 µs, UDS median 57.8 µs / p99 68.5 µs (a first look, not the W4 benchmark) |
| next | bench, secpol generation, uProtocol adapter (W4, adapter is stretch); C++ wrappers and one-way trampolines in the generator (stretch) |

## Build from a clean machine (Ubuntu 24.04, host build)

```sh
sudo apt-get install -y build-essential clang cmake ninja-build python3 python3-pip python3-venv \
                        protobuf-compiler clang-format clang-tidy cppcheck
git clone https://github.com/miketsukerman/qmp && cd qmp

python3 -m venv .venv && . .venv/bin/activate
pip install -e "codegen[dev]"          # protoc-gen-qmp + pytest/ruff/mypy/grpcio-tools

cmake --preset host-debug              # also: host-asan, host-ubsan, host-tsan, host-release
cmake --build build/host-debug
ctest --test-dir build/host-debug --output-on-failure
```

Expected tail of the `ctest` output:

```
100% tests passed, 0 tests failed out of 123
```

Generated bindings for the example service land in `build/host-debug/gen/joint/`
(`joint.qmp.h`, `joint.qmp.c`, `joint.reflect.json`). GoogleTest and nlohmann/json (qmpctl
only) are fetched with `FetchContent` on first configure (network needed once);
`-DQMP_BUILD_TOOLS=OFF` builds without qmpctl and without the JSON dependency.

### Try it: qmpctl against the demo server

```sh
export QMP_UDS_DIR=$(mktemp -d)
build/host-debug/examples/joint_demo/qmp_joint_server --publish-ms 50 &
alias qmpctl=$PWD/build/host-debug/tools/qmpctl/qmpctl

qmpctl list                                   # what is registered on this node
qmpctl describe Joint                         # interface, read from the running service
qmpctl call Joint SetTarget position_rad=1.5 max_velocity=2
qmpctl call Joint GetState --pretty
qmpctl subscribe Joint StateChanged --count 3 # one JSON line per update
```

On the QNX target the same commands run over native message passing; `list` walks
`/dev/name/local/qmp`.

### Codegen tests and lint

```sh
cd codegen
python3 -m pytest tests -q            # goldens, one fixture per validation error, C11/C++17 compile
ruff check . && ruff format --check . && mypy --strict protoc_gen_qmp tests
cd ..
clang-format --dry-run -Werror $(git ls-files 'include/**/*.h' 'runtime/**/*.[ch]' 'runtime/**/*.cpp' 'examples/**/*.c' 'tools/**/*.[ch]pp')
clang-tidy -p build/host-debug runtime/core/*.c runtime/transport/fake/*.c runtime/transport/uds/*.c
cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability --std=c11 \
         -Iinclude runtime/core runtime/transport/fake runtime/transport/uds examples/joint_demo/joint_server.c
```

Regenerate goldens on purpose only: `UPDATE_GOLDEN=1 python3 -m pytest tests -q`, then review
the diff.

### Running the generator by hand

```sh
protoc -I third_party/up-spec/up-core-api -I idl \
       --plugin=protoc-gen-qmp=$(which protoc-gen-qmp) \
       --qmp_out=flat:/tmp/gen idl/examples/demo/joint/v1/joint.proto
```

In CMake, `qmp_add_service(NAME joint PROTO idl/examples/demo/joint/v1/joint.proto)` does the
same (see `cmake/QmpCodegen.cmake`).

## QNX target build (manual — no SDP in CI)

Primary board: **Raspberry Pi 5** with the QNX SDP 8.0 quick-start target image (package
`com.qnx.qnx800.quickstart.rpi5`); bring-up guide: [`docs/target/rpi5.md`](docs/target/rpi5.md).
Image details (SDP patch level, security policy) are unverified — see `V-QNX-026`/`V-QNX-027`
in [`docs/VERIFY.md`](docs/VERIFY.md). Secondary board: Raspberry Pi 4 (one portability and
benchmark pass in the final week). x86_64 under QEMU is for functional development only — never
for headline latency numbers. Requires QNX SDP 8.0 installed on the host.

```sh
source ~/qnx800/qnxsdp-env.sh
cmake --preset qnx-aarch64le           # qcc -Vgcc_ntoaarch64le / q++ -Vgcc_ntoaarch64le_gpp
cmake --build build/qnx-aarch64le
# x86_64 (QEMU): cmake --preset qnx-x86_64
```

On QNX the runtime links the native transport (`runtime/transport/qnx/`); the target build
produces the core tests, `qmp_joint_server`/`qmp_joint_client`, `qmp_smoke`, `qmpctl` and its
tests. Set
`QMP_TARGET` to the board's SSH address, then copy and run one binary with
`scripts/target-run.sh`, or run every two-process scenario (timeouts, client/server death,
priority inheritance, cancellation, topics, qmpctl) with `scripts/target-smoke.sh`:

```sh
export QMP_TARGET=qnxuser@<pi5-address>
scripts/target-run.sh runtime/tests/qmp_core_tests
scripts/target-run.sh tools/qmpctl/tests/qmpctl_tests
scripts/target-smoke.sh
```

Walkthrough: [Codelab 02](docs/codelab/02-first-service-on-qnx.md).

Sanitizers on target: `-DQMP_SANITIZER=ubsan` (asan needs the shared runtime; no tsan on QNX —
use the host tsan preset).

## Repository layout

```
idl/                  qmp/options.proto (ext. 50000–50999), examples/demo/joint
third_party/up-spec/  pinned uProtocol protos (v1.5.8, Apache-2.0) — see PIN.md
codegen/              protoc-gen-qmp (Python ≥ 3.10, Jinja2) + golden tests
include/qmp/          public C ABI headers
runtime/core/         libqmp, OS-independent C11
runtime/transport/    fake (host tests), qnx (target), uds (bench/host tests only, no priority inheritance)
runtime/tests/        GoogleTest suites over the fake and uds transports
examples/joint_demo/  qmp_joint_server / qmp_joint_client: Joint actuator service in C
                      (--selftest on host; publisher/subscriber modes for topics)
examples/smoke/       qmp_smoke: hand-written service for target edge-case scenarios
tools/qmpctl/         qmpctl: list/describe/call/subscribe from reflection data (C++17, C ABI only)
scripts/              target-run.sh, target-smoke.sh (SSH to the QNX board)
cmake/                presets support, qnx-*.cmake toolchains, QmpCodegen.cmake
docs/                 DESIGN, DECISIONS (ADRs), VERIFY register, codelab, spikes
```

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Vendored uProtocol specs are
Apache-2.0 (Eclipse Foundation).
