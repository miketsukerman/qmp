# Codelab 01 — Define a service and call it (C first, C++ sidebar)

**You will:** write a service schema in Protobuf with uProtocol + qmp options, generate C11
bindings, implement the server side with a plain ops table, call it from a C client, and run
it all on your Linux host over the in-process `fake` transport — the same code later runs
unchanged over QNX native message passing.

**Time:** ~30 min. **Needs:** the host build from the README ("Build from a clean machine").

What's different from a sockets/RPC framework: there is no serialisation. The generated
message types are fixed-layout C structs whose size and offsets are proven at compile time, and
a call is one `MsgSendv` → server thread (running at *your* priority) → `MsgReplyv`.

---

## 1. Write the schema

Create `idl/examples/demo/joint/v1/joint.proto` (it already exists in the repo — read along):

```proto
syntax = "proto3";
package demo.joint.v1;

import "uprotocol/uoptions.proto";   // Eclipse uProtocol service options (vendored, v1.5.8)
import "qmp/options.proto";          // qmp-only bounds and modes (ext. 50000–50999)

message SetTargetRequest {
  float position_rad = 1;
  float max_velocity = 2;
}

message SetTargetReply {
  int32 result = 1;
}

message JointState {
  uint64 seq = 1;
  uint64 timestamp_ns = 2;
  float position_rad = 3;
  float velocity = 4;
  string mode = 5 [(qmp.max_len) = 16];   // REQUIRED: every string/bytes/repeated is bounded
}

message Empty {}                           // zero-field: payload_len = 0, no struct emitted

service Joint {
  option (uprotocol.service_id) = 0x0101;            // 16-bit, part of the QNX path
  option (uprotocol.service_name) = "demo.joint.Joint";
  option (uprotocol.service_version_major) = 1;      // must match between client and server
  option (uprotocol.service_version_minor) = 0;      // additive only
  option (uprotocol.publish_topic) = { id: 0x8001 name: "StateChanged" message: "JointState" };
  option (qmp.topic_mode) = { topic_id: 0x8001 mode: MODE_LATEST };

  rpc SetTarget(SetTargetRequest) returns (SetTargetReply) { option (uprotocol.method_id) = 0x0001; }
  rpc Stop(Empty) returns (Empty) { option (uprotocol.method_id) = 0x0002; option (qmp.oneway) = true; }
  rpc GetState(Empty) returns (JointState) { option (uprotocol.method_id) = 0x0003; }
}
```

Rules the generator enforces (errors are `file:line: error: …`, all at once):

| Allowed | Rejected in v1 |
|---------|----------------|
| scalars, enums, nested messages (≤ 4 deep), `string`/`bytes` with `qmp.max_len`, `repeated` with `qmp.max_count` | `map<>`, `oneof`, `google.protobuf.Any`/well-known types, unbounded fields, proto2, streaming rpc, recursive messages |
| method ids `0x0000–0x7FFF`, topic ids `0x8000–0xFFFF` | duplicates, out-of-range ids, `qmp.oneway` with a non-empty reply |

Try it: remove `[(qmp.max_len) = 16]`, rebuild, and read
`joint.proto:27: error: field 'mode': string requires (qmp.max_len)`.

## 2. Generate the bindings

The build does this for you (`cmake/QmpCodegen.cmake`):

```cmake
qmp_add_service(NAME joint PROTO ${PROJECT_SOURCE_DIR}/idl/examples/demo/joint/v1/joint.proto)
```

or by hand:

```sh
protoc -I third_party/up-spec/up-core-api -I idl \
       --plugin=protoc-gen-qmp=$(which protoc-gen-qmp) \
       --qmp_out=flat:/tmp/gen examples/demo/joint/v1/joint.proto
ls /tmp/gen      # joint.qmp.h  joint.qmp.c  joint.reflect.json
```

Open `joint.qmp.h`. The interesting parts:

```c
#define DEMO_JOINT_V1_JOINT_SERVICE_ID   0x0101U
#define DEMO_JOINT_V1_JOINT_UURI         "/101/1"            /* uProtocol URI, no authority */
#define DEMO_JOINT_V1_JOINT_QNX_PATH     "qmp/0101-0000/v1"  /* /dev/name/local/… on QNX   */
#define DEMO_JOINT_V1_JOINT_M_SETTARGET  0x0001U
#define DEMO_JOINT_V1_JOINT_T_STATECHANGED 0x8001U
#define DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE (24U + 48U)         /* header + largest payload  */

typedef struct demo_joint_v1_JointState_mode_t {
    uint16_t len;
    char data[16];
} demo_joint_v1_JointState_mode_t;                           /* 18 bytes, align 2 */

typedef struct demo_joint_v1_JointState {
    uint64_t seq;                                            /* 0   field 1 */
    uint64_t timestamp_ns;                                   /* 8   field 2 */
    float position_rad;                                      /* 16  field 3 */
    float velocity;                                          /* 20  field 4 */
    demo_joint_v1_JointState_mode_t mode;                    /* 24  field 5 */
    uint8_t _pad0[6];                                        /* explicit padding, zeroed */
} demo_joint_v1_JointState;                                  /* 48 bytes, align 8 */

QMP_STATIC_ASSERT(sizeof(demo_joint_v1_JointState) == 48, ...);
QMP_STATIC_ASSERT(offsetof(demo_joint_v1_JointState, mode) == 24, ...);
```

Layout is computed by the generator (field-number order, natural alignment, size padded to
8) and then *asserted* by the compiler, so a C client and a C++ server can never disagree about
bytes. The same header compiles as C11 and as C++17.

`joint.reflect.json` carries the same information for tools (`qmpctl`, W3) and is also embedded
in `joint.qmp.c` as `demo_joint_v1_Joint_reflect_json`.

## 3. Implement the server in C

The server side is an **ops table**: one function pointer per method plus a `void *ctx`. The
runtime validates the header, the version and the payload bounds *before* your handler runs.

```c
#include <string.h>
#include <qmp/qmp_server.h>
#include "joint.qmp.h"

typedef struct { float position_rad; uint64_t seq; } joint_model_t;

/* Runs on a libqmp worker thread AT THE CALLER'S PRIORITY (inherited through MsgReceive). */
static int on_set_target(void *ctx, qmp_req_t *req,
                         const demo_joint_v1_SetTargetRequest *in,
                         demo_joint_v1_SetTargetReply *out, int32_t *status) {
    joint_model_t *m = ctx;
    (void)req;                                   /* used for deferred replies (W2) */
    if (in->max_velocity <= 0.0f) {
        *status = QMP_UCODE_INVALID_ARGUMENT;    /* application error → UCode, not errno */
        return QMP_HANDLED;
    }
    m->position_rad = in->position_rad;
    m->seq++;
    out->result = 0;
    return QMP_HANDLED;                          /* runtime replies with *out and *status */
}

static int on_get_state(void *ctx, qmp_req_t *req, demo_joint_v1_JointState *out, int32_t *status) {
    joint_model_t *m = ctx;
    (void)req; (void)status;
    out->seq = m->seq;
    out->position_rad = m->position_rad;
    static const char mode[] = "hold";
    out->mode.len = sizeof(mode) - 1;
    memcpy(out->mode.data, mode, sizeof(mode) - 1);
    return QMP_HANDLED;
}

#define NTHREADS 2
/* All storage is caller-provided: no malloc after init. */
static uint64_t g_workbuf[QMP_SERVER_WORKBUF_SIZE(DEMO_JOINT_V1_JOINT_MAX_MSG_SIZE, NTHREADS) / 8];

int main(void) {
    joint_model_t model = {0};
    demo_joint_v1_Joint_ops ops = {0};
    ops.SetTarget = on_set_target;
    ops.GetState  = on_get_state;

    qmp_server_t server;
    qmp_server_cfg_t cfg = { NULL /* default transport */, NTHREADS, g_workbuf, sizeof g_workbuf };
    if (qmp_server_init(&server, &cfg) != EOK) return 1;
    if (demo_joint_v1_Joint_register(&server, 0 /* instance */, &ops, &model) != EOK) return 1;
    return qmp_server_run(&server) == EOK ? 0 : 1;   /* N workers RECEIVE-blocked on one channel */
}
```

Return conventions, everywhere in qmp:

* Functions return `EOK` or an errno. A handler returning a positive errno makes the runtime
  `MsgError(rcvid, errno)` so the client's call fails with that errno.
* Application outcomes go in `*status` as a uProtocol `UCode` (`QMP_UCODE_*`). The client gets
  `EOK` from the call and reads `status`.
* Methods not implemented (NULL in the ops table) answer `ENOSYS`; unknown method ids `ENOENT`;
  a wrong major version `ENOTSUP`; an out-of-bounds string `ERANGE`; a wrong size `EINVAL`.

## 4. Call it from a C client

```c
#include <stdio.h>
#include <qmp/qmp_client.h>
#include "joint.qmp.h"

int main(void) {
    qmp_client_t c;                                           /* caller-provided storage */
    if (qmp_client_init(&c, NULL) != EOK) return 1;
    int rc = demo_joint_v1_Joint_connect(&c, 0, NULL);        /* name_open + HELLO handshake */
    if (rc != EOK) { fprintf(stderr, "connect: %d\n", rc); return 1; }

    demo_joint_v1_SetTargetRequest req = { .position_rad = 0.5f, .max_velocity = 1.0f };
    demo_joint_v1_SetTargetReply rep;
    int32_t status = 0;
    rc = demo_joint_v1_Joint_SetTarget(&c, &req, &rep, &status);   /* one MsgSendv */
    printf("SetTarget: rc=%d status=%d result=%d\n", rc, status, rep.result);

    demo_joint_v1_JointState st;
    rc = demo_joint_v1_Joint_GetState(&c, &st, &status);
    printf("GetState: seq=%llu pos=%.2f mode=%.*s\n", (unsigned long long)st.seq,
           st.position_rad, st.mode.len, st.mode.data);

    qmp_client_close(&c);
    return 0;
}
```

`connect` sends `QMP_HELLO {service_id, instance_id, ver_major, ver_minor}`; the server answers
with its own version and a `server_generation` (changes when the server restarts), or
`ENOTSUP` on a major mismatch. Need a timeout or a uProtocol priority on one call? Use the
`_ex` variant with `qmp_call_opts_t{timeout_ns, up_priority}`.

## 5. Run it on the host

The repo's example does exactly this in one process (server + self-test client thread) over
the `fake` transport:

```sh
cmake --build build/host-debug --target joint_service
./build/host-debug/examples/joint_demo/joint_service --selftest
```

Expected:

```
joint_service: demo.joint.Joint listening on qmp/0101-0000/v1
selftest: seq=1 position=0.50 mode=hold
```

On QNX (W2) the same binaries use `name_attach`/`MsgSendv`; the fake transport is only a host
stand-in with the same blocking semantics (send-blocked client, receive-blocked workers).

## 6. Sidebar: the same service from C++17

The generated header is `extern "C"` and the structs are standard-layout, so C++ uses them as
is — no conversions, no copies. A minimal RAII wrapper over the C ABI:

```cpp
#include <optional>
#include <qmp/qmp_client.h>
#include "joint.qmp.h"

namespace demo::joint::v1 {

class JointClient {
public:
    JointClient() { qmp_client_init(&c_, nullptr); }
    ~JointClient() { qmp_client_close(&c_); }
    JointClient(const JointClient&) = delete;
    JointClient& operator=(const JointClient&) = delete;

    int connect(uint16_t instance = 0) { return demo_joint_v1_Joint_connect(&c_, instance, nullptr); }

    // errno in .first (EOK on transport success), UCode in .second
    std::pair<int, int32_t> setTarget(const demo_joint_v1_SetTargetRequest& req,
                                      demo_joint_v1_SetTargetReply& rep) {
        int32_t status = 0;
        int rc = demo_joint_v1_Joint_SetTarget(&c_, &req, &rep, &status);
        return {rc, status};
    }

    std::optional<demo_joint_v1_JointState> getState() {
        demo_joint_v1_JointState st{};
        int32_t status = 0;
        if (demo_joint_v1_Joint_GetState(&c_, &st, &status) != EOK || status != 0) return std::nullopt;
        return st;
    }

private:
    qmp_client_t c_{};
};

}  // namespace demo::joint::v1
```

Server-side in C++: give the ops table static member functions and pass `this` as `ctx`:

```cpp
struct JointServer {
    static int setTarget(void* ctx, qmp_req_t*, const demo_joint_v1_SetTargetRequest* in,
                         demo_joint_v1_SetTargetReply* out, int32_t* status) {
        return static_cast<JointServer*>(ctx)->onSetTarget(*in, *out, *status);
    }
    int onSetTarget(const demo_joint_v1_SetTargetRequest&, demo_joint_v1_SetTargetReply&, int32_t&);
};
```

The generated `*.qmp.hpp` (W2) will ship exactly this kind of wrapper (`qmp::Result<T>`, RAII
client/server, virtual-method service base) so you don't write it yourself; the C structs stay
the single source of truth. Rules: C++17 only, no exceptions, no RTTI, header-only.

## 7. What you learned

* A service is a proto3 file with uProtocol identity options and qmp bounds — no wire format to
  design, no serialisation code to write.
* Generated payloads are fixed-layout PODs with compile-time proofs; C and C++ share them.
* Servers are ops tables; handlers run at the caller's priority; application errors are UCodes,
  transport errors are errnos.
* Everything is caller-allocated; nothing on the call path allocates.

**Next:** Codelab 02 — deferred replies, one-way methods and topics on real QNX hardware.
