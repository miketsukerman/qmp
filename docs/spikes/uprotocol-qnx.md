# Spike: building up-core-api + up-cpp with q++ for aarch64le

**Time box:** 2 h (W1). **Owner:** codegen/adapter. **Resolves:** V-UP-009, ADR-005 §5 input.
**Status:** host-side analysis complete; the target compile could **not** be run — this
environment has no QNX SDP (licence). Commands below are ready to paste.

## What up-cpp needs

Taken from `eclipse-uprotocol/up-cpp` tag `v1.0.1` (latest release, 2024) and
`eclipse-uprotocol/up-conan-recipes` `main`, read 2026-11-09:

| Dependency | Version pinned by up-cpp | Notes |
|------------|--------------------------|-------|
| protobuf   | 3.21.12 (recipes also offer 3.15.0, 5.27.2) | full C++ runtime (`libprotobuf`), not lite |
| spdlog     | 1.13.0 | header-only ok; pulls fmt |
| up-core-api| `[~1.6, include_prerelease]` → **1.6.0-alpha.2…alpha.4** | **not** v1.5.8 |
| gtest      | 1.14.0 | tests only |
| build      | CMake ≥ 3.10, **Conan 2.x**, `CMAKE_CXX_STANDARD 17`, `-Wconversion -Werror` relaxed via `CMAKE_CXX_FLAGS_INIT` | |

Upstream already documents a QNX cross build in
`up-conan-recipes/README.md` §"Building Release Packages for QNX (cross-compile)" with Conan
profiles `tools/profiles/nto-8.0-aarch64-le` and `nto-8.0-x86_64`. The 8.0 aarch64 profile is:

```
compiler=qcc, compiler.version=12.2, compiler.cppstd=gnu14 (gnu17 noted as 8.0 default)
compiler.libcxx=cxx          # LLVM libc++
CC=qcc -Vgcc_ntoaarch64le
CXX=q++ -Vgcc_ntoaarch64le   # NOTE: no _gpp suffix — see finding 3
CFLAGS=CXXFLAGS=-D_XOPEN_SOURCE=700 -D_QNX_SOURCE
```

## Findings

1. **Version skew.** up-cpp 1.0.x is built against up-core-api **1.6.0-alpha**, whereas qmp pins
   up-spec **v1.5.8** (ADR-005). The `uoptions.proto` extension numbers are identical between the
   two (V-UP-001), so *our generated code* is unaffected, but the up-cpp library embeds
   `uprotocol/v1/*.pb.h` from 1.6.0-alpha. Linking up-cpp into the adapter would mean two copies
   of the uProtocol protos in one tree (ours for codegen, theirs inside up-cpp). Acceptable only
   if we never compile *our* `v1/*.proto` to C++ — which we don't (schema language only).
2. **Heavy dependency surface for a hackathon target.** Full `libprotobuf` + spdlog + fmt must be
   cross-compiled with Conan for QNX, and `libprotobuf` is a ~2–3 MB shared object on aarch64.
   None of it belongs on the measured hot path (ADR-005), but it has to be shipped to the Pi and
   debugged there. Upstream flags Conan QNX 8.0 support as "in progress" in the profile itself.
3. **Toolchain variant.** Upstream uses `q++ -Vgcc_ntoaarch64le` with `compiler.libcxx=cxx`;
   our toolchain files use `-Vgcc_ntoaarch64le_gpp` per the project brief. In SDP 7.1 `_gpp`
   selected GNU libstdc++; in 8.0 the C++ library is LLVM libc++ (ADR-007) and it is unclear
   whether a `_gpp` variant still exists in `$QNX_HOST/etc/qcc/gcc/12.2.0/default`. Logged as
   **V-QNX-025**; the sdp-level spike prints the list of variants.
4. **No blocker in the up-cpp source itself** for C++17 on GCC 12.2/libc++ 16: it uses
   `std::optional`, `std::variant`, `std::string_view`, no coroutines, no `<format>`.

## Commands to run on a host with SDP 8.0 (paste results into this file)

```sh
source ~/qnx800/qnxsdp-env.sh
pip install "conan>=2.0"
git clone https://github.com/eclipse-uprotocol/up-conan-recipes /tmp/up-conan-recipes
cd /tmp/up-conan-recipes
conan profile detect --force                              # host (Linux) profile
conan config install tools/qnx-8.0-extension/settings_user.yml

# build chain for aarch64le (expect: 4 packages created, ~20 min)
P=tools/profiles/nto-8.0-aarch64-le
conan create --version=3.21.12 --build=missing protobuf    # host protoc first
conan create -pr:h=$P --version=3.21.12 --build=missing protobuf
conan create -pr:h=$P --version=1.6.0-alpha4 up-core-api/release/
conan create -pr:h=$P --version=1.0.1 --build=missing up-cpp/release

# smoke: list the produced libs and check they are aarch64 QNX ELF
find ~/.conan2/p -name 'libup-cpp*' -o -name 'libprotobuf.so*' | xargs file
```

Expected: `ELF 64-bit LSB shared object, ARM aarch64 … for QNX`.  Failure modes to record:
`settings_user.yml` not accepted (Conan version), protobuf `abseil` requirement for 5.x,
`_gpp` vs plain variant mismatch when linking against libc++.

## Recommendation: **NO-GO for linking up-cpp in W4; GO for wire-level alignment**

* Do **not** make `adapters/uprotocol/` depend on up-cpp/libprotobuf. Implement the uP-L1
  `UTransport` surface (`send`, `registerListener(sourceFilter, sinkFilter, listener)`,
  `unregisterListener`) as a small self-contained C++17 header set over the C ABI, with a
  minimal `UMessage{UAttributes, payload}` struct whose field set follows
  `basics/uattributes.adoc` at v1.5.8 (V-UP-007). The adapter stays header-only + C ABI,
  consistent with ADR-006.
* Keep an **optional** CMake switch `QMP_ADAPTER_USE_UPCPP=OFF` that, when ON, compiles a thin
  bridge translating our `UMessage` to `uprotocol::v1::UMessage` for interop demos *on the
  host only*. Revisit if the target chain above succeeds in < 1 h.
* Re-run this spike when up-core-api 1.6.0 is released and up-cpp pins it; the qmp pin
  (ADR-005) can then move to 1.6.0 together.

Update ADR-005 §5 with this outcome (done in the same commit).
