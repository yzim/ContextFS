# Hardware-accelerated BLAKE3 in `cas_core`

**Status**: design approved, not yet implemented.
**Date**: 2026-05-23.
**Scope**: build-system change only. No behavior change in hash output, no callsite changes, no new public API.

## Goal

Drop the `BLAKE3_NO_AVX512 / BLAKE3_NO_AVX2 / BLAKE3_NO_SSE41 / BLAKE3_NO_SSE2 / BLAKE3_USE_NEON=0` defines at `CMakeLists.txt:156-161`, so `blake3_dispatch.c`'s runtime CPU detection picks the best SIMD path on each consumer machine.

Motivation is preemptive parity with upstream BLAKE3 defaults — no measured hotspot, just stop shipping a hash function with SIMD disabled while every consumer machine has AVX2 or NEON. The only observable difference is hashing throughput on modern CPUs.

## Non-goals

- No callsite restructuring in `object_store.cpp`. Single caller, single API, unchanged.
- No perf measurement or regression test. The point is to stop disabling, not to chase numbers.
- No assembly variants (`.S`, `.asm`). C intrinsics only — modern compilers turn them into within a few percent of the hand-written upstream asm, without needing `enable_language(ASM/ASM_MASM)` and per-toolchain source selection.
- No `BLAKE3_USE_TBB` (upstream's multi-threaded mode). Different concern, separate decision later.
- No BLAKE3 version bump beyond what's already vendored.

## Background

`include/blake3/` today vendors only the portable build:

```
blake3.c
blake3_dispatch.c
blake3.h
blake3_impl.h
blake3_portable.c
LICENSE_A2 / LICENSE_A2LLVM / LICENSE_CC0
```

`CMakeLists.txt:144-161` compiles only those three `.c` files and forces the portable path on every arch:

```cmake
target_compile_definitions(cas_core PRIVATE
    BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2
    BLAKE3_USE_NEON=0)
```

The accompanying comment makes the constraint explicit:

> "On aarch64 the vendored blake3 auto-enables NEON, but we only compile blake3_portable.c — link would fail with an unresolved blake3_hash_many_neon. Force the portable path on every arch."

The constraint is mechanical (don't reference symbols that aren't linked), not philosophical. The full upstream BLAKE3 1.8.4 distribution at `/home/zzh/agentvfs/include/blake3/` has the SIMD source files we need.

The single in-tree caller is `src/cas/object_store.cpp:78-83`:

```cpp
blake3_hasher hasher;
blake3_hasher_init(&hasher);
blake3_hasher_update(&hasher, type_tag, tag_len);
blake3_hasher_update(&hasher, body, body_len);
hash_t hash{};
blake3_hasher_finalize(&hasher, hash.data(), BLAKE3_OUT_LEN);
```

That API surface is identical between the portable and SIMD builds, so no caller changes are required.

## Design decisions

| Decision | Choice |
|---|---|
| Motivation | Preemptive parity with upstream defaults |
| Gating | Auto per-arch, `AGENTVFS_BLAKE3_SIMD=OFF` escape hatch (default ON) |
| Source layout | Vendor the SIMD `.c` files into `agentvfs-public/include/blake3/` |
| Assembly | C intrinsics only — no `.S` / `.asm`, no `enable_language(ASM/ASM_MASM)` |

## Files to vendor

Copy from `/home/zzh/agentvfs/include/blake3/` to `agentvfs-public/include/blake3/`:

```
blake3_sse2.c
blake3_sse41.c
blake3_avx2.c
blake3_avx512.c
blake3_neon.c
```

Do **not** copy: any `.S` / `.asm` file, the upstream `CMakeLists.txt`, `blake3_tbb.cpp`, `example*.c`, `main.c`, `test.py`, `Makefile.testing`, `dependencies/`, `cmake/`, `libblake3.pc.in`, `blake3-config.cmake.in`. We have our own build glue and the licence files (`LICENSE_A2`, `LICENSE_A2LLVM`, `LICENSE_CC0`) are already in place.

## CMake changes

Replace the existing block at `CMakeLists.txt:119-161` with the following (the additions are the `AGENTVFS_BLAKE3_SIMD` option and the per-arch `target_sources` / `set_source_files_properties` calls; everything else is unchanged):

```cmake
set(CAS_CORE_PORTABLE_SOURCES
    # ... existing entries unchanged ...
    include/blake3/blake3.c
    include/blake3/blake3_dispatch.c
    include/blake3/blake3_portable.c
)
set(CAS_CORE_POSIX_SOURCES
    src/cas/control_socket.cpp
)

option(AGENTVFS_BLAKE3_SIMD "Enable runtime-dispatched BLAKE3 SIMD paths" ON)

add_library(cas_core STATIC ${CAS_CORE_PORTABLE_SOURCES})
if(UNIX)
    target_sources(cas_core PRIVATE ${CAS_CORE_POSIX_SOURCES})
endif()
target_include_directories(cas_core PUBLIC src/cas include include/blake3)

if(AGENTVFS_BLAKE3_SIMD)
    set(_blake3_amd64_names amd64 AMD64 x86_64)
    set(_blake3_arm64_names aarch64 AArch64 arm64 ARM64 armv8 armv8a)

    if(CMAKE_SYSTEM_PROCESSOR IN_LIST _blake3_amd64_names)
        target_sources(cas_core PRIVATE
            include/blake3/blake3_sse2.c
            include/blake3/blake3_sse41.c
            include/blake3/blake3_avx2.c
            include/blake3/blake3_avx512.c)
        if(MSVC)
            set_source_files_properties(include/blake3/blake3_sse2.c
                PROPERTIES COMPILE_OPTIONS "/arch:SSE2")
            # MSVC has no dedicated /arch:SSE4.1 — /arch:AVX is the
            # narrowest flag that turns on SSE4.1 codegen.
            set_source_files_properties(include/blake3/blake3_sse41.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX")
            set_source_files_properties(include/blake3/blake3_avx2.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
            set_source_files_properties(include/blake3/blake3_avx512.c
                PROPERTIES COMPILE_OPTIONS "/arch:AVX512")
        else()
            set_source_files_properties(include/blake3/blake3_sse2.c
                PROPERTIES COMPILE_OPTIONS "-msse2")
            set_source_files_properties(include/blake3/blake3_sse41.c
                PROPERTIES COMPILE_OPTIONS "-msse4.1")
            set_source_files_properties(include/blake3/blake3_avx2.c
                PROPERTIES COMPILE_OPTIONS "-mavx2")
            set_source_files_properties(include/blake3/blake3_avx512.c
                PROPERTIES COMPILE_OPTIONS "-mavx512f;-mavx512vl")
        endif()
    elseif(CMAKE_SYSTEM_PROCESSOR IN_LIST _blake3_arm64_names)
        target_sources(cas_core PRIVATE include/blake3/blake3_neon.c)
        # 64-bit aarch64 has NEON unconditionally; no -mfpu flag needed.
        # blake3_dispatch.c picks NEON automatically when BLAKE3_USE_NEON
        # is unset (the upstream default).
    else()
        # Unknown arch — fall back to portable, same as
        # AGENTVFS_BLAKE3_SIMD=OFF.
        target_compile_definitions(cas_core PRIVATE
            BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2
            BLAKE3_USE_NEON=0)
    endif()
else()
    target_compile_definitions(cas_core PRIVATE
        BLAKE3_NO_AVX512 BLAKE3_NO_AVX2 BLAKE3_NO_SSE41 BLAKE3_NO_SSE2
        BLAKE3_USE_NEON=0)
endif()

if(NOT MSVC)
    target_compile_options(cas_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
```

### Key invariants

- The `-msse2 / -msse4.1 / -mavx2 / -mavx512f / -mavx512vl` flags (and the MSVC `/arch:` flags) are scoped to the individual BLAKE3 SIMD `.c` files via `set_source_files_properties`. No other `cas_core` source file gains a wider instruction-set baseline — the rest of the library still compiles against the toolchain's default ISA.
- `BLAKE3_USE_NEON` is left **unset** on aarch64. The upstream default in `blake3_impl.h` is "NEON on, on aarch64". The previous code had to force it to `0` because `blake3_neon.c` wasn't being compiled. Now that it is, the upstream default is correct.
- `AGENTVFS_BLAKE3_SIMD=OFF` produces a build byte-identical to today's: same compile inputs (`blake3.c`, `blake3_dispatch.c`, `blake3_portable.c`), same defines.

## Verification

### CI matrix coverage (no new jobs needed)

All three existing CI jobs at `.github/workflows/ci.yml` will exercise the new paths automatically because `AGENTVFS_BLAKE3_SIMD` defaults to ON:

- `linux` (ubuntu-latest, x86_64): exercises AVX2 / AVX512 / SSE4.1 / SSE2 dispatch.
- `macos` (macos-latest, arm64 Apple Silicon): exercises the NEON path.
- `windows` (windows-latest, MSVC x86_64): exercises the MSVC `/arch:` path.

If any of these CI jobs fails to build or any existing `cas_test_*` test fails on any platform after this change, the change is wrong — every checkpoint, every CAS read, every tree serialization goes through `blake3_hasher_*`, so a regression in hash output would surface in the first integration test.

### One new unit test: `cas_test_blake3_simd`

`tests/cas/test_blake3_simd.cpp` (source file, following the existing `test_working_tree.cpp` / `test_object_store.cpp` naming convention; the target name is `cas_test_blake3_simd`) does two things:

1. **Known-answer vector.** Hash the empty input via `blake3_hasher_init` + `blake3_hasher_finalize` and assert the 32-byte digest equals the BLAKE3 canonical empty-input vector `af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262`. This vector is published in the BLAKE3 specification and is reproducible with `b3sum`. Runs on every CI platform without conditional compilation; serves as basic API-correctness coverage.

2. **SIMD entry-point presence (build-time check).** Declare the per-arch SIMD entry point in the test source via `extern "C"` and take its address into a `static void* const` at static-init time. If the SIMD `.c` files aren't compiled into `cas_core`, the symbol is unresolved and the test target fails to link, which is the regression signal we want — visible at build time, not runtime. Then assert at runtime that the pointer is non-null as a belt-and-suspenders check:
   - On `__x86_64__` / `_M_X64`: take address of `blake3_hash_many_avx2`.
   - On `__aarch64__` / `_M_ARM64`: take address of `blake3_hash_many_neon`.
   - On other arches: no SIMD symbol referenced; the runtime check prints a `[skip]` notice and exits 0.

   This is the only safeguard against a future CMake refactor silently dropping the SIMD `target_sources` entries and quietly falling back to portable.

The test target is gated on `AGENTVFS_BLAKE3_SIMD=ON` in `CMakeLists.txt` so that the rollback build (`-DAGENTVFS_BLAKE3_SIMD=OFF`) doesn't try to link against symbols that aren't there. Wire the new test alongside the other `cas_test_*` executables and add it to the test invocations in `.github/workflows/ci.yml` for the `linux`, `macos`, and `windows` jobs. The `windows-daemon` job is intentionally not modified — same MSVC toolchain as `windows`, so SIMD coverage is redundant.

### No new shell integration test

`test_cas_smoke.sh` already round-trips checkpoints, which means every hashed blob travels through the new code path on Linux. If the hash output changed, the smoke test would fail immediately. We do not need a separate integration test for this change.

## Risk & rollback

- **MSVC `/arch:AVX512` requirement.** Requires VS 2017 15.3+ and a current MSVC toolset. The `windows-latest` GitHub runner has a current Visual Studio so this is fine for CI, but mention the requirement in the Windows build section of `README.md` (one-line note in the existing Windows prerequisites block — do not add a new section).
- **Unknown future arch (e.g., RISC-V).** Hits the `else` branch and falls back to portable. Silently slower but correct. Matches today's behavior on every arch other than x86_64/aarch64.
- **Rollback.** `cmake -DAGENTVFS_BLAKE3_SIMD=OFF` restores the current build verbatim. The vendored SIMD `.c` files are inert when the option is off — they're not in `target_sources`. No code outside `CMakeLists.txt` references them.

## What changes, file by file

```
agentvfs-public/
  CMakeLists.txt                        modify (block at lines 119-161)
  include/blake3/blake3_sse2.c          new (copied from agentvfs/)
  include/blake3/blake3_sse41.c         new (copied from agentvfs/)
  include/blake3/blake3_avx2.c          new (copied from agentvfs/)
  include/blake3/blake3_avx512.c        new (copied from agentvfs/)
  include/blake3/blake3_neon.c          new (copied from agentvfs/)
  tests/cas/test_blake3_simd.cpp        new
  .github/workflows/ci.yml              modify (add cas_test_blake3_simd to each job's test list)
  README.md                             modify (one-line MSVC version note in the Windows build section)
  CLAUDE.md                             modify (replace the "blake3 is vendored with all SIMD paths disabled..." paragraph with the new state — runtime-dispatched SIMD, AGENTVFS_BLAKE3_SIMD=OFF as the escape hatch)
```

No source file under `src/cas/` changes.
