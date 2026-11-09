You are my senior embedded systems engineer and pair programmer on "qmp", my individual entry for
the QNX Global Hackathon 2026. First read .github/copilot-instructions.md and every file in
.github/instructions/, and follow them strictly.

## Background (completed research)
- Closest prior art is Eclipse S-CORE communication (LoLa). It is ara::com-style, uses shared
  memory for data and QNX message passing only for tiny notifications, and is Bazel-based.
- Other QNX-ported middleware (vsomeip, ROS 2/DDS, gRPC, iceoryx) is socket- or shm-based.
- Ferrous Systems has a low-level Rust MsgSend wrapper for 8.0, with no IDL or codegen.
- Eclipse uProtocol is a transport-agnostic stack. Its existing transports include Zenoh, MQTT 5
  and Android Binder. There is none for QNX message passing.
- The gap we fill: IDL→bindings whose transport IS QNX message passing (priority inheritance,
  registered events, shm handles, secpol), with a C11 core and C ABI, uProtocol-compatible
  service definitions, and an optional uTransport binding.
- The service model follows SOME/IP and uProtocol (methods, one-way calls, topics). The wire
  header is inspired by Fuchsia FIDL.

## Example IDL to design around (VERIFY(up) option names against the pinned up-spec tag)
```proto
syntax = "proto3";
package demo.joint.v1;
import "uprotocol/uoptions.proto";
import "qmp/options.proto";

message Empty {}
message SetTargetRequest { float position_rad = 1; float max_velocity = 2; }
message SetTargetReply  { int32 result = 1; }
message JointState {
  uint64 seq = 1; uint64 timestamp_ns = 2; float position_rad = 3; float velocity = 4;
  string mode = 5 [(qmp.max_len) = 16];
}

service Joint {
  option (uprotocol.service_id) = 0x0101;
  option (uprotocol.service_name) = "demo.joint.Joint";
  option (uprotocol.service_version_major) = 1;
  option (uprotocol.service_version_minor) = 0;
  option (uprotocol.publish_topic) = { id: 0x8001, name: "StateChanged", message: "JointState" };
  option (qmp.topic_mode) = { topic_id: 0x8001, mode: LATEST };

  rpc SetTarget(SetTargetRequest) returns (SetTargetReply) {
    option (uprotocol.method_id) = 1;
    option (qmp.allow_types) = "joint_ctrl_t";
  }
  rpc Stop(Empty) returns (Empty) {
    option (uprotocol.method_id) = 2;
    option (qmp.oneway) = true;
  }
  rpc GetState(Empty) returns (JointState) { option (uprotocol.method_id) = 3; }
}
```

## Timeline (today is 2026-11-09)
- W1 (Nov 9–15): scaffold, pin up-spec, codegen for C sync methods, C core + fake transport,
  uProtocol build spike, SDP patch-level check (C++17 vs C++20).
- W2 (Nov 16–22): QNX transport — sync, one-way, deferred, handshake, versioning, lifecycle.
  C++ wrappers.
- W3 (Nov 23–29): topics (notify-then-pull), UDS transport, qmpctl.
- W4 (Nov 30–Dec 6): benchmarks including priority inversion, joint demo on RPi4.
  Stretch: uProtocol adapter, shm bulk, secpol generation.
- W5 (Dec 7–13): hardening, docs, codelab, video script, write-up. Demo-ready Dec 10.
- Buffer: Dec 14–15.

## Your tasks now — do NOT write implementation code yet
1. Ask up to 6 questions whose answers would change the design, then wait for my answers.
2. Draft docs/DESIGN.md:
   a. Component diagram (ASCII): host codegen; per-process layers (user code, optional C++
      wrapper, generated C bindings, libqmp C core); the kernel; qmpctl, the uProtocol adapter
      and the UDS transport.
   b. ASCII sequence diagrams for: connect + HELLO; sync call; deferred reply with client
      timeout; one-way call; subscribe + MsgDeliverEvent + pull; client crash; server crash +
      reconnect; uProtocol REQUEST/RESPONSE through the adapter.
   c. Generated API sketches for Joint: the C header (structs with explicit padding, client
      functions, ops table, deferred reply, publish/subscribe) and the C++ wrapper (Proxy,
      Skeleton, Result, RAII).
   d. Threading and priority model, worked through with three clients at different priorities,
      including how the adapter preserves UPriority.
   e. Error model: errno vs. UCode, with the mapping table.
   f. Identifier mapping table: UUri ⇄ qmp ids ⇄ QNX name.
   g. Explicit v1 non-goals.
3. Seed docs/VERIFY.md with every QNX 8.0 and uProtocol assumption, each with a test procedure.
4. Create docs/DECISIONS.md with these ADRs:
   - 001 Protobuf as schema-only IDL
   - 002 fixed-layout wire format
   - 003 notify-then-pull topics
   - 004 UDS transport for benchmarks only
   - 005 uProtocol alignment (pinned tag, reused options, adapter scope)
   - 006 C11 core with C ABI and a header-only C++ layer
   - 007 C++ standard level (C++17 default; C++20 only with SDP ≥ 8.0.2)
5. Propose the repo scaffold: full tree, CMake layout, toolchain files, CI outline, lint
   configs, pyproject.toml.
6. Write a risk register: the top 8 risks with likelihood, impact, mitigation and target week.

After I approve, implement W1 only, in small commits, building and testing after each step and
reporting results honestly.