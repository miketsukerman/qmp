Review the current diff as a strict QNX real-time, C-API and uProtocol-conformance reviewer.
Check:
- QNX correctness:
  - send-hierarchy violations
  - locks held across MsgReply / MsgDeliverEvent / MsgSend
  - unchecked return values
  - untrusted-input bounds checks
  - allocations on the call path
  - use of discontinued 8.0 features (Qnet, APS, PPS, io-pkt, ARMv7, __QNXNTO__)
  - Linux-only APIs in QNX code paths
  - missing VERIFY markers
- C API:
  - ABI stability, caller-owned storage, errno contract
  - padding zeroed by senders
  - generated headers compiling as C11 and C++17
- C++ layer: no duplicated logic or structs, no exceptions on the call path, no C++20 features
  unless ADR-007 allows them.
- uProtocol:
  - id ranges (methods 0x0000–0x7FFF, topics 0x8000–0xFFFF)
  - UCode / UPriority mappings
  - drift from the pinned up-spec
- Hygiene: tests (including the interop matrix), determinism of generated code, SPDX headers.
Output: blocking issues first, then non-blocking ones, each with file:line and a concrete fix.