# qmp design

Service-oriented communication over QNX native message passing, with uProtocol-compatible
service definitions. Status: **draft v0.1 (2026-11-09), pre-implementation**.

Decisions referenced as ADR-nnn live in [DECISIONS.md](DECISIONS.md). Unverified platform
assumptions are tagged `VERIFY(qnx8)` / `VERIFY(up)` and tracked in [VERIFY.md](VERIFY.md).

Design answers fixed on 2026-11-09 (Q1–Q6):

| # | Question | Decision |
|---|----------|----------|
| Q1 | Wire framing | Two-IOV send (header IOV + user POD struct IOV); server receives into one preallocated buffer |
| Q2 | Call timeouts | Hard per-call timeout via `TimerTimeout(_NTO_TIMEOUT_SEND\|_NTO_TIMEOUT_REPLY)`; server sees `_PULSE_CODE_UNBLOCK` |
| Q3 | Client callback thread | No hidden threads. `qmp_client_poll()`/`qmp_client_run()` on the application's thread |
| Q4 | HELLO | Implicit inside `qmp_client_connect()`; `server_generation` cached in the client |
| Q5 | up-spec pin | Latest **stable** tag `v1.5.8` (commit `cab8338`); see ADR-005 |
| Q6 | Server threading | N identical workers (default 4) RECEIVE-blocked on one channel; deferred requests in a fixed caller-provided pool |

---

## 1. Components

```
 HOST (Ubuntu)                                   TARGET (QNX 8.0, RPi5 primary / RPi4 secondary / x86_64)
 ─────────────────────────────                    ──────────────────────────────────────────────────────────
 joint.proto ─┐                                   ┌─ Process A: joint_service (C) ───────────────────────┐
 uoptions.proto├─► protoc ─► protoc-gen-qmp ──┐   │  user code: handlers, publish loop                   │
 qmp/options.proto┘  (python, Jinja2)         │   │  ───────────────────────────────────────────────────  │
                                              │   │  joint.qmp.h / joint.qmp.c  (generated C11 bindings)  │
          generated artefacts:                │   │  ───────────────────────────────────────────────────  │
          joint.qmp.h  joint.qmp.c            ├──►│  libqmp core (C11): hdr codec, dispatch, subs, deferred│
          joint.qmp.hpp (optional)            │   │  libqmp transport/qnx: ChannelCreate, MsgReceivev,     │
          joint.reflect.json                  │   │     MsgReplyv, MsgError, MsgDeliverEvent, shm handles  │
          joint.secpol.txt (optional)         │   └──────────────────────┬────────────────────────────────┘
                                              │                          │ MsgReceive / MsgReply / MsgDeliverEvent
                                              │   ┌──────────────────────▼────────────────────────────────┐
                                              │   │  QNX Neutrino microkernel  (procnto)                   │
                                              │   │  channels · connections · pulses · priority inheritance│
                                              │   │  registered events · shm handles · secpol              │
                                              │   └───▲──────────────▲────────────────▲───────────────────┘
                                              │       │ MsgSend      │ MsgSend        │ MsgSend
                                              │   ┌───┴───────────┐ ┌┴──────────────┐ ┌┴─────────────────────┐
                                              └──►│ Process B:    │ │ Process C:    │ │ Process D:           │
                                                  │ control (C++) │ │ qmpctl (C++)  │ │ uP app via adapter   │
                                                  │ joint.qmp.hpp │ │ reflect.json  │ │ up-cpp UTransport    │
                                                  │ joint.qmp.h   │ │ qmp C ABI     │ │ qmp uP adapter (C++) │
                                                  │ libqmp        │ │ libqmp        │ │ libqmp C ABI         │
                                                  └───────────────┘ └───────────────┘ └──────────────────────┘

 HOST-ONLY test/bench path (no kernel message passing):
   libqmp core + transport/uds (AF_UNIX SOCK_SEQPACKET) — same C API, benchmarks and CI only (ADR-004)
   libqmp core + transport/fake (in-process queues) — unit tests
```

Layering inside one process, top to bottom. Each layer only calls the one below it.

| Layer | Language | Depends on | Notes |
|-------|----------|-----------|-------|
| user code | C or C++ | generated bindings | handlers run on libqmp worker threads at inherited priority |
| `joint.qmp.hpp` | C++17 header-only | `joint.qmp.h` | wraps the **same** structs; no copies |
| `joint.qmp.h/.c` | C11 | `include/qmp/*.h` | ids, POD structs, stubs, dispatch table, reflection blob |
| libqmp core | C11 | libc only | header codec, dispatch, subscription registry, deferred pool, versioning, errors |
| libqmp transport | C11 | QNX libc (`<sys/neutrino.h>`, `<sys/dispatch.h>`) | one of: qnx, uds (host), fake (tests) |

---

## 2. Wire protocol v1

Native byte order, 8-byte alignment, single node (no Qnet in 8.0). Header first, then payload.

```c
#define QMP_MSG_TYPE   ((uint16_t)(_IO_MAX + 0x100))   /* 0x2FF; > _IO_MAX as required */

enum qmp_flags {
    QMP_F_ONEWAY       = 1u << 0,  /* server replies before running the handler        */
    QMP_F_IS_REPLY     = 1u << 1,  /* set by server in every MsgReply header            */
    QMP_F_SHM_ATTACHED = 1u << 2,  /* payload is followed by qmp_shm_desc_t (stretch)   */
    QMP_F_APP_ERROR    = 1u << 3,  /* status != OK; payload may be absent               */
    QMP_F_CONTROL      = 1u << 4,  /* resource_id is a qmp control opcode, not a method */
};

typedef struct qmp_hdr {            /* 24 bytes */
    uint16_t type;                  /* 0  QMP_MSG_TYPE                                  */
    uint16_t flags;                 /* 2  qmp_flags                                     */
    uint16_t service_id;            /* 4  uProtocol service id                          */
    uint16_t instance_id;           /* 6  0x0000 = default instance (uProtocol)         */
    uint16_t resource_id;           /* 8  method <0x8000 | topic >=0x8000 | control op  */
    uint8_t  ver_major;             /* 10                                               */
    uint8_t  ver_minor;             /* 11                                               */
    uint32_t payload_len;           /* 12 bytes following the header                    */
    int32_t  status;                /* 16 replies: UCode, 0 = OK                        */
    uint8_t  up_priority;           /* 20 UPriority 1..7 from adapter; 0 = native call  */
    uint8_t  reserved[3];           /* 21 sender zeroes, receiver ignores               */
} qmp_hdr_t;
_Static_assert(sizeof(qmp_hdr_t) == 24, "");
_Static_assert(offsetof(qmp_hdr_t, payload_len) == 12, "");
_Static_assert(offsetof(qmp_hdr_t, status) == 16, "");
_Static_assert(offsetof(qmp_hdr_t, up_priority) == 20, "");
```

Control opcodes (`QMP_F_CONTROL` set; `resource_id` field):

| op | value | request payload | reply payload |
|----|-------|-----------------|---------------|
| `QMP_OP_HELLO` | 1 | `qmp_hello_t {u16 service_id; u16 instance_id; u8 ver_major; u8 ver_minor; u8 pad[2];}` | `qmp_hello_reply_t {u8 ver_major; u8 ver_minor; u8 pad[2]; u32 server_generation; u32 max_msg_size; u32 pad2;}` — or `MsgError(ENOTSUP)` on major mismatch |
| `QMP_OP_SUBSCRIBE` | 2 | `qmp_subscribe_t {u16 topic_id; u16 pad; int32 event_handle; u64 client_cookie;}` | `qmp_subscribe_reply_t {u32 sub_id; u32 mode; u64 current_seq;}` |
| `QMP_OP_UNSUBSCRIBE` | 3 | `{u32 sub_id; u32 pad;}` | empty |
| `QMP_OP_PULL` | 4 | `qmp_pull_t {u32 sub_id; u32 max_samples; u64 after_seq;}` | `qmp_pull_reply_t {u64 seq; u32 remaining; u32 count;}` + `count` sample structs |

Payload rules (generator-enforced):

* Payload structs are POD, field-number order, explicit `_padN` members, 8-byte total size
  multiple, `_Static_assert` on `sizeof` and every `offsetof`.
* `string`/`bytes` → `struct { uint16_t len; char data[N]; }` (N = `qmp.max_len`; no NUL
  requirement; receiver validates `len <= N`).
* `repeated T` → `struct { uint32_t count; uint32_t _pad; T items[N]; }` (N = `qmp.max_count`;
  receiver validates `count <= N`).
* Zero-field messages (`Empty`) are special: `payload_len = 0`, the generated functions omit the
  parameter. No zero-sized C struct is ever emitted.
* Sender zeroes padding; receiver ignores it. All received data is untrusted and bounds-checked
  before the handler sees it.

Validation order on the server, per `runtime-c.instructions.md`: pulses → `_IO_CONNECT` →
other `_IO_*` → qmp header (type, size ≥ header, `payload_len` == received − 24, known
service/instance/resource, major version, flags) → payload bounds → dispatch.

---

## 3. Sequence diagrams

Notation: `──►` MsgSend/MsgSendv (blocking), `◄──` MsgReply/MsgError, `~~►` pulse/event.
`K` is the kernel.

### 3.1 Connect + HELLO (implicit, Q4)

```
 client (prio 20)                    K                          server workers (base prio 10)
 ───────────────                    ───                         ──────────────────────────────
 name_open("qmp/0101-0000/v1") ────►│                            name_attach done at init
                                    │  (procnto name resolution; _IO_CONNECT handling: VERIFY V-QNX-003)
 ◄────────────── coid               │
 MsgSendv(coid, [hdr CONTROL/HELLO, qmp_hello_t]) ──►  worker T1 unblocks @ prio 20 (inherited)
                                    │                            validate hdr, major == 1 ? ver_minor stored
 ◄──── [hdr IS_REPLY, qmp_hello_reply_t{1,0,gen=7,max=4096}] ── MsgReplyv
 cache: server_generation=7, server_minor=0                     T1 back to prio 10, MsgReceivev
 return EOK from qmp_client_connect()
```

Major mismatch: server `MsgError(rcvid, ENOTSUP)`; client returns `ENOTSUP`, no retry.
The client's private notification channel (`_NTO_CHF_COID_DISCONNECT`) is created in
`qmp_client_init()`, before connect, so server-death pulses are never missed.

### 3.2 Sync method call

```
 client (prio 20)                           K             worker T2 (base 10)
 Joint_SetTarget(c,&req,&rep,&status)
   TimerTimeout(MONO, SEND|REPLY, ns=50ms)
   MsgSendv(coid, iov[hdr][req]=2, iov[hdr][rep]=2) ──►  T2 runs @20 ; MsgReceivev into rxbuf[max_msg]
                                                          validate hdr+payload
                                                          ops->SetTarget(ctx, req, in, out, &st) → QMP_HANDLED
 ◄──── MsgReplyv(rcvid, 0, [hdr IS_REPLY status=st][out]) ─
 returns EOK, *status = st                                T2 back to 10, MsgReceivev
```

Only two kernel calls per round trip on the hot path (MsgSendv / MsgReplyv) plus `TimerTimeout`
when a timeout is armed. No heap, no copies beyond the kernel's.

### 3.3 Deferred reply with client timeout (Q2, Q6)

```
 client (prio 20)                     K                 worker T1          worker T3           user thread
 MsgSendv(GetState) ────────────────►│ T1 @20: handler returns QMP_DEFERRED
                                     │        req slot taken from deferred pool, rcvid kept
                                     │ T1 → MsgReceivev (prio back to 10)                    ... slow work ...
 [50 ms elapse]                      │
 kernel: client timeout, but channel has _NTO_CHF_UNBLOCK
 ◄── client stays REPLY-blocked ──── │ ~~► _PULSE_CODE_UNBLOCK{rcvid} to T3
                                     │ T3: find deferred req by rcvid
                                     │     ops->on_cancel(ctx, req)     (user drops its pointer)
                                     │     MsgError(rcvid, ETIMEDOUT)   (releases client)
 MsgSendv returns -1, errno ETIMEDOUT│ VERIFY V-QNX-006: errno seen by client
 Joint_GetState returns ETIMEDOUT    │
                                     │                                   Joint_GetState_reply(req,...)
                                     │                                   → EINVAL (slot already released);
                                     │                                     tolerated, never asserts
```

Rules: the `qmp_req_t*` stays valid until replied, cancelled, or client disconnect. The deferred
pool is caller-provided (`qmp_req_t slots[8]`); when full the server replies `MsgError(EAGAIN)`
(UCode `RESOURCE_EXHAUSTED` reported by client). Slot reuse is guarded by a generation counter in
`qmp_req_t` so a stale pointer from a cancelled request cannot reply to a newer request.

### 3.4 One-way call

```
 client (prio 30)                           worker T1
 MsgSendv([hdr ONEWAY, Stop payload_len=0]) ──►  T1 @30: validate, MsgReplyv(rcvid, 0, [hdr IS_REPLY]) immediately
 ◄──── returns EOK (handler has NOT run yet)      reply drops inheritance → T1 back to base 10
                                                  ops->Stop(ctx, &info)  runs at T1's base priority
```

Reply-before-work keeps the caller unblocked for the minimum time; the trade-off (handler runs at
server base priority, not caller priority) is documented in the handler callback docs.
`MsgSendPulse` is used only for payload-free internal signals, never for user methods.

### 3.5 Subscribe + MsgDeliverEvent + pull (notify-then-pull, ADR-003)

```
 client (prio 20)                                      K              server
 init: chid_c = ChannelCreate(_NTO_CHF_COID_DISCONNECT|_NTO_CHF_UNBLOCK)
       self_coid = ConnectAttach(0,0,chid_c,...)
 subscribe_StateChanged(c,&sub,cb,ctx):
   SIGEV_PULSE_INIT(&ev, self_coid, SIGEV_PULSE_PRIO_INHERIT, QMP_PULSE_TOPIC, sub_idx)
   SIGEV_MAKE_UPDATEABLE(&ev)                                       VERIFY V-QNX-008
   h = MsgRegisterEvent(&ev, coid_server)                           VERIFY V-QNX-007
   MsgSendv([hdr CONTROL/SUBSCRIBE, {topic 0x8001, event_handle h}]) ──► register (scoid,pid,topic,h)
 ◄──────────────────────────────── [{sub_id=3, mode=LATEST, current_seq=41}]

 ... later, server publish_StateChanged(&sample):
                                                                     copy sample into topic slot (LATEST) seq=42
                                                                     for each subscriber: MsgDeliverEvent(rcvid, &ev)
                                                                       with pulse value = (uint32)seq   (no lock held)
 ◄~~~ pulse on chid_c  {code QMP_PULSE_TOPIC, value seq=42} ────────── (ESRCH/EACCES → drop subscriber)
 qmp_client_poll(c, timeout):
   MsgReceivePulse(chid_c) → code/value
   if value > sub.last_seq:
     MsgSendv([hdr CONTROL/PULL,{sub_id 3, max 1, after 41}]) ──────► reply [{seq 42, remaining 0, count 1}][JointState]
     cb(ctx, &sample, 42) on the polling thread
```

Pulse coalescing is a feature: in LATEST mode the client only ever pulls the newest sample.
In QUEUE{depth} mode the server keeps a ring per topic; `remaining` tells the client to pull
again. The server **never** MsgSends to a client (deadlock-free by construction).

### 3.6 Client crash

```
 client B (crashes)             K                               server worker
   process exits               │ all coids of B closed
                               │ ~~► _PULSE_CODE_DISCONNECT{scoid} once per client process
                               │                                 ConnectDetach(scoid)
                               │                                 for subs where sub.scoid==scoid: remove
                               │                                 for deferred req where req.scoid==scoid:
                               │                                     ops->on_cancel(ctx, req); release slot
                               │                                 shm handles of pid: shm_revoke (stretch)
```

Deferred `MsgReply` to a dead client fails with `ESRCH`/`EBADF`; the runtime treats this as success.

### 3.7 Server crash + client reconnect

```
 client (poll loop)                            K                       server (restarts, generation 8)
                                               │ server dies → all its channels destroyed
 ◄~~~ _PULSE_CODE_COIDDEATH{coid} on chid_c ─── │ (channel created with _NTO_CHF_COID_DISCONNECT)
 qmp_client_poll: mark DISCONNECTED, on_state(ctx, QMP_DISCONNECTED)
 any in-flight MsgSendv returns ESRCH/EBADF → generated call returns ESRCH (UCode UNAVAILABLE)
 backoff 10ms,20ms,...,max 1s (configurable):
   name_open fails ENOENT → retry                                      name_attach("qmp/0101-0000/v1")
   name_open ok → HELLO → reply {gen=8}  ──────────────────────────────►
   gen changed (7→8): on_state(ctx, QMP_RECONNECTED_NEW_GENERATION)
   for each sub: MsgRegisterEvent again (new coid), SUBSCRIBE again
   client pulls once to refresh LATEST state (seq resets; cb called with current sample)
```

Reconnect happens inside `qmp_client_poll()` (or `qmp_client_reconnect()` explicitly for
poll-less sync-only clients) — never on a hidden thread (Q3).

### 3.8 uProtocol REQUEST/RESPONSE through the adapter (stretch, ADR-005)

```
 uP client app (up-cpp)        qmp uTransport adapter (proc D)            K      joint_service (C)
 UMessage{REQUEST, src=//D/…, sink=/0101/1/1, prio=CS4, ttl=50ms, payload=SetTargetRequest(protobuf)}
   transport.send(msg) ──────►  decode payload with protobuf → demo_joint_v1_SetTargetRequest (POD)
                                map sink UUri: ue_id 0x00000101 → svc 0x0101 inst 0; res 1 → method 1
                                map CS4 → QNX prio 16 (table §5.4): pthread_setschedprio(self,16)
                                hdr.up_priority = 5 (UPRIORITY_CS4), timeout = ttl
                                qmp_call(c, 1, &req, &rep, ttl) ──────────►  worker @16, handler, reply
                                ◄───────────────────────────────────────────
                                build UMessage{RESPONSE, reqid=msg.id, commstatus=UCode(status)}
                                encode rep → protobuf bytes
   listener.onReceive(resp) ◄───
```

The adapter is the only place protobuf *encoding* exists; the native path never serialises.
PUBLISH maps to a qmp subscription driven by the adapter's own `qmp_client_poll()` thread.

---

## 4. Generated API sketches — `demo.joint.v1.Joint`

### 4.1 C header `joint.qmp.h` (excerpt)

```c
/* SPDX-License-Identifier: Apache-2.0 */
/* Generated by protoc-gen-qmp 0.1 from joint.proto — DO NOT EDIT */
#ifndef DEMO_JOINT_V1_JOINT_QMP_H
#define DEMO_JOINT_V1_JOINT_QMP_H
#include <qmp/qmp.h>
#include <qmp/qmp_client.h>
#include <qmp/qmp_server.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ---- ids (from uprotocol.* options) ------------------------------------- */
#define DEMO_JOINT_V1_JOINT_SERVICE_ID      0x0101u
#define DEMO_JOINT_V1_JOINT_SERVICE_NAME    "demo.joint.Joint"
#define DEMO_JOINT_V1_JOINT_VER_MAJOR       1u
#define DEMO_JOINT_V1_JOINT_VER_MINOR       0u
#define DEMO_JOINT_V1_JOINT_M_SETTARGET     0x0001u
#define DEMO_JOINT_V1_JOINT_M_STOP          0x0002u
#define DEMO_JOINT_V1_JOINT_M_GETSTATE      0x0003u
#define DEMO_JOINT_V1_JOINT_T_STATECHANGED  0x8001u
#define DEMO_JOINT_V1_JOINT_UURI_SETTARGET  "/101/1/1"      /* uProtocol URI path, local authority */
#define DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE    (24u + 48u)   /* header + largest payload */

/* ---- payload structs (field-number order, explicit padding) ------------ */
typedef struct demo_joint_v1_SetTargetRequest {
    float position_rad;             /* 0 */
    float max_velocity;             /* 4 */
} demo_joint_v1_SetTargetRequest;   /* 8 */
_Static_assert(sizeof(demo_joint_v1_SetTargetRequest) == 8, "layout");
_Static_assert(offsetof(demo_joint_v1_SetTargetRequest, max_velocity) == 4, "layout");

typedef struct demo_joint_v1_SetTargetReply {
    int32_t result;                 /* 0 */
    uint8_t _pad0[4];               /* 4 */
} demo_joint_v1_SetTargetReply;     /* 8 */

typedef struct demo_joint_v1_JointState {
    uint64_t seq;                   /* 0  */
    uint64_t timestamp_ns;          /* 8  */
    float    position_rad;          /* 16 */
    float    velocity;              /* 20 */
    struct { uint16_t len; char data[16]; } mode;   /* 24, qmp.max_len = 16 */
    uint8_t  _pad0[6];              /* 42 */
} demo_joint_v1_JointState;         /* 48 */
_Static_assert(sizeof(demo_joint_v1_JointState) == 48, "layout");
_Static_assert(offsetof(demo_joint_v1_JointState, mode) == 24, "layout");

/* Empty messages generate no struct; functions omit the parameter. */

/* ---- client ------------------------------------------------------------ */
/* Return: EOK or errno. *status receives the UCode set by the server (0 = OK).
 * Blocking: yes (MsgSendv). Thread: caller's. Priority: caller's, inherited by server. */
int demo_joint_v1_Joint_connect(qmp_client_t *c, uint16_t instance_id, const qmp_client_cfg_t *cfg);

int demo_joint_v1_Joint_SetTarget(qmp_client_t *c,
                                  const demo_joint_v1_SetTargetRequest *req,
                                  demo_joint_v1_SetTargetReply *rep, int32_t *status);
int demo_joint_v1_Joint_SetTarget_ex(qmp_client_t *c,
                                     const demo_joint_v1_SetTargetRequest *req,
                                     demo_joint_v1_SetTargetReply *rep, int32_t *status,
                                     const qmp_call_opts_t *opts);  /* timeout_ns, up_priority */
int demo_joint_v1_Joint_Stop(qmp_client_t *c);                       /* oneway: returns when server has accepted */
int demo_joint_v1_Joint_GetState(qmp_client_t *c, demo_joint_v1_JointState *rep, int32_t *status);

/* topic StateChanged: callback runs on the thread calling qmp_client_poll()/qmp_client_run(). */
typedef void (*demo_joint_v1_Joint_StateChanged_cb)(void *ctx,
                                                    const demo_joint_v1_JointState *sample,
                                                    uint64_t seq);
int demo_joint_v1_Joint_subscribe_StateChanged(qmp_client_t *c, qmp_sub_t *sub,
                                               demo_joint_v1_Joint_StateChanged_cb cb, void *ctx);
/* qmp_sub_unsubscribe(sub), qmp_client_poll(c, timeout_ns), qmp_client_run(c) are in qmp_client.h */

/* ---- server ------------------------------------------------------------ */
/* Handlers run on a libqmp worker thread at the CALLER's priority (inherited).
 * Return QMP_HANDLED (reply with *out/*status), QMP_DEFERRED (keep req, reply later),
 * or a positive errno (runtime sends MsgError). One-way handlers run after the reply at the
 * worker's base priority and have no out/status. */
typedef struct demo_joint_v1_Joint_ops {
    int  (*SetTarget)(void *ctx, qmp_req_t *req, const demo_joint_v1_SetTargetRequest *in,
                      demo_joint_v1_SetTargetReply *out, int32_t *status);
    int  (*Stop)(void *ctx, const qmp_req_info_t *info);
    int  (*GetState)(void *ctx, qmp_req_t *req, demo_joint_v1_JointState *out, int32_t *status);
    void (*on_cancel)(void *ctx, qmp_req_t *req);           /* deferred req unblocked/disconnected */
    void (*on_client_gone)(void *ctx, pid_t pid);           /* _PULSE_CODE_DISCONNECT, optional   */
} demo_joint_v1_Joint_ops;

int demo_joint_v1_Joint_register(qmp_server_t *s, const demo_joint_v1_Joint_ops *ops, void *ctx);

/* deferred completion — any thread; tolerates the request having been cancelled (returns EINVAL) */
int demo_joint_v1_Joint_SetTarget_reply(qmp_req_t *req, const demo_joint_v1_SetTargetReply *out, int32_t status);
int demo_joint_v1_Joint_GetState_reply (qmp_req_t *req, const demo_joint_v1_JointState *out, int32_t status);

/* publish — any thread; copies *sample into the topic slot and delivers events without holding a lock */
int demo_joint_v1_Joint_publish_StateChanged(qmp_server_t *s, const demo_joint_v1_JointState *sample);

/* reflection blob (also written to joint.reflect.json) */
extern const unsigned char demo_joint_v1_Joint_reflect_json[];
extern const size_t        demo_joint_v1_Joint_reflect_json_len;

#ifdef __cplusplus
}
#endif
#endif
```

Core types from `include/qmp/`: `qmp_client_t`, `qmp_server_t`, `qmp_sub_t`, `qmp_req_t` are
opaque fixed-size storage (`QMP_CLIENT_STORAGE_SIZE` etc. with `_Static_assert` against the
private layout). `qmp_server_cfg_t { int nthreads /*default 4*/; int base_prio /*0 = inherit
caller*/; qmp_req_t *deferred_pool; size_t deferred_count; void *rxbuf; size_t rxbuf_size; }`.

### 4.2 C++ wrapper `joint.qmp.hpp` (excerpt)

```cpp
// SPDX-License-Identifier: Apache-2.0  — header-only, C++17, no exceptions, no RTTI, no heap
#include <qmp/cpp/qmp.hpp>      // qmp::Result<T>, qmp::Status, qmp::Client, qmp::Server, qmp::Subscription
#include "joint.qmp.h"

namespace demo::joint::v1 {

using SetTargetRequest = ::demo_joint_v1_SetTargetRequest;   // SAME structs, no conversion
using SetTargetReply   = ::demo_joint_v1_SetTargetReply;
using JointState       = ::demo_joint_v1_JointState;

class JointProxy {
public:
    static qmp::Result<JointProxy> connect(uint16_t instance = 0, const qmp::ClientConfig& = {});
    JointProxy(JointProxy&&) noexcept; JointProxy& operator=(JointProxy&&) noexcept;   // RAII: disconnect
    ~JointProxy();

    qmp::Result<SetTargetReply> SetTarget(const SetTargetRequest&, qmp::CallOptions = {}) noexcept;
    qmp::Status                 Stop() noexcept;
    qmp::Result<JointState>     GetState(qmp::CallOptions = {}) noexcept;

    // callback invoked from poll(); F must be trivially relocatable and fit qmp::Callback's 32-byte SBO
    template <class F>
    qmp::Result<qmp::Subscription> subscribeStateChanged(F&& on_sample) noexcept;

    qmp::Status poll(std::chrono::nanoseconds timeout) noexcept;   // drives callbacks + reconnect
    qmp::ConnectionState state() const noexcept;
private:
    qmp::Client client_;   // wraps qmp_client_t storage
};

class JointSkeleton {
public:
    explicit JointSkeleton(qmp::Server& s);          // registers ops table bound to *this
    virtual ~JointSkeleton();
    // Return qmp::Status::ok(), qmp::Status::app(UCode), qmp::Status::deferred(), or qmp::Status::errno_(E)
    virtual qmp::Status SetTarget(qmp::Request& req, const SetTargetRequest& in, SetTargetReply& out) = 0;
    virtual void        Stop(const qmp::RequestInfo&) = 0;
    virtual qmp::Status GetState(qmp::Request& req, JointState& out) = 0;
    virtual void        onCancel(qmp::Request&) noexcept {}
    virtual void        onClientGone(pid_t) noexcept {}

    qmp::Status publishStateChanged(const JointState&) noexcept;
    static qmp::Status replySetTarget(qmp::Request&, const SetTargetReply&, int32_t ucode = 0) noexcept;
    static qmp::Status replyGetState (qmp::Request&, const JointState&,     int32_t ucode = 0) noexcept;
};

}  // namespace demo::joint::v1
```

`qmp::Result<T>`: `{ int err; int32_t ucode; T value; }` with `ok()`, `operator bool`,
`value()`, `error()`; `T` is the C POD so `Result` is itself trivially copyable.
`qmp::Status` is `{int err; int32_t ucode;}` plus a `deferred` tag. `qmp::Subscription` is a
move-only RAII owner of a `qmp_sub_t` that unsubscribes in its destructor.

---

## 5. Threading and priority model

### 5.1 Server

* `qmp_server_run(s)` spawns `N-1` worker threads and runs the N-th on the caller; all are
  `MsgReceivev`-blocked on the single channel (channel flags: `_NTO_CHF_DISCONNECT |
  _NTO_CHF_UNBLOCK | _NTO_CHF_COID_DISCONNECT`; never `_NTO_CHF_FIXED_PRIORITY`).
* Choosing N: `N = 1 + max concurrent deferred handlers that block + 1 spare for pulses`,
  capped at the number of CPUs ×2. Default 4. Rationale: a worker blocked inside a handler
  cannot receive; pulses (`UNBLOCK`, `DISCONNECT`) must always find a free receiver.
* Base priority: all workers start at `cfg.base_prio` (default: priority of the caller of
  `qmp_server_run`). While a worker holds a received message its priority is the client's.
* Never hold a mutex across `MsgReplyv`, `MsgDeliverEvent`, `MsgSendv`. The subscription
  registry uses a short critical section to snapshot `(rcvid, event)` pairs, then delivers.

### 5.2 Worked example — three clients

Server `joint_service` base priority 10, N = 4. Clients: `logger` (prio 8), `hmi` (prio 20),
`control` (prio 30). Single core to make the scheduling visible.

```
 t0  logger  MsgSend GetState   → T1 inherits 8,  handler runs  @8
 t1  control MsgSend SetTarget  → T2 inherits 30 and PREEMPTS T1 (T1 is @8)      ← no inversion
 t2  T2 replies (MsgReplyv), drops to 10, blocks in MsgReceivev
 t3  hmi     MsgSend GetState   → T3 inherits 20, preempts T1 (@8)
 t4  T3 replies; T1 resumes @8 and finishes logger's request
```

Now a *deferred* case: `control` calls `SetTarget` and the handler defers waiting on a motor
ISR pulse delivered to the service's own worker pool:

```
 t0  control MsgSend SetTarget → T2 @30 returns QMP_DEFERRED, goes back to MsgReceivev @10
     control is REPLY-blocked; its priority is "lent" to nobody — the user's motor thread
     that will complete the request should run ≥ 30. qmp cannot do this for you; DESIGN RULE:
     a deferred handler must hand off to a thread whose priority ≥ the highest caller it serves,
     or use qmp_req_get_priority(req) to boost explicitly (pthread_setschedprio) and restore after reply.
 t1  motor pulse → T4 @ pulse priority; user completes with Joint_SetTarget_reply(req,…)
```

This rule is the one thing priority inheritance does *not* cover; the benchmark in W4 measures
it (deferred vs sync under a CPU-hog at prio 15).

### 5.3 Client

* Sync calls run on the caller's thread at the caller's priority; the server inherits it.
* Notification pulses carry the priority given in the sigevent; qmp uses
  `SIGEV_PULSE_PRIO_INHERIT` by default so a high-priority publisher wakes the subscriber
  promptly, and the subscriber's pull runs at the poller's own priority.
* No hidden threads (Q3): `qmp_client_poll()` is the only place callbacks and reconnects happen.

### 5.4 Adapter preserving UPriority

| UPriority | value in `hdr.up_priority` | default QNX priority |
|-----------|----------------------------|----------------------|
| CS0 | 1 | 8 |
| CS1 | 2 | 10 |
| CS2 | 3 | 12 |
| CS3 | 4 | 14 |
| CS4 (default for REQUEST) | 5 | 16 |
| CS5 | 6 | 18 |
| CS6 | 7 | 20 |

The adapter sets the sending thread's priority with `pthread_setschedprio` before the call
(so inheritance carries the UPriority into the server) and stamps `hdr.up_priority`. A qmp
server exposing a uP endpoint reads `hdr.up_priority` back when building the RESPONSE. Table is
a `qmp_up_priority_map_t` the integrator can override. Native calls leave `up_priority = 0`.

---

## 6. Error model

Two independent channels, never mixed:

* **Transport/OS errors** → C function return value (`errno`), produced by the kernel
  (`MsgSendv` failures), by `MsgError()` from the runtime, or by argument validation.
* **Application status** → `hdr.status` as a uProtocol `UCode` (verified at tag v1.5.8), returned
  through the `int32_t *status` out-parameter. `status != 0` sets `QMP_F_APP_ERROR` and the reply
  payload may be omitted (`payload_len = 0`).

Mapping used by (a) the client when reporting an errno to a uP adapter, (b) the adapter when
translating a `UStatus` back:

| errno (C return) | UCode (`hdr.status` / UStatus.code) | Situation |
|------------------|--------------------------------------|-----------|
| `EOK` 0 | `OK` 0 | success |
| `ETIMEDOUT` | `DEADLINE_EXCEEDED` 4 | `TimerTimeout` fired; server got `_PULSE_CODE_UNBLOCK` |
| `EINTR`, `ECANCELED` | `CANCELLED` 1 | signal / explicit cancel / `on_cancel` path |
| `EINVAL`, `EMSGSIZE`, `E2BIG` | `INVALID_ARGUMENT` 3 | header/payload validation failed |
| `ERANGE` | `OUT_OF_RANGE` 11 | `len > max_len`, `count > max_count` |
| `ENOENT` | `NOT_FOUND` 5 | `name_open` failed; unknown method/topic |
| `EEXIST` | `ALREADY_EXISTS` 6 | duplicate subscribe / register |
| `EACCES`, `EPERM` | `PERMISSION_DENIED` 7 | secpol channel rule; `qmp.allow_types` / permission level |
| `EAGAIN`, `ENOMEM`, `ENOSPC`, `EMFILE`, `ENFILE` | `RESOURCE_EXHAUSTED` 8 | deferred pool full, shm handle limit |
| `ENOTSUP`, `EPROTO` | `FAILED_PRECONDITION` 9 | major version mismatch at HELLO |
| `ESRCH`, `EBADF`, `ECONNRESET`, `ENOTCONN` | `UNAVAILABLE` 14 | server died / connection gone (client will reconnect) |
| `ENOSYS` | `UNIMPLEMENTED` 12 | `_IO_*` message, method without handler |
| `EIO` | `DATA_LOSS` 15 | QUEUE overflow dropped samples (reported on PULL) |
| `EFAULT`, other | `INTERNAL` 13 / `UNKNOWN` 2 | runtime bug / unmapped |

The reverse direction (UCode → errno) is only used by the adapter and uses the first errno in
each row. `UNAUTHENTICATED` (16) maps to `EACCES` on the way in.

---

## 7. Identifier mapping

Verified against up-spec `v1.5.8` `basics/uri.adoc`: `ue_id` low 16 bits = service id, high
16 bits = instance id; instance `0x0000` is the default; `0xFFFF` is a wildcard;
resources `0x0000–0x7FFF` methods, `0x8000–0xFFFF` topics.

| Concept | uProtocol (UUri / options) | qmp header / IDL | QNX |
|---------|---------------------------|------------------|-----|
| node | `authority_name` ("" = local) | — (single node) | — |
| service type | `ue_id & 0xFFFF` ⇐ `(uprotocol.service_id)` | `service_id` | part of name |
| instance | `ue_id >> 16` (0 = default) | `instance_id` (0 = default) | part of name |
| major version | `ue_version_major` ⇐ `(uprotocol.service_version_major)` | `ver_major` | `/v<major>` suffix |
| minor version | `(uprotocol.service_version_minor)` (not in UUri) | `ver_minor` | negotiated at HELLO |
| method | `resource_id` 0x0000–0x7FFF ⇐ `(uprotocol.method_id)` | `resource_id` | — |
| topic | `resource_id` 0x8000–0xFFFF ⇐ `publish_topic.id` | `resource_id`; pulse code `QMP_PULSE_TOPIC` | `MsgRegisterEvent` handle |
| endpoint | `/101/1/8001` (URI path, upper-case base16, no leading zeros) | — | `name_attach("qmp/0101-0000/v1")` → `/dev/name/local/qmp/0101-0000/v1` |
| permission | `method_permission_level` / `service_permission_level` | checked vs `ConnectClientInfoExt` | secpol type (`allow joint_ctrl_t joint_svc_t:channel connect;`) |

**Deviation note:** the project brief's example name `qmp/0101-0001/v1` uses instance 1 as if
it were the default. uProtocol says the default instance is `0x0000`; qmp follows uProtocol, so
the default name is `qmp/0101-0000/v1` (recorded in ADR-005).

Permission-level mapping (draft, VERIFY V-UP-005): a method's `method_permission_level` *L*
(default: service level, default 0) is satisfied when the caller's secpol type appears in
`qmp.allow_types` **or** when the caller's uid is 0 and *L* ≤ 1. Unknown/unset → allow. The
exact semantics are refined in W4 when secpol generation lands.

---

## 8. Explicit v1 non-goals

* Cross-node communication (Qnet is gone in 8.0; `authority_name` must be empty).
* Protobuf wire encoding on the native path; `maps`, `oneof`, `Any`, nested unbounded
  repeateds, proto2, streaming RPCs — all rejected by the generator with clear errors.
* Dynamic discovery / registry service beyond `/dev/name/local`. No uDiscovery, uSubscription
  or uTwin implementation; the adapter is uP-L1 only.
* Schema evolution beyond additive minor versions (no field removal, no renumbering).
* Encryption or authentication beyond what QNX secpol and `ConnectClientInfoExt` provide.
* Hidden threads in the client library; async futures/coroutines in the C++ layer.
* Zero-copy for *small* messages (the kernel copy is the design); shm bulk transfer is stretch.
* Hot-path heap allocation, formatted logging, or blocking I/O.
* Support for SDP < 8.0, ARMv7, `__QNXNTO__`, io-pkt, APS, PPS.
* Generating Rust, Python or Java bindings.
