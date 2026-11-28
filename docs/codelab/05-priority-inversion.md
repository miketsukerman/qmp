# Codelab 05 — Priority inversion: what native message passing buys you

**You will:** put a high-priority caller, a low-priority server and a medium-priority CPU
hog on one core, watch the caller get stuck behind the hog over a socket transport, watch
the same call go straight through over QNX message passing, and learn three limits of
priority inheritance that were measured on the board.

**Time:** ~25 min. **Needs:** Codelab 02 with a Raspberry Pi 5 (or another QNX 8.0 target)
reachable over SSH. This chapter has no host part: Linux sockets have no priority
inheritance, which is the point.

---

## 1. The problem

A control loop at priority 30 calls a service whose thread normally runs at priority 10.
A third thread at priority 15, unrelated to both, wants the CPU.

```
 priority
   30   control ──call──►(blocked, waiting for the reply)
   15   hog      ████████████████████████  runs: nothing above it is runnable
   10   service  (ready, holds the request, never gets the CPU)
```

The control loop is the most important thread in the system, yet it waits for a thread of
priority 15. That is priority inversion. It is unbounded: the wait lasts as long as the hog
wants to run.

With a socket the kernel cannot help. It sees a blocked reader and a runnable writer and
does not know that one is waiting for the other.

## 2. What QNX does instead

`MsgSend` tells the kernel exactly who waits for whom. When the service thread receives the
message, the kernel raises it to the sender's priority, and the service now outranks the
hog:

```
 priority
   30   control ──MsgSend──►  service handles the request at 30  ──MsgReply──► control
   15   hog      ██          (preempted)                              ████████████
   10   (the service thread's own priority, used again for its next low-priority caller)
```

qmp does nothing to make this happen. It only avoids getting in the way: a call is one
`MsgSendv` from the caller's own thread, and the handler runs on the thread that received
it. You can check that the handler really runs at the caller's priority:

```sh
export QMP_TARGET=qnxuser@<pi5-address>
scripts/target-smoke.sh 2>&1 | sed -n '/joint 5:/,/joint 6:/p'
```

Scenario `joint 5` starts the server with `on -p 10` and the client with `on -p 30`, and
captures `pidin` while a call is in flight. One server thread shows `30r RUNNING`. Scenario
`joint 5b` repeats it on a channel created with `_NTO_CHF_FIXED_PRIORITY`, where the thread
stays at `10r`.

## 3. Measure it

[`bench/prio_inversion.c`](../../bench/prio_inversion.c) builds the picture from section 1
out of three threads in one process:

| Thread | Priority | Does |
|--------|----------|------|
| server | 10 | one libqmp worker serving `Joint.SetTarget` |
| hog | 15 | spins for 20 ms, sleeps for 5 ms, repeats |
| client | 30 | calls `SetTarget` about every 7 ms and times each round trip |

The call period is jittered from a fixed seed, so the calls land on every phase of the
hog's cycle. All threads must share one CPU, otherwise the hog simply runs beside the
server; `bench/run.sh` starts every run with `on -C 3`.

```sh
bench/run.sh --calls 5000
bench/plot.py bench/results/<build id>
```

`run.sh` performs six runs and prints one `result:` line for each:

| Label | Transport | Hog | What it shows |
|-------|-----------|-----|---------------|
| `qnx-quiet` | native | no | the plain cost of a call |
| `qnx-fixed-quiet` | native, `_NTO_CHF_FIXED_PRIORITY` | no | the same without inheritance |
| `uds-quiet` | `AF_UNIX` sockets | no | the cost of the socket path |
| `qnx-hog` | native | yes | inheritance at work |
| `qnx-fixed-hog` | native, `_NTO_CHF_FIXED_PRIORITY` | yes | the negative control |
| `uds-hog` | `AF_UNIX` sockets | yes | the inversion |

## 4. Read the result

Look at two fields first: `handler_prio_min` and `handler_prio_max`. They are the lowest
and highest priority the handler observed itself running at.

* `qnx-hog` reports 30 for both. Its latency distribution is the same as `qnx-quiet`: the
  hog is invisible to the caller.
* `qnx-fixed-hog` and `uds-hog` report 10 for both. Most of their calls take milliseconds
  instead of microseconds, and the slowest takes one full hog burst. The `over_1ms` field
  counts those calls. Make the burst longer with `--hog-burst-ms` and the worst case grows
  with it; with a hog that never sleeps the call would never return.

`qnx-fixed-hog` is the important control. It uses the same kernel path, the same code and
the same message sizes as `qnx-hog`. The only difference is one channel flag, so the
difference in the result is inheritance and nothing else.

`plot.py` draws every run on one logarithmic axis, which is the picture to show someone
who asks why you would not just use sockets.

## 5. Three limits, measured on the board

Inheritance is not magic. The first two behaviours below were measured on the Pi 5 with
[`tools/probe/qnx_probe.c`](../../tools/probe/qnx_probe.c) and the smoke scenarios, and both
contradicted what the design first assumed. The third follows from how a deferred reply
works.

**A busy worker is not raised by callers that queue behind it.** The kernel raises a
thread when it *receives* a message. A worker that is in the middle of a priority-8
request stays at 8 while callers at 20 and 30 wait for it (V-QNX-010). They are served
highest first afterwards, but they wait for a handler running at someone else's priority.
So give a service enough workers that its most important callers always find one waiting
in `MsgReceive`, and keep handlers short.

**A worker keeps its caller's priority after the reply.** It is not put back to its own
priority until its next receive, and it follows callers downwards as well as upwards
(V-QNX-011). For an ordinary call this is harmless, because the worker goes straight back
to receiving. For a one-way call it matters: the handler runs after the caller has been
released. libqmp therefore records each worker's priority at start and restores it before
a one-way handler runs. Scenario `joint 6b` shows a priority-30 caller's `Stop` handled at
10.

**A deferred reply inherits nothing.** When a handler returns `QMP_DEFERRED`, the worker
goes back to receiving and the caller's priority is lent to nobody. Whatever thread
completes the request later must itself run at a suitable priority (DESIGN §5.2).

A fourth point is about topics. Notification pulses registered with
`SIGEV_PULSE_PRIO_INHERIT` leave the subscriber's thread at its own priority; the
publisher's priority does not travel with them (V-QNX-009). A subscriber that needs prompt
updates calls `qmp_client_poll()` from a thread of the priority it wants.

## 6. Try it yourself

1. Run the benchmark with `--prio-hog 35`. The hog now outranks the client itself, and no
   transport can help. What do `qnx-hog` and `uds-hog` report?
2. Run `scripts/target-run.sh tools/probe/qmp_qnx_probe` and find the line for V-QNX-010.
   Which priority did the busy worker have while the higher-priority senders were waiting?
3. On the board, start `on -p 10 qmp_joint_server --threads 1 --verbose`, make one client
   burn for five seconds with `on -p 8 qmp_joint_client --timeout 0 slow 5000 spin`, and
   during that time run `on -p 30 qmp_joint_client --timeout 0 once`. Compare the
   timestamps of `recv Burn` and `recv SetTarget` in the server log: the priority-30 client
   waited for the priority-8 handler. Repeat with `--threads 2` and it is served at once.

---

Back to [Codelab 04](04-inspect-with-qmpctl.md) · [README](../../README.md)
