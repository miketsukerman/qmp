# Repository scaffold proposal (W1)

Proposal only — nothing below exists yet except `docs/` and `.github/`. Approve or amend before
W1 step 1.

## 1. Tree

```
qmp/
├── LICENSE                          Apache-2.0
├── NOTICE                           attribution for up-spec, Jinja2, protobuf, GoogleTest
├── README.md                        overview, "build from a clean machine", manual target steps
├── CMakeLists.txt                   top level; options QMP_BUILD_TESTS/BENCH/CPP/TOOLS/ADAPTER/UDS
├── CMakePresets.json                host-debug, host-asan, host-ubsan, host-tsan, qnx-aarch64le, qnx-x86_64
├── .clang-format                    LLVM base, 4-space, 100 col, pointer-left-of-name for C
├── .clang-tidy                      bugprone-*, cert-*, misc-*, readability-* (C core)
├── .editorconfig
├── .gitattributes                   *.golden text eol=lf
├── .github/
│   ├── workflows/host.yml           see §4
│   ├── copilot-instructions.md      (exists)
│   ├── instructions/                (exists)
│   └── prompts/                     (exists)
├── cmake/
│   ├── qnx-aarch64le.cmake          toolchain: qcc -Vgcc_ntoaarch64le / q++ -Vgcc_ntoaarch64le_gpp
│   ├── qnx-x86_64.cmake             toolchain: qcc -Vgcc_ntox86_64 / q++ -Vgcc_ntox86_64_gpp
│   ├── qnx-common.cmake             shared: CMAKE_SYSTEM_NAME QNX, sysroot from $QNX_TARGET, -D__QNX__ check
│   ├── QmpCodegen.cmake             qmp_add_service(TARGET x PROTO idl/.../joint.proto CPP) → runs protoc + plugin
│   ├── QmpWarnings.cmake            per-language flag sets from the instructions
│   └── FindProtoc.cmake             locate protoc; the plugin is run from the Python venv
├── idl/
│   ├── qmp/options.proto            qmp.* extensions (50000–50999)
│   └── examples/demo/joint/v1/joint.proto
├── third_party/
│   └── up-spec/                     vendored subset @ v1.5.8 (up-core-api/uprotocol/**, LICENSE, NOTICE, PIN.md)
├── codegen/
│   ├── pyproject.toml               see §6
│   ├── protoc_gen_qmp/
│   │   ├── __init__.py
│   │   ├── __main__.py              plugin entry: stdin CodeGeneratorRequest → stdout CodeGeneratorResponse
│   │   ├── model.py                 Service/Method/Topic/Message/Field dataclasses, option extraction
│   │   ├── validate.py              v1 subset rules; error messages with file:line
│   │   ├── layout.py                C layout computation: offsets, padding, sizes, max_msg_size
│   │   ├── emit_c.py / emit_cpp.py / emit_reflect.py / emit_secpol.py
│   │   ├── templates/*.j2           service.qmp.h.j2, service.qmp.c.j2, service.qmp.hpp.j2, reflect.json.j2
│   │   └── _pb/                     generated descriptor modules: uoptions_pb2.py, options_pb2.py (checked in, regen script)
│   ├── scripts/regen_pb.sh          protoc → _pb/ from third_party + idl
│   └── tests/
│       ├── golden/joint/            joint.qmp.h/.c/.hpp/.reflect.json expected outputs
│       ├── golden/errors/*.proto    one proto per rejected construct + expected message
│       ├── test_golden.py           runs protoc with plugin, diffs
│       ├── test_layout.py           property tests for padding/alignment
│       └── test_compile.py          compiles generated header as C11 and C++17 (host cc), skipped if no cc
├── include/qmp/
│   ├── qmp.h                        version, qmp_hdr_t, flags, control ops, QMP_HANDLED/DEFERRED, errors
│   ├── qmp_errors.h                 qmp_errno_to_ucode(), qmp_ucode_to_errno(), UCode enum
│   ├── qmp_client.h                 qmp_client_t storage, init/connect/call/poll/run/close, qmp_sub_t
│   ├── qmp_server.h                 qmp_server_t storage, cfg, register, run/stop, qmp_req_t, reply, publish
│   ├── qmp_transport.h              internal transport vtable (public so transports can be out-of-tree)
│   └── cpp/
│       ├── qmp.hpp                  umbrella
│       ├── result.hpp  status.hpp  client.hpp  server.hpp  subscription.hpp  callback.hpp  span.hpp
├── runtime/
│   ├── CMakeLists.txt               libqmp (STATIC + SHARED), transport selection
│   ├── core/
│   │   ├── hdr.c                    encode/validate header
│   │   ├── dispatch.c               service/method lookup tables, payload bounds checks
│   │   ├── client.c  server.c       transport-independent state machines
│   │   ├── subs.c                   subscription registry, LATEST slot / QUEUE ring
│   │   ├── deferred.c               fixed pool, generation counters
│   │   ├── version.c                HELLO negotiation
│   │   ├── errors.c                 errno⇄UCode tables
│   │   ├── log.h                    qmp_log macro: slog2 on __QNX__, stderr on host
│   │   └── internal.h               private struct layouts + _Static_assert vs STORAGE_SIZE
│   ├── transport/
│   │   ├── qnx/qnx_transport.c      name_attach/open, Msg*, pulses, events  (#if __QNX__)
│   │   ├── uds/uds_transport.c      SOCK_SEQPACKET (host + optional QNX)
│   │   └── fake/fake_transport.c    in-process queues, thread-safe
│   └── tests/                       GoogleTest, extern "C" calls
│       ├── test_hdr.cpp  test_dispatch.cpp  test_errors.cpp  test_deferred.cpp  test_subs.cpp
│       └── test_e2e_fake.cpp        C client → fake → C server ops → reply
├── adapters/uprotocol/              (W4 stretch) CMakeLists, utransport_qmp.hpp/.cpp, tests
├── tools/qmpctl/                    (W3) main.cpp, cmd_list/call/sub/info; nlohmann/json for reflect
├── bench/                           (W4) latency.c jitter.c throughput.c prio_inversion.c, run.sh, plot.py
├── examples/joint_demo/
│   ├── CMakeLists.txt
│   ├── joint_service.c              C server using generated ops table
│   └── control_loop.cpp             C++ client using JointProxy
└── docs/
    ├── DESIGN.md  DECISIONS.md  VERIFY.md  SCAFFOLD.md  RISKS.md  BENCHMARKS.md
    ├── codelab/01-define-a-service.md … 05-benchmarks.md
    └── spikes/uprotocol-qnx.md  sdp-level.md
```

## 2. CMake layout

* Minimum 3.20. `project(qmp LANGUAGES C CXX)`; C standard 11 (`C_EXTENSIONS OFF`), C++ 17.
* Options (defaults): `QMP_BUILD_TESTS=ON`, `QMP_BUILD_CPP=ON`, `QMP_BUILD_TOOLS=ON`,
  `QMP_BUILD_BENCH=OFF`, `QMP_BUILD_ADAPTER=OFF`, `QMP_TRANSPORT_UDS=host-only ON`,
  `QMP_SANITIZER=none|asan|ubsan|tsan`.
* Transport selection is automatic: `__QNX__` → qnx + fake (+ uds if io-sock present and
  `QMP_TRANSPORT_UDS`), else uds + fake.
* Targets: `qmp::core` (OBJECT), `qmp` (STATIC, default) and `qmp_shared` (SHARED, exports via
  `QMP_API` macro), `qmp::cpp` (INTERFACE), `qmp::codegen` (custom command wrapper),
  `qmp_tests`, `qmpctl`, `joint_service`, `control_loop`, `bench_*`.
* `QmpCodegen.cmake` provides
  `qmp_add_service(NAME joint PROTO idl/examples/demo/joint/v1/joint.proto [CPP] [SECPOL] OUT_VAR)`,
  running `protoc --plugin=protoc-gen-qmp=${venv}/bin/protoc-gen-qmp -I third_party/up-spec/up-core-api -I idl`.
  Generated files go to `${CMAKE_BINARY_DIR}/gen/<pkg>/` and are added as sources, so a proto
  change rebuilds.
* GoogleTest via `FetchContent` (v1.15.x, BSD-3) only when `QMP_BUILD_TESTS`.
* `install()` exports `qmpConfig.cmake`; the plugin is installed as a console script by pip, not
  by CMake.

## 3. Toolchain files

`cmake/qnx-common.cmake` (included by both):
```cmake
set(CMAKE_SYSTEM_NAME QNX)
if(NOT DEFINED ENV{QNX_HOST} OR NOT DEFINED ENV{QNX_TARGET})
  message(FATAL_ERROR "Source qnxsdp-env.sh first")
endif()
set(CMAKE_SYSROOT "$ENV{QNX_TARGET}")
set(CMAKE_FIND_ROOT_PATH "$ENV{QNX_TARGET}/${QMP_QNX_ARCH}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_C_COMPILER   qcc)  set(CMAKE_C_COMPILER_TARGET   gcc_nto${QMP_QNX_ARCH})
set(CMAKE_CXX_COMPILER q++)  set(CMAKE_CXX_COMPILER_TARGET gcc_nto${QMP_QNX_ARCH}_gpp)
# CMake's QNX platform module turns COMPILER_TARGET into -V<target>; verified in W1 (V-QNX-019)
```
`qnx-aarch64le.cmake`: `set(QMP_QNX_ARCH aarch64le)` + `CMAKE_SYSTEM_PROCESSOR aarch64`.
`qnx-x86_64.cmake`: `set(QMP_QNX_ARCH x86_64)` + `CMAKE_SYSTEM_PROCESSOR x86_64`.
Presets `qnx-aarch64le`/`qnx-x86_64` set `CMAKE_TOOLCHAIN_FILE` and `QMP_BUILD_TESTS=ON`
(tests are built, run manually on target).

## 4. CI outline (`.github/workflows/host.yml`)

Runs on `ubuntu-24.04` on every push/PR. No SDP in CI (licence); cross build documented in README
as a manual step.

| job | steps |
|-----|-------|
| `lint` | `pip install -e codegen[dev]`; `ruff check`, `ruff format --check`, `mypy codegen`; `clang-format --dry-run -Werror` on `runtime include tools examples bench`; `cppcheck --std=c11 --error-exitcode=1 runtime/core` |
| `codegen` | `pytest codegen/tests -q` (golden + errors + compile-as-C11/C++17 with gcc/g++) |
| `host-build` (matrix `sanitizer: [none, asan, ubsan, tsan]`, `cc: [gcc-13, clang-18]`) | `cmake --preset host-$san`; `cmake --build`; `ctest --output-on-failure`; uploads generated `joint.qmp.h` as artefact |
| `clang-tidy` | `run-clang-tidy -p build runtime/core` |
| `docs` | `markdownlint docs/*.md`; checks every `VERIFY(` in sources has an id present in `docs/VERIFY.md` (small python script) |

Target steps (manual, README): `source ~/qnx800/qnxsdp-env.sh && cmake --preset qnx-aarch64le && cmake --build build/qnx-aarch64le`, then deploy with `scripts/target-run.sh` to the RPi5 (primary board; RPi4 is the secondary board, see `docs/target/rpi5.md`), run `qmp_tests` and `examples`.

## 5. Lint configs

* `.clang-format`: `BasedOnStyle: LLVM`, `IndentWidth: 4`, `ColumnLimit: 100`,
  `AlignConsecutiveMacros: true`, `AllowShortFunctionsOnASingleLine: Inline`,
  `PointerAlignment: Right` (C), `Cpp11BracedListStyle: true`. Generated files carry
  `// clang-format off` guards? **No** — generated code is formatted by the templates to pass
  clang-format so CI can lint them too.
* `.clang-tidy`: `Checks: bugprone-*,cert-*,misc-*,performance-*,readability-*,-readability-magic-numbers,-readability-identifier-length,-bugprone-easily-swappable-parameters`, `WarningsAsErrors: '*'`, `HeaderFilterRegex: '(include|runtime)/'`.
* `cppcheck`: `--enable=warning,style,performance,portability --inline-suppr --suppress=missingIncludeSystem`.
* Python: ruff (`select = ["E","F","I","B","UP","N","S","PL"]`, line 100), mypy `strict = true`.

## 6. `codegen/pyproject.toml`

```toml
[build-system]
requires = ["setuptools>=68", "wheel"]
build-backend = "setuptools.build_meta"

[project]
name = "protoc-gen-qmp"
version = "0.1.0"
description = "protoc plugin generating C11/C++17 bindings for qmp (QNX message passing)"
license = { text = "Apache-2.0" }
requires-python = ">=3.10"
dependencies = ["protobuf>=5.26,<6", "Jinja2>=3.1,<4"]

[project.optional-dependencies]
dev = ["pytest>=8", "ruff>=0.6", "mypy>=1.11", "types-protobuf", "grpcio-tools>=1.62"]

[project.scripts]
protoc-gen-qmp = "protoc_gen_qmp.__main__:main"

[tool.setuptools.packages.find]
where = ["."]
include = ["protoc_gen_qmp*"]

[tool.setuptools.package-data]
protoc_gen_qmp = ["templates/*.j2", "py.typed"]

[tool.ruff]
line-length = 100
target-version = "py310"
[tool.ruff.lint]
select = ["E", "F", "I", "B", "UP", "N", "S", "PL"]
ignore = ["S101", "PLR0913"]
exclude = ["protoc_gen_qmp/_pb/*"]

[tool.mypy]
strict = true
python_version = "3.10"
exclude = ["protoc_gen_qmp/_pb/"]
[[tool.mypy.overrides]]
module = "protoc_gen_qmp._pb.*"
ignore_errors = true

[tool.pytest.ini_options]
testpaths = ["tests"]
```

`grpcio-tools` is used only as a convenient way to get a matching `protoc` for tests
(it is Apache-2.0); system `protoc` is used if present.

## 7. Codegen rules to codify (since `codegen.instructions.md` is empty)

Proposed content for `.github/instructions/codegen.instructions.md`, to be approved separately:

* Reject with `file:line: error:` messages: proto2, `map<>`, `oneof`, `google.protobuf.Any`,
  nested messages beyond depth 4, `string/bytes/repeated` without `qmp.max_len/max_count`,
  `repeated` of `string/bytes` without both, streaming rpc, method ids ≥ 0x8000, topic ids
  < 0x8000, duplicate ids, a `qmp.oneway` method whose reply is not an empty message,
  `qmp.topic_mode` referencing an undeclared topic, `UServiceTopic.message` not resolvable,
  enum values not fitting `int32`, `max_len > 65535`, `max_count > 2^31`.
* Layout: field-number order; `float/double/intN/uintN/bool(→uint8_t)/enum(→int32_t)`;
  nested message inline; alignment = max member alignment; struct size padded to 8.
* Names: `<package with dots→_>_<Message>`; functions `<pkg>_<Service>_<Method>`;
  macros `<PKG>_<SERVICE>_M_<METHOD>` / `_T_<TOPIC>`.
* Deterministic output (sorted iteration, no timestamps) so goldens are stable.
