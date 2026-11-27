# Changelog

All notable changes to qmp are recorded here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html) as set out in
[ADR-011](docs/DECISIONS.md) and the [Versioning](README.md#versioning) section of the README.

One version covers the runtime (`libqmp`), the generator (`protoc-gen-qmp`) and `qmpctl`.
Each release also states its wire-protocol and reflection versions: two processes can talk
when their wire versions are equal, whatever their product versions are.

While the version is 0.y.z, a breaking change bumps the minor number.

## [Unreleased]

## [0.3.0] - 2026-11-27

Wire version 1 · reflection version 1

### Added
- Topics with notify-then-pull delivery over registered events: `LATEST` and `QUEUE` modes,
  loss accounting, resubscription after a server restart (ADR-003).
- UDS transport over `AF_UNIX`/`SOCK_SEQPACKET` for benchmarks and host tests, also built on
  QNX. It has no priority inheritance (ADR-004).
- `qmpctl` with `list`, `describe`, `call` and `subscribe`, driven only by reflection data
  that every generated service serves through the new `QMP_OP_REFLECT` control opcode
  (ADR-010).
- Semantic versioning (ADR-011): `include/qmp/qmp_version.h` as the single source of the
  version, `QMP_VERSION_STRING`, `QMP_VERSION_HEX`, `QMP_VERSION_AT_LEAST()` and
  `QMP_WIRE_VERSION`; `qmp_version_string()`, `qmp_version_number()`, `qmp_wire_version()`,
  `qmp_wire_version_compatible()`, `qmp_build_id()` and `qmp_build_version()`.
- HELLO advertises the wire version in both directions and the server's runtime version in
  the reply, in bytes that were zeroed padding before. `qmp_client_server_wire_version()`
  and `qmp_client_server_runtime_version()` read them. A peer that sends 0 is a pre-0.3.0
  build and is treated as wire version 1, so this stays compatible with 0.2.0.
- `qmpctl --version` reports the wire and reflection versions and has a `--json` form;
  `qmpctl describe` shows the runtime, wire version and generator of the remote service.
- Generated headers refuse to compile against a runtime of another minor version.
- `scripts/check-version.sh` and a tag-triggered release workflow.

### Changed
- `protoc-gen-qmp` takes its version from the product version instead of its own number.
- `qmp_hello_t._pad0[2]` is now `wire_version` and `_pad0`; `qmp_hello_reply_t._pad0[2]` is
  `wire_version` and `_pad0`, and `_pad1` is `runtime_version`. Sizes and offsets of all
  other fields are unchanged.

## [0.2.0] - 2026-11-16

Wire version 1

### Added
- Native QNX message-passing transport: `name_attach`, `MsgSendv`, unblock and disconnect
  pulses, with the receiving thread inheriting the caller's priority (ADR-009).
- Deferred replies from a fixed pool, one-way calls, client timeouts with in-flight
  cancellation, and client and server death handling with reconnect.
- `acl_check` hook on kernel-provided client credentials.
- Two-process target scenarios in `scripts/target-smoke.sh`, run on the Raspberry Pi 5
  (ADR-008).

## [0.1.0] - 2026-11-09

Wire version 1

### Added
- `protoc-gen-qmp` for synchronous methods: validated schema to C11 fixed layouts with
  static asserts, ids, UUri constants, client stubs and the server ops table (ADR-001,
  ADR-002).
- C11 runtime core with a stable C ABI and caller-provided storage: header codec, UCode
  mapping, dispatch and the HELLO version handshake (ADR-006).
- In-process fake transport for host tests.

0.1.0 and 0.2.0 were not tagged: their sources all report 0.1.0. The entries describe the
state of `main` after the W1 and W2 merges (commits `8fdc41116eb0` and `d924da1f1be8`).

[Unreleased]: https://github.com/miketsukerman/qmp/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/miketsukerman/qmp/releases/tag/v0.3.0
