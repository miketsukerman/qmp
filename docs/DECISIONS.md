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
on pull, overflow reported as `DATA_LOSS`). Callbacks run on the thread that calls
`qmp_client_poll()`; libqmp creates no threads (Q3).

**Consequences.** + Server never blocks on a client; subscribers cannot slow publishers.
+ Pulse coalescing gives natural "latest value" semantics for control loops. + The pull is a
priority-inheriting `MsgSend`, so a high-priority subscriber is served first. − Two kernel calls
(pulse + pull) per delivered sample instead of one; measured in W4. − A large sample still costs
one copy per subscriber; shm handles (stretch) address bulk data.

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
   (`send`, `registerListener`, `unregisterListener`) for REQUEST/RESPONSE and PUBLISH on top of
   the C ABI, using up-cpp if it builds with `q++` (spike V-UP-009), otherwise a minimal
   self-contained `UMessage` struct. No uDiscovery/uSubscription/uTwin. UPriority is carried in
   `hdr.up_priority` and mapped to QNX priorities (DESIGN §5.4). The adapter is never the measured
   hot path.

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

**Consequences.** + One runtime to test and measure; C and C++ interop trivially (interop
matrix C↔C++). + No libc++ ABI exposure across process/library boundaries. − C++ ergonomics
limited to what header-only wrappers can express (no coroutines, `Result<T>` instead of
exceptions). − Some duplication of doc comments between `.h` and `.hpp` templates.

---

## ADR-007 C++ standard level: C++17 default; C++20 only with SDP ≥ 8.0.2

**Status:** proposed 2026-11-09 — finalised after spike `docs/spikes/sdp-level.md` (V-QNX-020)

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
