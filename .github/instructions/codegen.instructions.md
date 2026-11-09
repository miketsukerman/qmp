---
applyTo: "codegen/**,idl/**"
---
# protoc-gen-qmp rules (Python ≥ 3.10, Jinja2)

## Scope
- Protobuf is a schema language only. The plugin never emits varint encoders/decoders; payloads
  are fixed-layout C11 PODs shared byte-for-byte by C and C++.
- Descriptor modules for `uprotocol/uoptions.proto` and `qmp/options.proto` are checked in under
  `protoc_gen_qmp/_pb/` and regenerated only with `codegen/scripts/regen_pb.sh`.
- upb quirk: call `options.HasExtension(ext)` / `options.Extensions[ext]`; the ExtensionDict has
  no `HasExtension`. Extension lookups are typed `Any` behind `_opt_*` helpers in `model.py`.

## Validation (`validate.py`) — collect ALL errors, then fail
- Every message is `<file>:<line>: error: <text>`, using SourceCodeInfo locations. Sorted by
  file, line, text. Nothing is generated for a file with errors.
- Reject in v1: proto2 files/messages, `map<>`, `oneof`, `google.protobuf.Any` and every other
  well-known type, zero-field messages used as field types, recursive messages, embedding
  deeper than `MAX_NESTING` (4) levels, `string`/`bytes` without `(qmp.max_len)`, `repeated`
  without `(qmp.max_count)`, `max_len`/`max_count` on the wrong field kind or out of range,
  `(uprotocol.field_size)`/`(uprotocol.field_count)` disagreeing with the qmp option, streaming
  rpcs, missing/duplicate/out-of-range `service_id`, `service_version_major/minor`,
  `method_id` (0x0000–0x7FFF) and topic ids (0x8000–0xFFFF), topics without a name or with
  `resources`, `UServiceTopic.message` that does not resolve or is empty, `(qmp.oneway)` on a
  method whose reply is not a zero-field message, `(qmp.topic_mode)` on an undeclared topic or
  with a QUEUE depth outside 1..65535.
- Names shown to users drop protoc's leading dot (`err.Req`, not `.err.Req`).
- Every rule has a fixture `tests/golden/errors/<rule>.proto` + `<rule>.expected`. Add one when
  you add a rule.

## Layout (`layout.py`)
- Members in field-number order. `bool→uint8_t`, `enum→int32_t`, `int32/sint32/sfixed32→int32_t`,
  `uint32/fixed32→uint32_t`, 64-bit likewise, `float`, `double`. Nested messages inline.
- `string`/`bytes` → helper `struct { uint16_t len; char|uint8_t data[N]; }` named
  `<Msg>_<field>_t`; `repeated T` → `struct { uint32_t count; T items[N]; }`.
- Alignment = max member alignment; message structs padded to a multiple of 8 (`MSG_ALIGN`).
  Padding is explicit (`uint8_t _padN[k]`), senders zero it, receivers ignore it.
- Emit `QMP_STATIC_ASSERT` for sizeof and every member offset so the compiler proves the
  computed layout. Any change to the layout algorithm is a wire-protocol change → ADR.

## Emission (`emit.py`, `templates/*.j2`)
- Deterministic: sorted iteration, no timestamps, no absolute paths. Goldens are byte-exact.
- Output ≤ 100 columns (use the `wrap` filter for argument lists). Integer suffixes uppercase
  (`0x0101U`) so generated code passes `readability-uppercase-literal-suffix`.
- Headers must compile as C11 (`-std=c11 -Wall -Wextra -Wpedantic -Werror`) AND C++17; wrap in
  `extern "C"` and never use C-only constructs outside `#ifndef __cplusplus`.
- Naming: types `<pkg_with_underscores>_<Message>`; functions `<pkg>_<Service>_<Method>`;
  macros `<PKG>_<SERVICE>_{SERVICE_ID,VER_MAJOR,UURI,M_<METHOD>,T_<TOPIC>,UURI_<X>,...}`.
- Only sync methods get client stubs/trampolines in W1. One-way (W3) and topics (W2) are
  declared in the ops struct/macros but produce no stubs yet.
- Reflection JSON (`<svc>.reflect.json`, `qmp_reflect_version: 1`) is also embedded as a byte
  array plus `_len` in the `.c` file.

## Tests and lint
- `cd codegen && python3 -m pytest tests -q` (uses `grpc_tools.protoc`; no system protoc needed).
  Regenerate goldens intentionally with `UPDATE_GOLDEN=1` and review the diff.
- `ruff check . && ruff format --check . && mypy --strict protoc_gen_qmp tests` must be clean.
