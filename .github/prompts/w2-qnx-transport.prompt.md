Implement transport/qnx in C11: sync calls, one-way (immediate reply), deferred replies with
cancellation, HELLO and versioning, and lifecycle. Then add the C++ wrappers. Post a plan first
and wait for my OK.
- Server:
  - name_attach at `qmp/<sid>-<iid>/v<major>`
  - thread pool and the receive-loop order from runtime-c.instructions.md
  - per-client-process state and ConnectDetach on disconnect
  - per-method ACL hook (ConnectClientInfoExt, cached per scoid)
- Client:
  - name_open and a private channel with _NTO_CHF_COID_DISCONNECT
  - TimerTimeout-based call timeouts
  - reconnect with exponential backoff and server_generation tracking
- C++ layer: .hpp generation (Proxy, Skeleton via trampolines, Result, RAII) and golden tests.
- Target smoke tests (examples/smoke/), covering:
  - normal call
  - timeout
  - deferred reply
  - kill -9 of the client
  - kill -9 of the server, then restart
  - the full interop matrix (C/C++ client × C/C++ server)
  Give me exact RPi5 commands (via scripts/target-run.sh) and the expected output.
- Add result fields to docs/VERIFY.md and write docs/codelab/02-first-service-on-qnx.md.