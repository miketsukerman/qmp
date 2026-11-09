# qmp — Service-oriented communication over QNX native message passing (uProtocol-aligned)

## Mission
qmp is an individual entry for the QNX Global Developer Hackathon 2026
(deadline 2026-12-15 14:00 EST; demo-ready by 2026-12-10).

- Services are described in Protobuf (proto3), used as a SCHEMA LANGUAGE ONLY, annotated with
  Eclipse uProtocol service options plus a few qmp.* options.
- A Python protoc plugin (`protoc-gen-qmp`) generates C11 bindings (primary) and optional C++
  header-only wrappers over the SAME structs.
- The runtime `libqmp` is C11 with a stable C ABI. The transport is QNX native message passing
  (MsgSend/MsgReceive/MsgReply, registered events, pulses, shared-memory handles). It is NOT
  sockets, and protobuf varint encoding is NOT used on the hot path.
- An optional adapter exposes qmp as an Eclipse uProtocol uP-L1 UTransport binding.

The project must KEEP and SHOWCASE the properties of QNX IPC: synchronous semantics, priority
inheritance, determinism and low overhead. The native generated API is primary. uProtocol is an
interoperability layer and is never the measured hot path.

Hackathon categories: Best use of QNX-specific Technologies, Developer Tooling & Host Tools,
Open-Source Contribution, Codelab/Tutorial.

## Platform facts (QNX SDP 8.0) — treat as constraints
- Target: Raspberry Pi 4, aarch64le, QNX SDP 8.0 (QNX Everywhere image). Secondary target:
  x86_64 (QEMU/VM).
- Host: Ubuntu 22.04/24.04. Source qnxsdp-env.sh before building.
- Toolchain:
  - C: `qcc -Vgcc_ntoaarch64le` (x86_64: `-Vgcc_ntox86_64`)
  - C++: `q++ -Vgcc_ntoaarch64le_gpp` (x86_64: `-Vgcc_ntox86_64_gpp`)
  - Compiler is GCC 12.2. The C++ standard library is LLVM libc++ (16 in 8.0, 18 in 8.0.2).
- Language levels:
  - C11 for the core.
  - C++17 for the C++ layer by default. C++20 only if the project pins SDP ≥ 8.0.2 (record this
    in an ADR).
  - NEVER use C++20 modules (they are broken on QNX). Do not rely on C++23.
- Sanitizers on target: ubsan; asan and lsan are available on 8.0 with the shared runtime only.
  No tsan. Use tsan on the host build instead.
- DISCONTINUED in 8.0 — never use or suggest: Qnet (no cross-node message passing), Adaptive
  Partitioning Scheduler (APS), PPS/PiPS, io-pkt (the network stack is io-sock), ARMv7 targets,
  and the `__QNXNTO__` macro (use `__QNX__`).
- Naming: name_attach() registers under /dev/name/local/. Its flags argument is documented as
  unused in 8.0. Assume NO global name service unless verified.
- Notifications: the server calls MsgDeliverEvent() with sigevents that the client registered via
  MsgRegisterEvent(). Unregistered events are rejected with EACCES.
- Message types 0.._IO_MAX (511) are reserved. qmp message types must be > _IO_MAX.
- Priority inheritance is the default. Never set _NTO_CHF_FIXED_PRIORITY on service channels.
- Lifecycle signals:
  - _NTO_CHF_DISCONNECT fires once per CLIENT PROCESS, when its last connection goes away.
  - _NTO_CHF_COID_DISCONNECT on the client's own channel detects server death.
- Identity and access control:
  - ConnectClientInfoExt() returns pid, uid/gids and the security type id.
  - Channel-level access uses security policies: `allow <client_t> <server_t>:channel connect;`.
- Bulk data:
  1. shm_open(SHM_ANON), then shm_ctl to make the object revocable and sealed.
  2. shm_create_handle(fd, client_pid, ..., SHM_CREATE_HANDLE_OPT_NOFD).
  3. Send the handle in a MsgReply; the client maps it with mmap_handle().
  4. Revoke access with shm_revoke().
  Handles are single-use and per-PID, and the number outstanding is limited by
  RLIMIT_SHM_HANDLES_NP.

## Uncertainty protocol (IMPORTANT)
Training data covers QNX 8.0 and uProtocol thinly. When you are not certain that an API, flag,
option name, field number or behaviour is correct for QNX SDP 8.0 or for the pinned uProtocol
version:
1. Say so in chat.
2. Add `/* VERIFY(qnx8): ... */` or `/* VERIFY(up): ... */` at the use site.
3. Add an entry to docs/VERIFY.md: id, file:line, question, how to test, status.

Never invent APIs. Never use Linux-only APIs in QNX code paths: epoll, eventfd, inotify,
signalfd, memfd, SCM_RIGHTS, systemd, or the Linux /proc layout.

## IDL profile
- Vendor up-spec under third_party/up-spec at a PINNED tag (record tag and commit in ADR-005).
  Import "uprotocol/uoptions.proto". VERIFY(up) the path, option names and field numbers
  against that tag.
- Use uProtocol options for everything they cover:
  - service: service_id (16-bit), service_name, service_version_major/minor,
    service_permission_level, publish_topic / notification_topic (UServiceTopic,
    ids 0x8000–0xFFFF)
  - method: method_id (0x0000–0x7FFF), method_permission_level
- qmp-only options live in idl/qmp/options.proto, with extension numbers 50000–50999 (outside
  uProtocol's 51000–51699 block):
  - qmp.max_len / qmp.max_count — REQUIRED on every string/bytes/repeated field in v1
  - qmp.oneway — the method's reply must be Empty
  - qmp.topic_mode — LATEST or QUEUE{depth}
  - qmp.allow_types — secpol type names allowed to call the method
- Rejected in v1 with clear errors: maps, oneof, Any, unbounded fields, proto2, streaming rpc.

## Architecture (changes require an ADR in docs/DECISIONS.md)

    idl/                     qmp/options.proto, examples
    third_party/up-spec/     pinned uProtocol protos (Apache-2.0)
    codegen/                 protoc-gen-qmp (Python ≥3.10, Jinja2), templates, golden tests
    runtime/                 libqmp — C11, stable C ABI, no C++ runtime dependency
      core/                  OS-independent: header codec, dispatch tables, subscription
                             registry, versioning, errors. Builds and tests on the Linux host.
      transport/qnx/         native message passing (qcc only)
      transport/uds/         Unix-domain sockets; same API; benchmarks + host tests ONLY
      transport/fake/        in-process transport for unit tests
    include/qmp/             public C headers (qmp.h, qmp_client.h, qmp_server.h)
    include/qmp/cpp/         header-only C++ wrappers (Result, RAII, base classes)
    adapters/uprotocol/      uP-L1 UTransport on top of the C ABI (C++, STRETCH)
    tools/qmpctl/            CLI (C++, links only the C ABI)
    bench/                   latency / jitter / throughput / priority-inversion harness
    examples/joint_demo/     demo: actuator service in C, control loop in C++
    docs/                    DESIGN.md, DECISIONS.md, VERIFY.md, BENCHMARKS.md,
                             codelab/, spikes/

Generated output per service:
- `<svc>.qmp.h` — C11: ids (#define), POD payload structs with explicit padding and
  _Static_assert on size and offsets, client functions, server ops table, deferred-reply and
  publish functions. Wrapped in `extern "C"` guards; must compile as both C11 and C++17.
- `<svc>.qmp.c` — dispatch tables and stubs.
- `<svc>.qmp.hpp` — optional C++ wrappers over the SAME structs (no copies or conversions).
- `<svc>.reflect.json` — reflection data, also embedded in the binary as a byte array.
- `<svc>.secpol.txt` — optional security-policy snippet.

## Identifier mapping (uProtocol ⇄ qmp ⇄ QNX)
- service_id (16-bit) + instance_id (16-bit) ⇄ uProtocol ue_id.
  VERIFY(up): ue_id bit layout at the pinned tag.
- method_id or topic id ⇄ uProtocol resource_id. Methods: 0x0000–0x7FFF. Topics: 0x8000–0xFFFF.
- service_version_major ⇄ ue_version_major.
- QNX name: `/dev/name/local/qmp/<service_id hex4>-<instance_id hex4>/v<major>`,
  for example `qmp/0101-0001/v1`.

## Wire protocol v1
Native byte order (single node), 8-byte alignment. The header comes first, then the payload.

    typedef struct {            /* 24 bytes; _Static_assert size and every offset */
        uint16_t type;          /* QMP_MSG_TYPE = _IO_MAX + 0x100 */
        uint16_t flags;         /* ONEWAY, IS_REPLY, SHM_ATTACHED, APP_ERROR */
        uint16_t service_id;
        uint16_t instance_id;
        uint16_t resource_id;   /* method (<0x8000) or topic (>=0x8000) */
        uint8_t  ver_major;
        uint8_t  ver_minor;
        uint32_t payload_len;
        int32_t  status;        /* replies: uProtocol UCode value, 0 = OK. VERIFY(up) values */
        uint8_t  up_priority;   /* UPriority from the adapter; 0 = native call */
        uint8_t  reserved[3];   /* sender sets to zero; receiver ignores */
    } qmp_hdr_t;

- Transport and OS errors are returned with MsgError(rcvid, errno). Application errors go in
  `status` (UCode semantics).
- Payloads are generated POD structs with bounded fields. Strings are
  `struct { uint16_t len; char data[N]; }` and repeated fields are
  `struct { uint32_t count; T items[N]; }`. The generator emits explicit padding fields; senders
  zero padding and receivers ignore it.
- Handshake: the first message after name_open is QMP_HELLO
  {service_id, instance_id, ver_major, ver_minor}. The reply carries the server's version and a
  server_generation, or MsgError on a major-version mismatch. Minor versions are additive only.

## Communication patterns
- **Sync method:** MsgSendv → a server thread that inherits the client's priority → MsgReplyv.
- **Deferred reply:** the handler returns QMP_DEFERRED and keeps its `qmp_req_t*`, which stays
  valid until it is replied, cancelled (_PULSE_CODE_UNBLOCK) or the client disconnects. An
  optional on_cancel callback reports cancellation. Tolerate MsgReply failing because the client
  already unblocked.
- **One-way:** the server replies IMMEDIATELY on receipt, before doing the work, so priority
  inheritance still applies. Use MsgSendPulse only for payload-free signals.
- **Topics (notify-then-pull):**
  - The client owns a private channel created with _NTO_CHF_COID_DISCONNECT.
  - It registers an updateable SIGEV_PULSE with MsgRegisterEvent; the pulse value carries a
    sequence number.
  - It sends SUBSCRIBE {topic, event handle}.
  - The server calls MsgDeliverEvent; the client then pulls with MsgSend.
  - NEVER MsgSend from server to client.
- **Lifecycle:**
  - Server: on _PULSE_CODE_DISCONNECT, call ConnectDetach(scoid) and drop that client process's
    subscriptions, shm handles and deferred requests.
  - Client: on _PULSE_CODE_COIDDEATH or ESRCH/EBADF, reconnect with backoff, re-handshake and
    re-subscribe. A changed server_generation means the server restarted.
- **Access control:**
  - Channel level: secpol `channel connect` rules.
  - Method level: ConnectClientInfoExt, cached per scoid, checked against qmp.allow_types and
    method_permission_level. Document how permission levels map.

## C API rules
- Functions return EOK or an errno value. Application status (UCode) comes back via an
  out-parameter.
- All objects use caller-provided storage: client, server, subscription, deferred request
  (`qmp_client_t c; qmp_client_init(&c, ...)`). No malloc after init.
- Callbacks take `void *ctx` as their first argument. Document the thread and priority context of
  every callback.
- Generated naming: `<proto_package_underscored>_<Service>_<Method>`. Types keep their proto
  message names under the package prefix.

## Real-time and coding rules
- No heap allocation, blocking I/O or formatted logging on the call path after init.
- Preallocate receive buffers sized to the interface's largest message, so no extra MsgRead.
- Never hold a mutex across MsgReply, MsgDeliverEvent or MsgSend.
- Check every return value. All received data is untrusted: bounds-check before use.
- Logging: slog2 on target, stderr on host, behind a qmp_log macro.
- Server threading: N identical threads RECEIVE-blocked on one channel. Document how N is chosen.
- Timing: ClockCycles() with SYSPAGE cycles_per_sec on target (VERIFY on RPi4);
  clock_gettime(CLOCK_MONOTONIC) on host.

## Build, test, CI
- CMake ≥ 3.20 with cmake/qnx-aarch64le.cmake and cmake/qnx-x86_64.cmake toolchain files.
- Warnings:
  - C: -std=c11 -Wall -Wextra -Wpedantic -Werror
  - C++: -std=c++17 -Wall -Wextra -Werror
- Host build: core, fake and uds transports, C++ layer, qmpctl (uds), adapter unit tests,
  codegen golden tests. Run sanitizers on host: asan, ubsan, tsan.
- Tests:
  - C core: GoogleTest calling the C API via extern "C" (or a tiny C harness).
  - Codegen: pytest with golden files.
  - Interop matrix: C↔C, C↔C++, C++↔C, C++↔C++.
- Lint: clang-format, clang-tidy/cppcheck (C core), ruff + mypy (codegen).
- GitHub Actions: host build and tests on every push. Cross-compile only if an SDP can be
  provisioned legally in CI; otherwise document the manual target step in the README.

## Licensing
- Original code: Apache-2.0 with SPDX headers (same license as uProtocol).
- Allowed dependencies: protobuf (BSD-3), Jinja2 (BSD-3), GoogleTest (BSD-3),
  nlohmann/json (MIT), up-spec / up-cpp (Apache-2.0). Ask before adding anything else.
- Don't copy QNX documentation code beyond trivial API usage. Never commit QNX SDK files or
  target images.

## How to work with me
- Before any non-trivial change, post a plan (files, APIs, risks, tests) in ≤12 bullets and
  WAIT for my OK.
- Keep diffs small: one logical change per commit, Conventional Commits messages.
- After each step, build, run the tests and report honestly, including failures and target tests
  that were skipped.
- Record decisions in docs/DECISIONS.md. Each milestone adds one chapter to docs/codelab/.
- I run target steps manually: give me exact commands and the expected output. I will paste the
  results back.
- If a request conflicts with these instructions, say so and ask.