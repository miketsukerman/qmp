# Raspberry Pi 5 bring-up (primary target board)

The Raspberry Pi 5 is the primary QNX SDP 8.0 target board (ADR-008). The Raspberry Pi 4 is
the secondary board, used for one portability and benchmark pass in the final week. x86_64
under QEMU is for functional development only — never for headline latency numbers.

Everything stated here about the Pi 5 image that has not been confirmed on the real board is
tracked in `docs/VERIFY.md` (V-QNX-026, V-QNX-027, V-QNX-028, V-QNX-029, V-QNX-030).

## Hardware

- Raspberry Pi 5.
- Official Raspberry Pi Active Cooler — **required for benchmarks** (thermal throttling
  invalidates latency numbers; see V-QNX-028).
- Official 27 W USB-C power supply (an underpowered supply can trigger frequency capping).
- microSD card for the target image.
- Ethernet connection to the host (SSH/scp deployment; see `scripts/target-run.sh`).

## Image

1. In QNX Software Center, install the package `com.qnx.qnx800.quickstart.rpi5`
   (QNX SDP 8.0 Quick Start Target Image for Raspberry Pi 5).
2. Flash the image to the microSD card following the package's instructions.
3. Update the host SDP 8.0 packages so the target libraries on the image match the SDK used
   for cross-compiling. Whether the shipped image matches the host SDP patch level is
   V-QNX-026 — record both versions in the baseline table below.

## Build and run

```sh
source ~/qnx800/qnxsdp-env.sh
cmake --preset qnx-aarch64le
cmake --build build/qnx-aarch64le
```

Deploy and run binaries with the helper script (copies with `scp`, runs with `ssh`,
propagates the exit code):

```sh
export QMP_TARGET=qnxuser@<pi5-address>     # required
# optional: QMP_BUILD_DIR (default build/qnx-aarch64le), QMP_TARGET_DIR (default /tmp/qmp)
scripts/target-run.sh runtime/tests/qmp_core_tests
```

## Baseline record

Fill in by hand after first boot; update when the image changes.

| item | value |
|------|-------|
| Image build ID / package version | TBD |
| `uname -a` | TBD |
| Host SDP level (`cat $QNX_TARGET/etc/qversion`) | TBD |
| CPU count and clock (`pidin info`) | TBD |
| Security policy active? (V-QNX-027) | TBD |
| Date | TBD |

## Benchmark conditions

Every recorded benchmark run on the Pi 5 must satisfy:

- Active cooler fitted and working.
- CPU frequency fixed (not scaling) for the duration of the run.
- Throttling checked **before and after** each run; discard the run if throttled.

The exact QNX 8.0 method for pinning the CPU frequency and for checking
frequency/throttling state on the Pi 5 is unverified — see **V-QNX-028** in
`docs/VERIFY.md`. Until it is confirmed, cross-check `ClockCycles()` against
`clock_gettime(CLOCK_MONOTONIC)` around each run (V-QNX-016, V-QNX-030) and record the
result with the benchmark CSV.

No CPU tuning flags (`-mcpu=cortex-a76`) are enabled; that is an open item (V-QNX-029).
