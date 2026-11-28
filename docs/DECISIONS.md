# Architecture decision records

Format: Context · Decision · Consequences · Status. New ADRs are appended; superseded ones are
marked, never deleted.

---

## ADR-001 Protobuf as a schema-only IDL

**Status:** accepted 2026-11-09

**Context.** We need an IDL that tool authors already know, that has a mature parser
(`protoc`) and plugin protocol, and that uProtocol already uses for its service options. We do
*not* want protobuf's varint wire encoding on a real-time hot path: it is variable-length,
requires per-field parsing, and does not map to fixed POD structs with static bounds.

**Decision.** Services are written in proto3 and compiled by `protoc` with the `protoc-gen-qmp`
plugin. The plugin reads the `FileDescriptorSet` and emits C11 fixed-layout structs; the
protobuf binary encoding is never used on the native path. Only a bounded subset is accepted
(ADR-002). The uProtocol adapter is the one place protobuf encode/decode runs.

**Consequences.** + Reuse of `protoc`, editor tooling, uProtocol options, and the existing
uProtocol service catalogue. + One IDL for native QNX and uP interop. − Users must learn the
rejected subset (maps, oneof, Any, unbounded fields). − Two representations of each message
exist when the adapter is used (POD and protobuf); the adapter owns the conversion.

---

## ADR-002 Fixed-layout wire format

**Status:** accepted 2026-11-09

**Context.** QNX message passing copies bytes between address spaces once, in the kernel. Any
serialisation step adds a second copy and parsing time, and makes payload size data-dependent.
Fuchsia FIDL demonstrates that an IDL can define a fixed in-memory layout that *is* the wire
format.

**Decision.** The wire format is: 24-byte `qmp_hdr_t` followed by a generated POD struct in
native byte order with 8-byte alignment (DESIGN §2). Layout rules: fields in field-number order,
explicit padding members, `_Static_assert` on size and every offset, bounded strings
(`{uint16_t len; char data[N]}`) and repeateds (`{uint32_t count; uint32_t _pad; T items[N]}`).
Every `string`/`bytes`/`repeated` must carry `qmp.max_len` / `qmp.max_count`. Zero-field
messages are transmitted as `payload_len = 0`. Client sends with two IOVs (header, user struct);
servers receive into one preallocated buffer sized to the largest message (Q1).

**Consequences.** + Zero serialisation; the user's struct is the message. + Worst-case message
size is known at compile time (`*_MAX_MSG_SIZE`), enabling static receive buffers. + Layout is
identical for C and C++ so the C++ layer needs no conversions (ADR-006). − Single-node only (no
byte-order negotiation) — acceptable, Qnet is gone in 8.0. − Schema evolution is additive-only
within a major version; any layout change is a major bump.

---

## ADR-003 Notify-then-pull topics

**Status:** accepted 2026-11-09

**Context.** Pub/sub must respect the QNX rule that a server never `MsgSend`s to a client
(deadlock and priority-inversion risk). The kernel-provided mechanism for server→client
signalling is `MsgDeliverEvent()` with client-registered events (mandatory registration via
`MsgRegisterEvent()` in 8.0). Pulses carry only a 32-bit value and 8-bit code, so payloads
cannot ride on them.

**Decision.** A subscription is: client registers an updateable `SIGEV_PULSE` on its own private
channel → sends `SUBSCRIBE{topic, handle}` → server stores `(scoid, handle)` and, on publish,
copies the sample into the topic's slot/ring and calls `MsgDeliverEvent` with the sequence number
as pulse value → client pulls the sample with `PULL` (a normal `MsgSend`). `qmp.topic_mode`
chooses `LATEST` (single slot, coalescing) or `QUEUE{depth}` (ring, `remaining` count returned
on pull, overflow reported as a `dropped` count, see the amendment). Callbacks run on the thread that calls
`qmp_client_poll()`; libqmp creates no threads (Q3).

**Consequences.** + Server never blocks on a client; subscribers cannot slow publishers.
+ Pulse coalescing gives natural "latest value" semantics for control loops. + The pull is a
priority-inheriting `MsgSend`, so a high-priority subscriber is served first. − Two kernel calls
(pulse + pull) per delivered sample instead of one; measured in W4. − A large sample still costs
one copy per subscriber; shm handles (stretch) address bulk data.

**Amendment (2026-11, W2 implementation).** Three details changed when the design met the
kernel API:

1. *Event blob, not handle.* 8.0's `MsgRegisterEvent(&ev, coid)` rewrites the `sigevent`
   **in place**; there is no separate int32 handle to send. SUBSCRIBE therefore carries the
   whole registered `sigevent` as an opaque, bounded blob (`event[64]`, V-QNX-042) that the
   server hands verbatim to `MsgDeliverEvent`. The core never interprets it, which also lets
   the fake transport use its own registration record.
2. *Cursor-keyed PULL.* PULL sends `{topic_id, after_seq}` instead of `{sub_id, max}` and the
   reply gained a 64-bit `dropped` count. The pulse value (updateable, V-QNX-045) is advisory
   only: pulses can coalesce or fail to queue (V-QNX-044), so `qmp_client_poll` drains every
   subscription and the `after_seq` cursor is the single source of truth. Loss is reported,
   never hidden: LATEST counts overwrites, QUEUE counts ring overflow, both per pull and in
   the callback.
3. *Caller-provided storage.* Topic slots/rings and the subscriber table live in
   `qmp_server_cfg_t.topicbuf` sized by the generated `*_TOPIC_STORAGE_SIZE`
   (`QMP_TOPIC_STATE_SIZE` + `QMP_SUBTABLE_SIZE`); bounds are explicit (`EAGAIN` when the
   subscriber table is full) and `topicbuf == NULL` disables topics (`ENOTSUP`). Publish
   snapshots the subscriber list under the lock and delivers events outside it; a dead
   subscriber (`ESRCH`/`EACCES`/`EBADF` from deliver) is dropped on the spot, and
   `_PULSE_CODE_DISCONNECT` cleanup removes all of a process's subscriptions. Registrations
   die with the connection, so reconnect re-registers and re-subscribes; old sub ids are
   rejected by the new generation (V-QNX-046).

---

## ADR-004 UDS transport for benchmarks and host tests only

**Status:** accepted 2026-11-09

**Context.** The core must be testable in CI on Linux without a QNX kernel, and the benchmark
write-up needs a credible "sockets vs. message passing" comparison on the *same* API and payloads.
A Unix-domain-socket transport gives both — but it must not become a supported production path,
since it cannot provide priority inheritance, registered events or secpol.

**Decision.** `runtime/transport/uds/` implements the same internal transport interface over
`AF_UNIX`/`SOCK_SEQPACKET`. It builds on the Linux host (and optionally on QNX via io-sock for a
same-box comparison). It is excluded from the public API docs, marked `QMP_TRANSPORT_UDS_BENCH_ONLY`
in CMake, and does not implement topics with event semantics beyond what the benchmark needs.
`transport/fake/` (in-process) is the unit-test transport.

**Amendment 2026-11-27 (W3, implemented).** The transport is built by default on QNX as well
(`QMP_TRANSPORT_UDS=ON`, links `libsocket`), because the W4 priority-inversion benchmark compares
both transports on the same board. Native message passing stays `qmp_transport_default()` on QNX;
on the host the default is now UDS, so examples and integration tests cross real process
boundaries. `QMP_TRANSPORT_UDS_BENCH_ONLY` is the PUBLIC compile definition that makes
`qmp_transport_uds()` visible. What the transport deliberately does **not** have:

* **No priority inheritance.** A socket carries bytes, not scheduling context: the server thread
  runs at its own priority whatever the waiting client's priority is, `qmp_rcv_info_t.priority`
  is always 0 and `QMP_LISTEN_FIXED_PRIORITY` is a no-op. This is the property the benchmark
  demonstrates, not a bug to fix.
* No kernel unblock: timeouts are local to the client, which sends a best-effort CANCEL record
  and discards a late reply by call id. No `receive_pulse`, so cancellation waits for an idle
  worker.
* No registered events or secpol: notifications are small records on a second socket of the same
  client, dropped when its buffer is full (like a pulse that cannot queue). Topics work because
  correctness comes from the pull cursor (ADR-003).
* The pid is kernel-supplied only on Linux (`SO_PEERCRED`); elsewhere it is self-declared.

**Consequences.** + Full CI on GitHub-hosted runners, including asan/ubsan/tsan. + Apples-to-apples
latency/jitter numbers. − Some code paths (lifecycle pulses, inheritance) are QNX-only and can be
exercised only in manual target runs; those are flagged in test reports as "skipped on host".

---

## ADR-005 uProtocol alignment: pinned tag, reused options, adapter scope

**Status:** accepted 2026-11-09 (pin), adapter scope = stretch

**Context.** Eclipse uProtocol defines service identity (service id, instance, version,
resource id) and Protobuf options for annotating services, plus a transport abstraction
(uP-L1 `UTransport`). Aligning with it makes qmp services addressable from any uP client and gives
us a well-specified identifier model. Its existing transports (Zenoh, MQTT 5, Binder) have no QNX
message-passing binding.

**Decision.**
1. **Pin:** `third_party/up-spec` is vendored from `eclipse-uprotocol/up-spec` at tag
   **`v1.5.8`**, commit **`cab833880c2f1d7965efd507c06446358e2c949f`** (2024-05-28, the latest
   stable tag; only `v1.6.0-alpha.*` exist beyond it — `v1.6.0-alpha.7` = `347d68438afd9e4813193b243dc98fb42963ad99`,
   2025-10-23). `uoptions.proto` option numbers are identical in both, so upgrading to 1.6.0 when
   released is a one-line bump. Only `up-core-api/uprotocol/**` and `LICENSE`/`NOTICE` are
   vendored. License: Apache-2.0.
2. **Reused options** (import `"uprotocol/uoptions.proto"`, package `uprotocol`):
   `service_id`=51100, `service_name`=51101, `service_version_major`=51102,
   `service_version_minor`=51103, `service_permission_level`=51104, `publish_topic`=51105,
   `notification_topic`=51106 (both `UServiceTopic{id, name, message, resources, permission_level}`),
   `method_id`=51200, `method_permission_level`=51201. Verified 2026-11-09 (V-UP-001).
3. **qmp options** live in `idl/qmp/options.proto`, package `qmp`, extension numbers
   50000–50999: `max_len`=50001, `max_count`=50002 (FieldOptions); `oneway`=50010,
   `allow_types`=50011 (repeated string, MethodOptions); `topic_mode`=50020 (repeated
   `TopicMode{topic_id, mode, depth}`, ServiceOptions). The uProtocol fixed-size options
   `field_size`/`field_count`/`message_size` are *not* reused for bounds because their semantics
   differ (SOME/IP: string size includes NUL/BOM; integer narrowing); the generator errors if
   both families are present and disagree (V-UP-010).
4. **Identifier mapping** per DESIGN §7. The default instance id is `0x0000` as in up-spec, so
   the default QNX name is `qmp/0101-0000/v1` (deviates from the brief's `0101-0001` example).
5. **Adapter scope (stretch, W4):** `adapters/uprotocol/` implements uP-L1 `UTransport`
   (`send`, `registerListener(sourceFilter, sinkFilter, listener)`, `unregisterListener`) for
   REQUEST/RESPONSE and PUBLISH on top of the C ABI with a minimal self-contained `UMessage`
   struct. **Spike outcome (2026-11-09, `docs/spikes/uprotocol-qnx.md`, V-UP-009): NO-GO for
   linking up-cpp on target** — up-cpp 1.0.1 pins up-core-api 1.6.0-alpha (not 1.5.8), needs
   full libprotobuf + spdlog cross-built via Conan, and upstream's QNX 8.0 profile is marked
   "in progress". An optional host-only bridge (`QMP_ADAPTER_USE_UPCPP`, default OFF) may be
   added for interop demos. No uDiscovery/uSubscription/uTwin. UPriority is carried in
   `hdr.up_priority` and mapped to QNX priorities (DESIGN §5.4). The adapter is never the measured
   hot path.
6. **Vendoring done (W1):** `third_party/up-spec/PIN.md` records tag/commit and the re-vendor
   recipe. Differences found against the project brief while confirming the pin:
   * `UCode` is defined in `uprotocol/v1/ustatus.proto`, not a `ucode.proto` (V-UP-003).
   * `ue_id`: service id is the **low** 16 bits, instance the **high** 16 bits
     (`basics/uri.adoc`); wildcard instance in the spec is `0xFFFF` (`0xFFFF_0000` mask), and the
     brief's default instance `0001` is replaced by the spec default `0000` (V-UP-002).
   * URI path uses upper-case hex with leading zeros omitted (`/101/1/8001`), not zero-padded
     hex4 (V-UP-006).
   * uProtocol UUIDs are UUIDv8, not v7 (V-UP-007).
   * `UServiceTopic` has fields `id=1, name=2, message=3, resources=4, permission_level=5`; v1
     rejects `resources` (V-UP-008).
   * Default message priority when unset is CS1; REQUESTs must be ≥ CS4 (V-UP-004).
   * `uoptions.proto` in the vendored tree has CRLF line endings; `.gitattributes` normalises
     them to LF.

**Consequences.** + Any uP-annotated proto compiles with qmp unchanged (if within the subset).
+ Clear path to uP interop without affecting native performance. − Pinning to v1.5.8 means
`v1.6` additions are not available until re-pinned. − Two option families for size bounds; the
generator validates consistency.

---

## ADR-006 C11 core with stable C ABI; header-only C++ layer

**Status:** accepted 2026-11-09

**Context.** Target integrators range from bare-C safety code to modern C++ applications and
tools in other languages (Rust via FFI). The C++ standard library on QNX changed between SDP
patch levels (libc++ 16 → 18), so a C++ ABI across binaries is fragile. Hackathon time is short:
one implementation must serve both languages.

**Decision.** `libqmp` is C11 (`-std=c11 -Wall -Wextra -Wpedantic -Werror`) with a stable C
ABI: errno-return convention, `int32_t *status` out-param for UCode, `void *ctx`-first
callbacks, caller-provided opaque storage with `QMP_*_STORAGE_SIZE` constants. Generated
bindings are C11 and compile as C++17 (extern "C" guards). The C++ layer
(`include/qmp/cpp/`, generated `*.qmp.hpp`) is header-only, C++17, exceptions- and RTTI-free, and
wraps the *same* POD structs (type aliases, no conversion). `qmpctl` and the adapter link only
the C ABI.

**Status of the C++ layer (2026-11-28).** Not implemented: `include/qmp/cpp/` and the
`*.qmp.hpp` generator output do not exist yet. C++ code uses the C bindings, which compile as
C++17.

**Consequences.** + One runtime to test and measure; C and C++ interop trivially (interop
matrix C↔C++). + No libc++ ABI exposure across process/library boundaries. − C++ ergonomics
limited to what header-only wrappers can express (no coroutines, `Result<T>` instead of
exceptions). − Some duplication of doc comments between `.h` and `.hpp` templates.

---

## ADR-007 C++ standard level: C++17 default; C++20 only with SDP ≥ 8.0.2

**Status:** proposed 2026-11-09 — finalised after spike `docs/spikes/sdp-level.md` (V-QNX-020).
**Proposed outcome (W1 spike):** accept as written — stay on C++17, no `QMP_CXX20` in the
hackathon timeframe; the spike adds V-QNX-025 (`_gpp` variant vs. plain `gcc_nto<arch>` and
libc++ selection) which must be answered before the first target build.

**Context.** QNX SDP 8.0 ships GCC 12.2 with libc++ 16; SDP 8.0.2 ships libc++ 18. C++20
library features (`std::span`, `<format>`, `<ranges>`, `std::jthread`) are incomplete in libc++
16 and C++20 modules are broken on QNX. The hackathon judges will build on whichever 8.0 image
they have.

**Decision.** The C++ layer targets **C++17** (`-std=c++17 -Wall -Wextra -Werror`) and uses no
C++20 features. If the installed SDP is ≥ 8.0.2 *and* a feature is worth it, a single
`QMP_CXX20` CMake option may enable C++20 for `examples/` and `tools/` only — never for
`include/qmp/cpp/` or generated headers. Modules and C++23 are banned. `__QNX__` is the platform
macro.

**Consequences.** + Builds on every 8.0 patch level. + Generated headers stay usable by the widest
range of integrators. − `std::span`-like helpers must be provided by a tiny `qmp::Span`.
− Re-evaluate when the project pins SDP ≥ 8.0.2 (record outcome here).

---

## ADR-008 Raspberry Pi 5 as primary target board

**Status:** accepted 2026-11-14.

**Context.** The project originally named the Raspberry Pi 4 (QNX Everywhere image) as the
QNX SDP 8.0 target. QNX now ships an official SDP 8.0 Quick Start Target Image for the
Raspberry Pi 5 (package `com.qnx.qnx800.quickstart.rpi5`). Both boards are aarch64le, so the
`qnx-aarch64le` preset and toolchain file are unchanged.

**Decision.** The **Raspberry Pi 5** running the QNX 8.0 Pi 5 quick-start image is the primary
target board. Rationale: newer official QNX 8.0 image, faster CPU, demo platform. The
Raspberry Pi 4 becomes the secondary board, used for one portability and benchmark pass in the
final week. x86_64 (QEMU) remains the functional-development fallback and is never used for
headline latency numbers. Bring-up guide: `docs/target/rpi5.md`. No CPU tuning flags are added
(`-mcpu=cortex-a76` is an open item, V-QNX-029). Unverified image properties are tracked as
V-QNX-026/027/028/030.

**Consequences.** + Published benchmark numbers will be Pi 5 numbers (a `docs/BENCHMARKS.md`
recording the board is not written yet). + Demo runs on current official QNX hardware support. − Pi 4 gets only one
portability/benchmark pass in the final week. − Active cooling is required on the Pi 5 for all
benchmark measurements (thermal throttling, V-QNX-028).

## ADR-009 Native QNX transport semantics are the reference; the fake transport follows them

**Status:** accepted 2026-11-16.

**Context.** Milestone W2 implemented the native transport
(`runtime/transport/qnx/qnx_transport.c`) plus the lifecycle features that only make sense
against kernel semantics: one-way calls, deferred replies with a bounded preallocated table,
client call timeouts, unblock/disconnect handling, reconnect with generation tracking, and a
per-method ACL hook fed by kernel credentials. Three design questions had to be settled.

**Decision.**
1. *Receive info is widened, not duplicated.* `qmp_rcv_info_t` carries 64-bit `rcvid` and
   `pulse_value` (rcvid_t and pulse values are 64-bit on 8.0, V-QNX-033), the client's
   reply-buffer capacity `dstmsglen` (V-QNX-032) and kernel-supplied credentials
   (`uid`/`gid`/`type_id`/`cred_valid`). Every transport fills the same struct; the core never
   touches QNX headers.
2. *Lifecycle callbacks live in `qmp_server_cfg_t`, not in generated ops tables.*
   `acl_check`, `on_cancel` and `on_client_gone` are properties of the server instance, not of
   one service's methods; the generated ops tables stay pure per-method dispatch. Wiring
   per-method callbacks through codegen is deferred to the codegen milestone.
3. *The kernel is the reference implementation.* Wherever the fake transport's behaviour
   differed from documented 8.0 kernel behaviour, the fake was changed to match, never the
   core or the native transport: REPLY-blocked clients stay blocked on timeout until the
   server acts on the `_PULSE_CODE_UNBLOCK` pulse (a late normal reply after the pulse is a
   *successful* call, V-QNX-006); scoids are per connection; disconnect is delivered as an
   event after the last connection of a client process closes; pulses are delivered before
   messages.

**Consequences.** + The conformance suite (`qmp_core_tests`) exercises kernel semantics even
on the host, so a green host run predicts target behaviour. + No `#ifdef __QNX__` in the core.
− The fake transport is more complex (connection table, event queue). − `type_id` is 0 until
`ConnectClientInfoExt` with `_NTO_CLIENTINFO_GETTYPEID` is verified on target (V-QNX-014), so
`qmp.allow_types` checks cannot be enforced yet — only uid/gid ACLs work today.

---

## ADR-010 Services serve their own reflection data; qmpctl is reflection-only

**Status:** accepted 2026-11-27 (W3).

**Context.** `qmpctl` must work with any qmp service without being rebuilt for it (ADR-006: it
links only the C ABI). The generator already writes `<svc>.reflect.json` and embeds the same
bytes in the service binary, but nothing made them reachable: a tool on the target would have
needed the right JSON file for every running service, kept in step with each binary by hand.
There is no registry process and no global name service (DESIGN §8).

**Decision.**
1. *The service is the source of truth for its own interface.* `qmp_service_desc_t` gains
   `reflect_json`/`reflect_json_len` (generated code fills them) and the runtime answers a
   new control opcode `QMP_OP_REFLECT` with a window of the document, at most
   `QMP_REFLECT_CHUNK_MAX` (4096) bytes per call, replied directly from the constant blob.
   `qmp_client_reflect()` is the client side. No new message type, no change to existing
   opcodes or payloads: wire protocol v1 grows by one additive control opcode.
2. *`qmpctl` contains no generated code.* It discovers names from the transport's namespace,
   reads each service's reflection document, validates it (it is untrusted input) and
   converts JSON ⇄ fixed-layout payloads itself. `--reflect FILE` remains as the fallback for
   services that cannot describe themselves (hand-written descriptors, older runtimes).
3. *Tests keep the compiler as the oracle.* The codec is tested against the structs
   `protoc-gen-qmp` generated for the same proto, so reflection data and C layout cannot drift
   apart unnoticed.
4. *nlohmann/json 3.11.3 (MIT)* is the JSON library for tools, fetched at configure time and
   pinned by hash. `libqmp` itself stays dependency-free C11.

**Alternatives considered.** (a) File-only reflection (`--reflect` mandatory): no runtime
change, but unusable on a target where the files are not deployed, and silently wrong when a
file is stale. (b) A generated "introspection method" in every service: consumes a method id
per service and makes the IDL carry tooling concerns. (c) A registry daemon: a new single
point of failure for something `/dev/name/local` already provides.

**Consequences.** + `qmpctl list|describe|call|subscribe` work against any running generated
service with zero deployment. + The document a tool sees is by construction the one the binary
was built from. − Each service binary answers one more opcode; the blob (≈6 KiB for the Joint
example) was already embedded. − The interface description is readable by any client that may
connect to the channel; it is not covered by the per-method ACL hook (it reveals names and
layouts, never data). A deployment that considers this sensitive restricts `channel connect`
with secpol, or registers a descriptor without the blob. − Servers built before this ADR answer
`ENOSYS` and need `--reflect`.

---

## ADR-011 Semantic versioning: one product version, a separate wire version

**Status:** accepted 2026-11-27 (W3).

**Context.** The number 0.1.0 was typed by hand in four places (`CMakeLists.txt`, `qmp.h`,
`codegen/pyproject.toml`, `protoc_gen_qmp/__init__.py`) and had not moved since W1, there
were no tags and no changelog. "Wire protocol v1" existed only as a phrase in DESIGN §2 and
as the fixed value of `QMP_MSG_TYPE`. Nothing told a user whether two builds can talk, or
whether generated code fits the runtime it is compiled against.

**Decision.**
1. *One product version, SemVer 2.0.0,* for `libqmp`, `protoc-gen-qmp` and `qmpctl`
   together. They are released from one tree and generated code is tightly coupled to the
   runtime, so separate numbers would only add a compatibility matrix.
2. *`include/qmp/qmp_version.h` is the single source of truth.* It is checked in, not
   generated, so the generated code and consumers that do not use CMake see it with a plain
   `-Iinclude`. CMake parses it for `project(VERSION)`; `scripts/check-version.sh` (CI)
   compares it with the generator's `__version__`, the newest `CHANGELOG.md` section and,
   on a release, the tag.
3. *The wire protocol has its own integer, `QMP_WIRE_VERSION`,* covering the header layout,
   flag bits, control opcodes and their payloads, status codes and topic delivery
   semantics. Two processes can talk if and only if their wire versions are equal. HELLO
   carries it in both directions in a byte that every earlier build sent as zeroed padding;
   0 is read as 1. A mismatch is `ENOTSUP` at HELLO. The HELLO reply also carries the
   server's `QMP_VERSION_HEX` (0 = unknown) for tools.
4. *What moves which number.* The question is always whether something built against the
   previous release still compiles, links, talks and parses unchanged.

   | Surface | Major | Minor | Patch |
   |---------|-------|-------|-------|
   | C API (`include/qmp/`) | remove or rename; change a signature or the meaning of an error code | add a function, macro, enum value or option | fix behaviour to match the documented contract |
   | C ABI (ADR-006) | change size or layout of a public struct, including caller-provided storage | use bytes that were reserved and zeroed | — |
   | Wire protocol | anything that bumps `QMP_WIRE_VERSION` | new control opcode; new use of a reserved or padding byte | — |
   | Transports | remove one; change the `qmp/<svc>-<inst>/v<major>` naming or the `qmp_transport_t` vtable | add a transport or a transport option | fix without changing semantics |
   | Generator, generated code | previously generated code no longer compiles or links; generated names change | new generated functions, `.proto` options, reflection JSON fields | output changes that do not alter the generated API |
   | `qmpctl` | remove or rename a command or flag; change JSON output fields or exit codes | new command, flag or JSON field | wording of human-readable output |

   While the version is 0.y.z a breaking change bumps the *minor* (SemVer §4); additions
   and fixes go in the patch number. `runtime/core/internal.h`, the fake transport's
   internals, tests, examples and benchmark numbers are not versioned surfaces.
5. *Generated code is pinned to its runtime.* The generated header fails with `#error`
   unless the runtime has the same minor (0.x), or the same major and an equal or newer
   minor (from 1.0).
6. *Four numbers, each answering one question:* product version (which API, generator and
   tools?), wire version (can these two processes talk?), reflection version
   `qmp_reflect_version` (can this qmpctl read that description? ADR-010), and the
   per-service `major.minor` from the `.proto` (is this client compatible with that
   interface? DESIGN §7), which belongs to the service author.
7. *Releases.* An annotated tag `vX.Y.Z` on `main` whose name equals the header version;
   release candidates are `vX.Y.Z-rc.N`. `CHANGELOG.md` (Keep a Changelog) is written by
   hand and names the wire and reflection versions of every release. The tag makes CI
   publish a GitHub Release; the QNX build and the RPi5 run stay manual (no SDP in CI) and
   happen before the tag. Between releases the header keeps the last released number and
   `qmp_build_version()` adds SemVer build metadata (`0.3.0+4.g<hash>`); `qmp_build_id()`
   stays the plain commit hash that target logs are matched against.
8. *First release is 0.3.0,* one minor per completed weekly milestone (W1 → 0.1.0, W2 →
   0.2.0, W3 → 0.3.0); the two older ones exist only as changelog entries. 1.0.0 is
   reserved for the point where wire version 1 and the C ABI are declared frozen, which is
   not before the W4 benchmark and adapter work.

**Alternatives considered.** (a) Generate the header from `project(VERSION)`: one fewer
parse step in CMake, but generated code compiled outside CMake (CI does this) would need a
build directory on its include path. (b) A `VERSION` text file: the generator's packaging
cannot read a file above its own project root, so a consistency check would be needed
anyway. (c) Version the generator separately: a matrix nobody needs yet. (d) Put the wire
version in `qmp_hdr_t.reserved`: costs nothing either, but would be checked on every
message for a property that cannot change after HELLO. (e) Only define the constant: a
future wire v2 would then fail with a malformed-message error instead of a clear refusal.

**Consequences.** + Every binary and every log line can name its release
(`qmpctl --version`, the demo banners), and `qmpctl describe` shows what a remote service
was built with. + A future incompatible wire change is refused at connect time. + Stale
generated code is a compile error, not a runtime surprise. − The version must be bumped in
two files (header and generator) per release; CI catches a mismatch. − Each release needs a
changelog entry and a manual target run before tagging. − A 0.2.0 peer is indistinguishable
from a wire-v1 peer that chose to send 0; this is intended.
