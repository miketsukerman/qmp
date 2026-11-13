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

## Status (W1)

| Area | State |
|------|-------|
| `protoc-gen-qmp` | sync methods: validated schema → C11 layouts with static asserts, ids, UUri constants, client stubs, ops table, reflection JSON |
| `libqmp` core | header codec, UCode mapping, dispatch, version handshake, caller-provided storage |
| transports | `fake` (in-process, host tests). QNX native: W2. UDS (bench only): W3 |
| tests | 36 GoogleTest cases (host) + 55 codegen pytest cases, asan/ubsan/tsan clean |
| deferred reply, one-way, topics, lifecycle, access control | W2–W3 |
| qmpctl, bench, uProtocol adapter | W3–W4 |

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
100% tests passed, 0 tests failed out of 36
```

Generated bindings for the example service land in `build/host-debug/gen/joint/`
(`joint.qmp.h`, `joint.qmp.c`, `joint.reflect.json`). GoogleTest is fetched with
`FetchContent` on first configure (network needed once).

### Codegen tests and lint

```sh
cd codegen
python3 -m pytest tests -q            # goldens, one fixture per validation error, C11/C++17 compile
ruff check . && ruff format --check . && mypy --strict protoc_gen_qmp tests
cd ..
clang-format --dry-run -Werror $(git ls-files 'include/**/*.h' 'runtime/**/*.[ch]' 'runtime/**/*.cpp' 'examples/**/*.c')
clang-tidy -p build/host-debug runtime/core/*.c runtime/transport/fake/*.c
cppcheck --quiet --error-exitcode=1 --enable=warning,performance,portability --std=c11 \
         -Iinclude runtime/core runtime/transport/fake examples/joint_demo/joint_service.c
```

Regenerate goldens on purpose only: `UPDATE_GOLDEN=1 python3 -m pytest tests -q`, then review
the diff.

### Running the generator by hand

```sh
protoc -I third_party/up-spec/up-core-api -I idl \
       --plugin=protoc-gen-qmp=$(which protoc-gen-qmp) \
       --qmp_out=flat:/tmp/gen examples/demo/joint/v1/joint.proto
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

The QNX native transport (`runtime/transport/qnx/`) arrives in W2; until then the target build
produces the core library and generated bindings only. Copy and run tests with
`scripts/target-run.sh` (set `QMP_TARGET` to the board's SSH address first):

```sh
export QMP_TARGET=qnxuser@<pi5-address>
scripts/target-run.sh runtime/tests/qmp_core_tests
```

Sanitizers on target: `-DQMP_SANITIZER=ubsan` (asan needs the shared runtime; no tsan on QNX —
use the host tsan preset).

## Repository layout

```
idl/                  qmp/options.proto (ext. 50000–50999), examples/demo/joint
third_party/up-spec/  pinned uProtocol protos (v1.5.8, Apache-2.0) — see PIN.md
codegen/              protoc-gen-qmp (Python ≥ 3.10, Jinja2) + golden tests
include/qmp/          public C ABI headers
runtime/core/         libqmp, OS-independent C11
runtime/transport/    fake (W1), qnx (W2), uds (W3, bench only)
examples/joint_demo/  actuator service in C (--selftest on host)
cmake/                presets support, qnx-*.cmake toolchains, QmpCodegen.cmake
docs/                 DESIGN, DECISIONS (ADRs), VERIFY register, codelab, spikes
```

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). Vendored uProtocol specs are
Apache-2.0 (Eclipse Foundation).
