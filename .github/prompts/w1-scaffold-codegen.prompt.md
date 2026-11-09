Implement W1 per docs/DESIGN.md. Post a plan first and wait for my OK, then:
1. Scaffold the repo: CMake, toolchain files (qcc/q++ variants as in the instructions), CI host
   job with sanitizers, lint configs, LICENSE (Apache-2.0), and a README with a
   "build from a clean machine" section.
2. Vendor up-spec at a pinned tag in third_party/up-spec and record tag + commit in ADR-005.
   Confirm the uoptions.proto path, option names and field numbers, and list every difference
   from the instructions file.
3. Create idl/qmp/options.proto (extension numbers 50000–50999) and compiled Python modules for
   both option sets.
4. Build protoc-gen-qmp:
   - validation per codegen.instructions.md
   - C output (.h/.c) for SYNC METHODS ONLY: computed layouts with static asserts, ids, client
     functions, ops table, dispatch
   - reflection JSON and UUri constants
   - golden tests for the Joint example and for each validation error
   - a CI step that compiles the generated headers as C11 and C++17
5. runtime/core in C11: header codec, error type with the UCode mapping, dispatch table, version
   check, caller-provided storage types. Unit tests.
6. transport/fake plus an end-to-end host test: C client → fake → C server ops table → reply.
7. Spikes (time-boxed, report in docs/spikes/):
   a. uprotocol-qnx.md — try building up-core-api + up-cpp with q++ for aarch64le; give a
      go/no-go recommendation.
   b. sdp-level.md — give me the commands to print the installed SDP patch level and the
      libc++ version; propose the ADR-007 outcome.
8. Write docs/codelab/01-define-a-service.md (C first, with a C++ sidebar).
Report what builds, test results, open VERIFY items and any deviations.