Benchmarks, then the demo, then stretch goals in priority order. Post a plan first and wait for
my OK.

Benchmarks (bench/): same IDL over the qnx and uds transports on the same RPi4, using the C API
(plus one C++-wrapper run to show its overhead). Output CSV plus a Python plot script.
- RTT p50/p99/p99.9/max at 16 B, 1 KB, 16 KB and 64 KB; 100k iterations after warm-up.
- Jitter under CPU load; throughput; CPU cost per call.
- PRIORITY INVERSION scenario:
  - one 1 kHz high-priority client
  - K low-priority flood clients
  - one medium-priority CPU hog
  Report the high-priority client's latency distribution for both transports.
- Every CSV header records: SDP version and patch level, image, CPU governor, priorities,
  runmasks, compiler flags.

Demo (examples/joint_demo/):
- Actuator service in C (SetTarget / Stop / GetState) and StateChanged topic at 500 Hz.
- High-priority control loop in C++ and a low-priority dashboard flooder.
- Live latency readout; transport selectable by flag.
- Scripted kill/restart of the server.

Stretch 1 — uProtocol adapter (adapters/uprotocol/), based on the W1 spike outcome:
- UTransport send / register_listener / unregister_listener over the C ABI
- UPriority table (ADR-007 or a new ADR), UCode mapping, UUri ⇄ name mapping
- example: a uProtocol-style client calling Joint.GetState and subscribing to StateChanged over
  QNX native IPC
- adapter latency reported SEPARATELY and clearly labelled
- docs/codelab/04-uprotocol-over-qnx-ipc.md
Stretch 2 — a shm bulk method using revocable, sealed NOFD handles.
Stretch 3 — secpol generation from qmp.allow_types, plus a demo of a denied client.

Before we publish any numbers, remind me to check the QNX Everywhere license terms on publishing
benchmarks.