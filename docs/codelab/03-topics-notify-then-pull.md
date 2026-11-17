# Codelab 03 — Topics: notify-then-pull with MsgDeliverEvent

**You will:** add a published topic to the joint service, understand why a QNX server must
**never** `MsgSend` to a client, subscribe from the client with a registered pulse event,
watch LATEST and QUEUE modes handle a slow subscriber (with honest loss accounting), and
survive `kill -9` of a subscriber and of the server.

**Time:** ~40 min. **Needs:** Codelab 02 (host build working; Pi 5 optional but strongly
recommended for the kill/restart scenarios).

---

## 1. The problem: events without server→client sends

Methods are easy on QNX: the client `MsgSend`s, the server inherits the client's priority,
replies, done. Events are the other direction — and the obvious design ("the server MsgSends
the update to each subscriber") is forbidden: a slow or dead subscriber would REPLY-block the
server, invert priorities, and a subscriber that simultaneously calls the server deadlocks.

The kernel's answer is `MsgDeliverEvent()`: the **client** describes how it wants to be
poked (a `struct sigevent`), registers it, and hands it to the server; the server fires it
without ever blocking. A pulse carries a code and a value — no payload. So qmp splits an
update into two cheap steps (ADR-003, *notify-then-pull*):

1. **notify** — the server copies the sample into preallocated topic storage and delivers a
   pulse to each subscriber: "topic X has news".
2. **pull** — the woken client fetches the sample with a normal, priority-inheriting
   `MsgSend` (`QMP_OP_PULL`).

The server never waits for a subscriber; a high-priority subscriber's pull is served first —
exactly the QNX properties we want to keep.

## 2. Declare the topics in the .proto

`idl/examples/demo/joint/v1/joint.proto` publishes the same sample under two topics with
different loss semantics:

```proto
option (uprotocol.publish_topic) = { id : 0x8001 name : "StateChanged" message : "JointState" };
option (qmp.topic_mode)         = { topic_id : 0x8001 mode : MODE_LATEST };

option (uprotocol.publish_topic) = { id : 0x8002 name : "StateLog" message : "JointState" };
option (qmp.topic_mode)         = { topic_id : 0x8002 mode : MODE_QUEUE depth : 8 };
```

Topic ids live in `0x8000–0xFFFF` (uProtocol resource ids for topics; methods stay below
`0x8000`). `qmp.topic_mode` picks the storage:

* **LATEST** — one slot; a new publish overwrites the old sample. A slow subscriber skips
  straight to the newest value and is told how many it missed. Right for control loops.
* **QUEUE{depth}** — a fixed ring; a slow subscriber replays the backlog in order until the
  ring overflows, and then the loss is counted. Right for logs and audit trails.

Loss is **always reported, never hidden**: every delivery carries a `dropped` count.

## 3. What the generator emits

Re-run codegen and look at `joint.qmp.h`:

```c
/* server side: typed publish — any thread, no heap, no blocking on subscribers */
int demo_joint_v1_Joint_publish_StateChanged(qmp_server_t *s, const demo_joint_v1_JointState *sample);

/* client side: typed subscription with caller-provided storage */
typedef void (*demo_joint_v1_Joint_StateChanged_cb)(void *ctx,
        const demo_joint_v1_JointState *sample, uint64_t seq, uint64_t dropped);
int demo_joint_v1_Joint_subscribe_StateChanged(qmp_client_t *c,
        demo_joint_v1_Joint_StateChanged_sub_t *sub,
        demo_joint_v1_Joint_StateChanged_cb cb, void *ctx);

/* sizing for the server's preallocated topic storage */
#define DEMO_JOINT_V1_JOINT_TOPIC_STORAGE_SIZE /* slots + rings + subscriber table */
```

The server opts in by pointing `cfg.topicbuf` at a static buffer of that size — no heap,
bounded by construction (`EAGAIN` when the subscriber table is full, never silent growth).
Leave `topicbuf` NULL and topics are simply off (`ENOTSUP`).

## 4. What happens underneath

```
client                                   kernel                server
ChannelCreate(_NTO_CHF_COID_DISCONNECT)          <- private notify channel
SIGEV_PULSE_INIT + SIGEV_MAKE_UPDATEABLE
MsgRegisterEvent(&ev, server_coid)               <- ev is REWRITTEN into a kernel handle
SUBSCRIBE {topic, ev blob}  ───────────────────► store (scoid, rcvid, ev); reply {sub_id, seq}
          ...                                    publish: copy sample, seq++,
                                                 MsgDeliverEvent(rcvid, &ev) per subscriber
◄~~ pulse {value = seq}  (advisory!)
qmp_client_poll():
  PULL {topic, after_seq} ─────────────────────► reply {seq, dropped, remaining} + sample
  cb(ctx, sample, seq, dropped); repeat while remaining > 0
```

Four kernel facts shape this (all tracked in `docs/VERIFY.md`):

* Registration is **mandatory** in 8.0: `MsgDeliverEvent` refuses an event the client didn't
  register against this server — a misbehaving server can't forge pulses (V-QNX-042).
* `MsgRegisterEvent` rewrites the sigevent **in place**; that's why SUBSCRIBE carries the
  whole 64-byte blob, not a handle number (V-QNX-042).
* Pulses can **coalesce or fail to queue**. The pulse value is only a hint: on wake-up the
  client drains every subscription using its `after_seq` cursor, which is the single source
  of truth (V-QNX-044). Delivery never blocks the publisher; a full queue just means the
  next poll catches up.
* Registrations die with the connection. After a server restart your sub ids and events are
  gone — `qmp_client_reconnect()` re-registers and re-subscribes for you, and the changed
  `server_generation` tells you it happened (V-QNX-046).

Also note what the publisher does *not* do: it never holds the topic lock across
`MsgDeliverEvent`. It copies the sample and snapshots the subscriber list under the lock,
then delivers lock-free; a dead subscriber (`ESRCH`) is dropped on the spot.

## 5. Try it on the host

The fake transport models the same semantics (registered events, pulse-queue overflow,
connection-death cleanup), so everything above runs in the host tests:

```sh
cmake --build --preset host-debug && ctest --preset host-debug -R topics
```

`runtime/tests/test_topics_fake.cpp` is the guided tour: typed delivery, LATEST overwrite
accounting, QUEUE ordering and overflow, a slow poller catching up after lost pulses
(`delivered + dropped == published` — try breaking it), unsubscribe racing a publisher, and
restart + resubscribe.

## 6. Try it on the target

Server with a 50 ms publisher, subscriber printing every update:

```sh
./qmp_joint_server --publish-ms 50 &
./qmp_joint_client subscribe state 20 30        # topic, updates wanted, time budget
# update: recv=123.456789 seq=7 dropped=0 state_seq=7 position=0.070000
# PASS subscribe
```

Now be slow on purpose — 5 ms publishing against a 300 ms-per-poll subscriber:

```sh
./qmp_joint_client subscribe state 10 30 300    # LATEST: skips ahead, dropped= counts it
./qmp_joint_client subscribe log   40 30 300    # QUEUE:  replays 8, then counts overflow
```

Then get violent: `kill -9` one of two subscribers (the server logs the disconnect and the
other keeps receiving), and `kill -9` the server mid-stream (the subscriber logs
`server gone, reconnecting`, picks up the new generation, and updates resume).
`scripts/target-smoke.sh` runs all of these as scenarios `joint 7`–`7e`.

## 7. Where to look when something is off

- `ENOTSUP` from subscribe/publish → the server has no `cfg.topicbuf`.
- `EAGAIN` from subscribe → subscriber table full (`QMP_SERVER_MAX_SUBS`); a bound, not a bug.
- `dropped=` keeps growing → you're polling slower than the publisher publishes. That's the
  design working: choose QUEUE with a bigger depth, or poll faster — but the loss was never
  silent.
- Updates stop after a server restart → check for `qmp_client_reconnect` in your poll loop's
  `ESRCH` path (see `examples/joint_demo/joint_client.c`, `mode_subscribe`).

**Next:** Codelab 04 — benchmarks: what notify-then-pull costs, measured (later milestone).
