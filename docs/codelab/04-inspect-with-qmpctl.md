# Codelab 04 — Inspect and exercise a running service with qmpctl

**You will:** list the services on a node, read a service's interface straight from the
running process, call methods with JSON arguments, watch a topic as JSON lines, and
understand how a tool without any generated code can do all of that.

**Time:** ~20 min. **Needs:** Codelab 02 (host build working). The Pi 5 part is optional; the
commands are identical there.

---

## 1. The idea: the service describes itself

`protoc-gen-qmp` writes `joint.reflect.json` next to the bindings and embeds the same bytes
in `joint.qmp.c` (Codelab 01). The generated service descriptor points at that blob, and the
runtime answers one extra control message, `QMP_OP_REFLECT`, with a window of it:

```
qmpctl                                           qmp_joint_server
  walk the names            /dev/name/local/qmp/0101-0000/v1   (uds: $QMP_UDS_DIR/qmp.0101-0000.v1.sock)
  HELLO                     ───────────────────►
  REFLECT {offset, max_len} ───────────────────►  reply: {total_len, len} + bytes of the JSON
  ... until offset == total_len
```

So `qmpctl` links only the C ABI (`libqmp`) and contains **no generated code**. It converts
JSON to the fixed-layout payload — and back — using the offsets and bounds from that
document. There is no registry process either: the names the servers attached *are* the
registry (ADR-010, DESIGN §3.9).

## 2. Start a server and look around (host)

```sh
cmake --build build/host-debug
export QMP_UDS_DIR=$(mktemp -d)          # where the uds transport keeps its sockets
build/host-debug/examples/joint_demo/qmp_joint_server --publish-ms 50 &
alias qmpctl=$PWD/build/host-debug/tools/qmpctl/qmpctl

qmpctl list
```

```
NAME              SERVICE           VERSION  METHODS  TOPICS  UURI
qmp/0101-0000/v1  demo.joint.Joint  1.0      4        2       /101/1
```

`describe` prints the interface the *running binary* was built from:

```sh
qmpctl describe Joint
```

```
service demo.joint.v1.Joint
  name      demo.joint.Joint
  id        0x0101  instance 0x0000
  version   1.0
  uuri      /101/1
  endpoint  qmp/0101-0000/v1  (transport uds)
  runtime   0.3.0  (wire 1)
  generator protoc-gen-qmp 0.3.0

methods:
  0x0001  SetTarget(demo.joint.v1.SetTargetRequest) returns (demo.joint.v1.SetTargetReply)
  0x0002  Stop(demo.joint.v1.Empty) returns (demo.joint.v1.Empty)  [oneway]
  0x0003  GetState(demo.joint.v1.Empty) returns (demo.joint.v1.JointState)
  0x0004  Burn(demo.joint.v1.BurnRequest) returns (demo.joint.v1.BurnReply)

topics:
  0x8001  StateChanged  demo.joint.v1.JointState  LATEST
  0x8002  StateLog  demo.joint.v1.JointState  QUEUE depth 8

message demo.joint.v1.JointState {  // 48 bytes
  uint64 seq = 1;           // offset 0
  uint64 timestamp_ns = 2;  // offset 8
  float position_rad = 3;   // offset 16
  float velocity = 4;       // offset 20
  string mode = 5;          // offset 24, max_len 16
}
...
```

A service can be addressed by any of its names (`Joint`, `demo.joint.v1.Joint`,
`demo.joint.Joint`) or by id: `0101`, `0101-0000`, `0101-0000/v1` (service-instance, hex,
major version). Use the id form when two instances of one service are running.

Narrow `describe` to one method and it also shows the argument template:

```sh
qmpctl describe Joint SetTarget
```

```
method Joint.SetTarget
  0x0001  SetTarget(demo.joint.v1.SetTargetRequest) returns (demo.joint.v1.SetTargetReply)
  uuri  /101/1/1

message demo.joint.v1.SetTargetRequest {  // 8 bytes
  float position_rad = 1;  // offset 0
  float max_velocity = 2;  // offset 4
}
message demo.joint.v1.SetTargetReply {  // 8 bytes
  int32 result = 1;  // offset 0
}

example:
  qmpctl call 0101-0000/v1 SetTarget '{"position_rad":0.0,"max_velocity":0.0}'
```

`--json` gives the machine-readable form (for `describe <service>` it is the reflection
document itself).

## 3. Call methods

```sh
qmpctl call Joint SetTarget '{"position_rad": 1.5, "max_velocity": 2}'
# {"result":0}
qmpctl call Joint GetState --pretty
```

```json
{
  "seq": 12,
  "timestamp_ns": 6701411413818,
  "position_rad": 1.5,
  "velocity": 0.0,
  "mode": "hold"
}
```

Arguments can also be `key=value` pairs (`a.b=1` reaches into a nested message), `@file`, or
`-` for stdin:

```sh
qmpctl call Joint SetTarget position_rad=0.25 max_velocity=1
echo '{"duration_ms": 20}' | qmpctl call Joint Burn -
```

Fields you leave out keep their proto3 default. Everything else is strict — an unknown
field, a wrong type, a string longer than `max_len`, too many repeated items are errors
*before* anything is sent:

```sh
qmpctl call Joint SetTarget position=1
# qmpctl: SetTarget: position: no such field in demo.joint.v1.SetTargetRequest (fields: position_rad, max_velocity)
```

The two error channels of qmp stay visible (DESIGN §6). An application status comes back
with its uProtocol name, a transport error as an errno; both exit with code 1:

```sh
qmpctl call Joint SetTarget position_rad=1 max_velocity=0
# qmpctl: Joint.SetTarget returned status 3 (INVALID_ARGUMENT)
```

JSON mapping in one line each: integers are numbers (64-bit values keep full precision; a
decimal or `0x` string is accepted on input); `float`/`double` are numbers, with `"NaN"`,
`"Infinity"` and `"-Infinity"` for the non-finite values; an enum is its value name; `bytes`
is base64; a nested message is an object; a repeated field is an array.

## 4. Watch a topic

```sh
qmpctl subscribe Joint StateChanged --count 3
```

```
{"topic":"StateChanged","seq":12,"dropped":0,"rx_ns":1795772308583255248,"sample":{"seq":13,"timestamp_ns":6701446871968,"position_rad":1.5,"velocity":0.0,"mode":"hold"}}
{"topic":"StateChanged","seq":13,"dropped":0,"rx_ns":1795772308633793574,"sample":{"seq":14,"timestamp_ns":6701497317038,"position_rad":1.5,"velocity":0.0,"mode":"hold"}}
{"topic":"StateChanged","seq":14,"dropped":0,"rx_ns":1795772308683830342,"sample":{"seq":15,"timestamp_ns":6701547419459,"position_rad":1.5,"velocity":0.0,"mode":"hold"}}
qmpctl: 3 updates received, 0 dropped
```

One JSON object per line on stdout (pipe it into `jq`, a file, a plotter); the summary goes
to stderr. `seq` is the topic's sequence number, `dropped` the updates lost between the
previous line and this one (Codelab 03: LATEST overwrites, QUEUE overflows — never
silently), `rx_ns` the wall-clock time the sample was pulled, in nanoseconds since the epoch.

Without `--count` or `--duration MS` it runs until Ctrl-C. Kill the server while it runs and
start it again:

```
qmpctl: qmp/0101-0000/v1 went away, reconnecting
qmpctl: reconnected (server generation 2025119488)
```

The subscription is re-registered on the new server and the lines continue (sequence
numbers restart with the new server generation).

## 5. The same on the Pi 5

The target build produces `qmpctl` too; there it uses native message passing by default,
and `list` walks `/dev/name/local/qmp`:

```sh
scp build/qnx-aarch64le/tools/qmpctl/qmpctl $QMP_TARGET:/tmp/qmp/
ssh $QMP_TARGET '/tmp/qmp/qmp_joint_server --publish-ms 50 &  sleep 1
                 /tmp/qmp/qmpctl list
                 /tmp/qmp/qmpctl call Joint GetState'
```

A `qmpctl call` is an ordinary `MsgSend`: the server's worker inherits qmpctl's priority.
Run a call with `on -p 40 /tmp/qmp/qmpctl call Joint Burn duration_ms=2000 spin=true` and
look at `pidin` in a second shell to see the server thread at priority 40 while it works.

`scripts/target-smoke.sh` runs these steps as scenarios 8a–8d (list/describe, call with an
application error, subscribe, subscribe across a server restart).

## 6. Services that cannot describe themselves

A hand-written `qmp_service_desc_t` (like `examples/smoke`) or a binary built before the
reflection opcode existed answers `ENOSYS`. `list` shows such a service with
`(no reflection data ...)`; pass the JSON file the generator wrote instead:

```sh
qmpctl --reflect build/host-debug/gen/joint/joint.reflect.json call 0101 GetState
```

## What you learned

* A generated qmp service carries its own interface description and serves it on request;
  tools need the C ABI and nothing else.
* `qmpctl list | describe | call | subscribe` — and that `call` is a real, priority-inheriting
  client call, not a side channel.
* Errors stay in their two channels: errno for transport, UCode for the application.

Options at a glance: `--transport qnx|uds|fake`, `--timeout MS` (default 2000, 0 = wait
forever), `--reflect FILE`, `--json`, `--pretty`, `--count N`, `--duration MS`.
