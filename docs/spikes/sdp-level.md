# Spike: installed SDP patch level, libc++ version and C++ standard (ADR-007)

**Time box:** 30 min. **Resolves:** V-QNX-020, feeds V-QNX-019/V-QNX-025.
**Status:** commands prepared; needs a host with QNX SDP 8.0 (not available in this
environment). Paste the output below the commands.

## Commands (host, after `source ~/qnx800/qnxsdp-env.sh`)

```sh
# 1. SDP level and installed patches
echo "$QNX_TARGET"                                   # …/qnx800/target/qnx
cat "$QNX_TARGET/etc/qversion" 2>/dev/null           # e.g. 8.0.0 or 8.0.2  (VERIFY: file name)
ls "$QNX_HOST/../.."/*.txt 2>/dev/null | head        # some SDPs drop release notes here
ls "$QNX_TARGET"/etc/                                # look for *version*, *release*, patches/

# 2. Compiler and available -V variants
qcc -V 2>&1 | head -20                               # lists "gcc_ntoaarch64le", "..._gpp"?, "..._cxx"?
ls "$QNX_HOST/etc/qcc/gcc/"                          # 12.2.0
ls "$QNX_HOST/etc/qcc/gcc/12.2.0/"                   # default + per-variant config files
qcc -Vgcc_ntoaarch64le -v 2>&1 | grep -E "gcc version|Target"

# 3. libc++ version (16 in 8.0, 18 in 8.0.2)
grep -rn "define _LIBCPP_VERSION" "$QNX_TARGET/usr/include/c++/v1/__config" \
                                   "$QNX_TARGET/usr/include/c++/v1/__config_site" 2>/dev/null
echo '#include <version>
int main(){return _LIBCPP_VERSION;}' > /tmp/v.cpp
q++ -Vgcc_ntoaarch64le -std=c++17 -E -dM /tmp/v.cpp | grep -E "_LIBCPP_VERSION|__cplusplus"

# 4. Does the _gpp variant exist / what does it select?   (V-QNX-025)
q++ -Vgcc_ntoaarch64le_gpp -std=c++17 -E -dM /tmp/v.cpp 2>&1 | grep -E "_LIBCPP_VERSION|__GLIBCXX__|error" | head
q++ -Vgcc_ntoaarch64le     -std=c++17 -E -dM /tmp/v.cpp 2>&1 | grep -E "_LIBCPP_VERSION|__GLIBCXX__|error" | head

# 5. C11 + C++17 compile of a generated header with the real toolchain (V-QNX-019)
d=codegen/tests/golden/joint
qcc -Vgcc_ntoaarch64le -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -I$d -c $d/joint.qmp.c -o /tmp/j_c.o && echo C11_OK
q++ -Vgcc_ntoaarch64le -std=c++17 -Wall -Wextra -Werror -x c++ -Iinclude -I$d -c $d/joint.qmp.c -o /tmp/j_cxx.o && echo CXX17_OK
```

### Expected output (to confirm)

* `qversion` → `8.0.x`. If `x ≥ 2`, libc++ should be 18 (`_LIBCPP_VERSION 180000`); on 8.0.0
  expect `160000`.
* `gcc version 12.2.0`.
* Step 4: if the `_gpp` line prints `error: unknown variant`, switch `CMAKE_CXX_COMPILER_TARGET`
  in `cmake/qnx-common.cmake` to `gcc_nto<arch>` (what the upstream uProtocol QNX profile uses)
  and close V-QNX-025. If it prints `__GLIBCXX__`, `_gpp` means GNU libstdc++ — keep `_gpp` only
  if libstdc++ is really shipped in your SDP; otherwise prefer libc++ for ABI consistency with
  the rest of the image.
* Step 5: both `_OK` lines. Any `-Wpedantic` complaint there is a generator bug (file it).

## Results

_(paste here)_

```
qversion:
qcc -V:
_LIBCPP_VERSION:
_gpp variant:
C11_OK / CXX17_OK:
```

## Proposed ADR-007 outcome

**Stay on C++17; do not introduce `QMP_CXX20` in the hackathon timeframe.** Reasons:

1. Judges build on "whichever 8.0 image they have" — 8.0.0 (libc++ 16) is the common
   denominator and its C++20 library (`<format>`, `<ranges>`, `std::jthread`) is incomplete.
2. Nothing in the W1–W4 scope needs C++20: `Result<T>` is a hand-rolled discriminated union,
   `qmp::Span<T>` replaces `std::span`, callbacks are function pointers + `void *ctx` (C ABI).
3. Avoiding C++20 also sidesteps the known-broken modules support on QNX.

If the spike shows 8.0.2 (libc++ 18) *and* a measurable benefit appears later (e.g. `std::span`
in `tools/qmpctl`), add `QMP_CXX20` for `tools/` and `examples/` only, as ADR-007 already allows.
Mark ADR-007 **accepted** with that text once the paste-back confirms the libc++ version.
