Prepare the submission. Post a plan first and wait for my OK.
- Hardening:
  - fuzz header and payload validation on the host
  - resolve or document every VERIFY item
  - zero warnings, sanitizer-clean host run
- README:
  - 3-sentence pitch and architecture diagram
  - quick start for host and RPi4
  - language bindings (C primary, C++ optional)
  - benchmark table with a methodology link
  - uProtocol compatibility statement: pinned version, what is and isn't supported
  - limitations
  - roadmap: cross-node via a uProtocol Zenoh or SOME/IP gateway (Qnet is gone in 8.0),
    upstreaming the transport to eclipse-uprotocol, resource-manager integration
    (qmp_server_attach_dispatch), fields/properties, Rust bindings via bindgen, tracing
- Final codelab chapter: the priority-inversion experiment.
- 3-minute demo video script: shot list, commands and expected output.
- Submission write-up: problem, solution, the QNX technologies used and why each matters, C ABI
  and uProtocol interoperability, results, next steps. Every claim must be backed by our
  measured data.