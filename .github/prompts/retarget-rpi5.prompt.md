---
mode: agent
description: Retarget qmp from Raspberry Pi 4 to Raspberry Pi 5 as the primary QNX 8.0 board
---

# Task: make Raspberry Pi 5 the primary target board for qmp

## Context
qmp currently names the Raspberry Pi 4 as its QNX SDP 8.0 target. The project is
switching to a **Raspberry Pi 5** running the official QNX SDP 8.0 Quick Start
Target Image for Raspberry Pi 5 (package `com.qnx.qnx800.quickstart.rpi5`).
The Pi 4 becomes a **secondary** board, used only for a final portability and
benchmark pass. x86_64 (QEMU) remains the functional-development fallback.

Both boards are aarch64le, so the existing `qnx-aarch64le` preset and toolchain
file stay as they are. This is a documentation, instructions and tooling change.
It is NOT a code change.

## Hard constraints
- Do not change any C/C++ source, generated code, codegen logic, compiler flags,
  or CMake build behaviour. In particular, do NOT add `-mcpu=cortex-a76` or any
  other CPU tuning. Record it as an open VERIFY item instead.
- Do not state anything about the Pi 5 QNX image as fact unless it is already
  in the repo. Anything unconfirmed goes into `docs/VERIFY.md` using the
  existing entry format and ID scheme (follow what is already in that file;
  use the `VERIFY(qnx8)` tag convention from the instructions).
- Do not delete Pi 4 information. Reword it as "secondary board".
- Keep the always-on instructions file short. Put details in docs and reference them.

## Steps

1. **Find every reference.** Search the whole repo (including `.github/`,
   `docs/`, `cmake/`, `examples/`, `runtime/`, `codegen/`, README) for:
   `Pi 4`, `Pi4`, `RPi4`, `rpi4`, `Raspberry Pi 4`, `BCM2711`, `Cortex-A72`,
   `qnxuser@<rpi>`. List the hits before editing.

2. **Update the target statements.**
   - `README.md`: "QNX target build" section names the Raspberry Pi 5 running
     the QNX 8.0 Pi 5 quick-start image as primary; Pi 4 as secondary; QEMU for
     functional development only (never for headline latency numbers).
   - `.github/copilot-instructions.md` and any `.github/instructions/*.instructions.md`:
     one short line for the target hardware (Pi 5 primary, Pi 4 secondary,
     QEMU functional only) and a pointer to `docs/target/rpi5.md`.
   - `docs/DESIGN.md`, `docs/codelab/`, `docs/spikes/`, the prompt files under
     `.github/prompts/` (W2–W5 milestones, review/target-results prompts):
     replace primary-board references accordingly.
   - `docs/DECISIONS.md`: add a new ADR (next free number), "Raspberry Pi 5 as
     primary target board", status Accepted, date 2026-11-14. Rationale: newer
     official QNX 8.0 image, faster CPU, demo platform. Consequences: benchmark
     numbers are Pi 5 numbers; Pi 4 used for one portability/benchmark pass in
     the final week; active cooling required for measurements.

3. **Add `docs/target/rpi5.md`, the bring-up guide.** Sections:
   - Hardware: Pi 5, official active cooler (required for benchmarks), power
     supply, microSD, Ethernet to the host.
   - Image: install `com.qnx.qnx800.quickstart.rpi5` via QNX Software Center,
     flash it, and update the host SDP 8.0 packages so target libraries
     match the SDK used for building.
   - Build and run: `source ~/qnx800/qnxsdp-env.sh`,
     `cmake --preset qnx-aarch64le`, `cmake --build build/qnx-aarch64le`,
     then deploy and run with the script from step 4.
   - Baseline record: a table to fill in by hand, with image build ID,
     `uname -a`, CPU count and clock (`pidin info`), whether a security
     policy is active, date. Leave values as `TBD`.
   - Benchmark conditions: cooler fitted, fixed CPU frequency, throttling
     checked before and after each run. Mark the exact QNX method for pinning
     and checking frequency as a VERIFY item.

4. **Add `scripts/target-run.sh`** (POSIX sh, `set -eu`, shellcheck-clean):
   - Configured by env vars `QMP_TARGET` (required, e.g. `qnxuser@192.168.1.50`),
     `QMP_BUILD_DIR` (default `build/qnx-aarch64le`), `QMP_TARGET_DIR`
     (default `/tmp/qmp`).
   - `scripts/target-run.sh <relative-binary-path> [args...]` copies the binary
     with `scp`, runs it with `ssh`, and propagates the exit code.
   - Clear error if `QMP_TARGET` is unset or the binary is missing.
   - Update README and `docs/target/rpi5.md` to use it instead of raw `scp`/`ssh`,
     e.g. `scripts/target-run.sh runtime/tests/qmp_core_tests`.

5. **Add VERIFY entries** (existing format, status open):
   - Pi 5 quick-start image is built against the same SDP 8.0 level as the host SDK.
   - Whether the Pi 5 image runs with a security policy enabled (affects
     name_attach permissions and the secpol demo).
   - How to pin and check CPU frequency and detect thermal throttling on QNX 8.0 / Pi 5.
   - Whether `-mcpu=cortex-a76` tuning is supported by the SDP 8.0 toolchain and
     worth enabling (do not enable it now).
   - `ClockCycles()` rate on Pi 5 (`SYSPAGE_ENTRY(qtime)->cycles_per_sec`) for the benchmarks.

## Acceptance criteria
- `cmake --preset host-debug && cmake --build build/host-debug && ctest --test-dir build/host-debug`
  still passes all tests, and codegen `pytest`, `ruff`, `mypy` are unchanged and green.
- No `.c`, `.h`, `.cpp`, `.py` (except none), `.proto`, or CMake logic changed. Only
  docs, instructions, prompts, and the new script.
- A repo-wide search for the patterns in step 1 shows only intentional
  "secondary board" mentions of the Pi 4.
- `shellcheck scripts/target-run.sh` is clean.
- Work on branch `chore/target-rpi5`, one PR titled
  `chore: retarget primary QNX board to Raspberry Pi 5`, with the list of
  files changed and the new VERIFY IDs in the PR description.
