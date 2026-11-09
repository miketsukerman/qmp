Implement topics and tooling. Post a plan first and wait for my OK.
1. Topics (C):
   - SUBSCRIBE / UNSUBSCRIBE
   - MsgRegisterEvent with an updateable SIGEV_PULSE whose value is the sequence number
   - a per-client-process subscriber table
   - MsgDeliverEvent fan-out without holding locks
   - pull path supporting LATEST and QUEUE{depth}
   - overflow counters
   - never MsgSend from server to client
   Add the C++ subscription RAII wrapper.
2. Add transport/uds with the identical C API. Document clearly that it has NO priority
   inheritance.
3. qmpctl (C++, links only the C ABI, driven only by reflection data):
   - list: walk /dev/name/local/qmp
   - describe
   - call with JSON arguments
   - subscribe: prints JSON lines with receive timestamps
   - stats: via a built-in introspection method
   - uri: prints the uProtocol UUri
4. Host integration tests over UDS. Target scenarios:
   - subscriber crash mid-stream
   - server restart with resubscribe
   - slow subscriber in QUEUE mode
5. Write docs/codelab/03-topics-and-qmpctl.md.