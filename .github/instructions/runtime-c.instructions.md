---
applyTo: "runtime/**,include/qmp/*.h,examples/**/*.c,bench/**/*.c"
---
# C runtime rules (C11, QNX)
- Wrap QNX-only code in `#if defined(__QNX__)`. The core must build on the Linux host without it.
- Server receive loop, in this order:
  1. Pulses: _PULSE_CODE_DISCONNECT, _PULSE_CODE_UNBLOCK, _PULSE_CODE_COIDDEATH, qmp pulses.
  2. _IO_CONNECT → reply EOK (VERIFY(qnx8): is it still sent on 8.0?).
  3. Other _IO_* messages → MsgError(ENOSYS).
  4. qmp messages → fully validate the header before touching the payload: type,
     length ≥ header, payload_len vs. received size, known service and resource, version,
     reserved bytes ignored.
  5. Anything else → MsgError(EINVAL).
- On _PULSE_CODE_DISCONNECT: ConnectDetach(scoid), then release all per-process state.
- Events: call MsgDeliverEvent only with client-registered handles. On ESRCH or EACCES, drop the
  subscription. Never hold a lock during delivery.
- Shared memory: always SHM_ANON + revocable + sealed + NOFD handles. Revoke on unsubscribe or
  disconnect.
- Use opaque structs with public sizeof constants for caller-provided storage
  (`QMP_CLIENT_STORAGE_SIZE`), with _Static_assert linking them to the internal layout.
- Use no global mutable state except the documented, init-time registries.
- Benchmarks set runmasks (ThreadCtl) and priorities (pthread_setschedparam) explicitly and
  record both in the CSV header.