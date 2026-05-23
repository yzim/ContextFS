# Hardware-accelerated BLAKE3 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Drop the `BLAKE3_NO_*` defines and the forced `BLAKE3_USE_NEON=0` so that `blake3_dispatch.c`'s runtime CPU detection picks the best SIMD path (AVX2/AVX512/SSE4.1/SSE2 on x86_64, NEON on aarch64) on every consumer machine.

**Architecture:** Build-system change only — no callsite changes. Vendor the upstream BLAKE3 1.8.4 SIMD C source files into `include/blake3/`, wire them into `cas_core` with per-file `-msse2 / -msse4.1 / -mavx2 / -mavx512f -mavx512vl` flags (or MSVC `/arch:` equivalents) scoped to those individual translation units only, gated by a new `AGENTVFS_BLAKE3_SIMD` CMake option (default ON). C intrinsics only — no assembly, no `enable_language(ASM/ASM_MASM)`.

**Tech Stack:** CMake 3.18+, C/C++ toolchains (GCC/Clang on Linux/macOS, MSVC on Windows), vendored BLAKE3 1.8.4 reference C implementation.

**Reference spec:** `docs/superpowers/specs/2026-05-23-blake3-hwaccel-design.md`

**Source of SIMD files to vendor:** `/home/zzh/agentvfs/include/blake3/` (the user's local copy of the full upstream BLAKE3 1.8.4 distribution).

---

## File map

```
agentvfs-public/
  CMakeLists.txt                        modify (block at lines 119-161, plus new test target near line 464)
  include/blake3/blake3_sse2.c          new (copy from /home/zzh/agentvfs/include/blake3/)
  include/blake3/blake3_sse41.c         new (copy from /home/zzh/agentvfs/include/blake3/)
  include/blake3/blake3_avx2.c          new (copy from /home/zzh/agentvfs/include/blake3/)
  include/blake3/blake3_avx512.c        new (copy from /home/zzh/agentvfs/include/blake3/)
  include/blake3/blake3_neon.c          new (copy from /home/zzh/agentvfs/include/blake3/)
  tests/cas/test_blake3_simd.cpp        new
  .github/workflows/ci.yml              modify (add cas_test_blake3_simd to each job's test list)
  README.md                             modify (one-line MSVC version note in Windows build prerequisites)
  CLAUDE.md                             modify (replace the "blake3 is vendored with all SIMD paths disabled..." paragraph)
```

---

### Task 1: Vendor the SIMD source files

**Files:**
- Create: `include/blake3/blake3_sse2.c`
- Create: `include/blake3/blake3_sse41.c`
- Create: `include/blake3/blake3_avx2.c`
- Create: `include/blake3/blake3_avx512.c`
- Create: `include/blake3/blake3_neon.c`

These five files are copied verbatim from `/home/zzh/agentvfs/include/blake3/` (a checkout of the full upstream BLAKE3 1.8.4 distribution). No content modification.

- [ ] **Step 1: Confirm the source files exist where expected**

Run:
```bash
ls -1 /home/zzh/agentvfs/include/blake3/blake3_sse2.c \
      /home/zzh/agentvfs/include/blake3/blake3_sse41.c \
      /home/zzh/agentvfs/include/blake3/blake3_avx2.c \
      /home/zzh/agentvfs/include/blake3/blake3_avx512.c \
      /home/zzh/agentvfs/include/blake3/blake3_neon.c
```
Expected: all five paths print, no errors.

- [ ] **Step 2: Copy the five files into `include/blake3/`**

Run from the repo root:
```bash
cp /home/zzh/agentvfs/include/blake3/blake3_sse2.c   include/blake3/blake3_sse2.c
cp /home/zzh/agentvfs/include/blake3/blake3_sse41.c  include/blake3/blake3_sse41.c
cp /home/zzh/agentvfs/include/blake3/blake3_avx2.c   include/blake3/blake3_avx2.c
cp /home/zzh/agentvfs/include/blake3/blake3_avx512.c include/blake3/blake3_avx512.c
cp /home/zzh/agentvfs/include/blake3/blake3_neon.c   include/blake3/blake3_neon.c
```

- [ ] **Step 3: Verify the build is still clean with no CMake changes yet**

The new files are NOT yet referenced by `CMakeLists.txt`, so they should be inert.

Run:
```bash
cmake -B build -DAGENTVFS_EBPF=OFF
cmake --build build -j
```
Expected: build succeeds. The new `.c` files are not compiled (they're not in `CAS_CORE_PORTABLE_SOURCES` yet).

- [ ] **Step 4: Spot-check one of the SIMD files compiles in isolation**

This is just a sanity check that the vendored files are well-formed and the include path resolves. Run:
```bash
cc -c -O2 -msse2   -Iinclude -Iinclude/blake3 include/blake3/blake3_sse2.c   -o /tmp/blake3_sse2.o
cc -c -O2 -mavx2   -Iinclude -Iinclude/blake3 include/blake3/blake3_avx2.c   -o /tmp/blake3_avx2.o
```
Expected: both produce object files with no warnings or errors. (Skip on Windows — the next task wires them into CMake which handles MSVC correctly.)

- [ ] **Step 5: Commit**

```bash
git add include/blake3/blake3_sse2.c include/blake3/blake3_sse41.c \
        include/blake3/blake3_avx2.c include/blake3/blake3_avx512.c \
        include/blake3/blake3_neon.c
git commit -m "vendor: import BLAKE3 1.8.4 SIMD C sources (not yet wired)"
```

---

### Task 2: Write the failing test target

**Files:**
- Create: `tests/cas/test_blake3_simd.cpp`
- Modify: `CMakeLists.txt` (add the `cas_test_blake3_simd` target alongside existing `cas_test_*` targets near line 464-489)

Note: this task is the TDD RED step. The new test target compiles under the current portable-only build, the known-answer test passes (BLAKE3 produces the same hash regardless of SIMD), but the SIMD-presence test fails to link because the SIMD function symbols (`blake3_hash_many_avx2`, etc.) don't exist in `cas_core` yet. That link failure is the "test fails" signal. Task 3 makes it link.

- [ ] **Step 1: Write the test source file**

Create `tests/cas/test_blake3_simd.cpp`:

```cpp
// Verifies that hardware-accelerated BLAKE3 is wired into cas_core.
//
// Two assertions:
//
// (1) Known-answer test: hashing the empty input yields the BLAKE3
//     canonical empty-string digest published in the upstream spec.
//     This is correctness coverage that runs on every platform.
//
// (2) SIMD-presence test: takes the address of the per-arch SIMD entry
//     point declared by blake3_impl.h. If the entry point's translation
//     unit (blake3_avx2.c / blake3_neon.c / ...) is not compiled into
//     cas_core, the link step fails with an unresolved-symbol error,
//     which is exactly the regression we want to catch.

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "blake3.h"
// Intentionally NOT including blake3_impl.h — its SIMD function
// declarations are gated by BLAKE3_NO_* defines that cas_core may or
// may not set. Declaring the SIMD entry point ourselves via extern "C"
// below keeps the test source decoupled from cas_core's build options.

namespace {

// Canonical BLAKE3 empty-input hash from the upstream spec /
// https://github.com/BLAKE3-team/BLAKE3/blob/master/test_vectors/test_vectors.json
// — input "", hash =
constexpr std::array<uint8_t, BLAKE3_OUT_LEN> kEmptyInputHash = {
    0xaf, 0x13, 0x49, 0xb9, 0xf5, 0xf9, 0xa1, 0xa6,
    0xa0, 0x40, 0x4d, 0xea, 0x36, 0xdc, 0xc9, 0x49,
    0x9b, 0xcb, 0x25, 0xc9, 0xad, 0xc1, 0x12, 0xb7,
    0xcc, 0x9a, 0x93, 0xca, 0xe4, 0x1f, 0x32, 0x62,
};

void test_empty_input_known_answer() {
    blake3_hasher h;
    blake3_hasher_init(&h);
    std::array<uint8_t, BLAKE3_OUT_LEN> got{};
    blake3_hasher_finalize(&h, got.data(), got.size());
    if (got != kEmptyInputHash) {
        std::fprintf(stderr, "empty-input BLAKE3 mismatch\n");
        std::fprintf(stderr, "expected: ");
        for (auto b : kEmptyInputHash) std::fprintf(stderr, "%02x", b);
        std::fprintf(stderr, "\n     got: ");
        for (auto b : got)             std::fprintf(stderr, "%02x", b);
        std::fprintf(stderr, "\n");
        std::abort();
    }
}

// SIMD entry-point presence check. The pointer is initialized at static
// init time; if the underlying symbol isn't defined the program fails
// to link, which is the build-time regression signal we want.
//
// Skipped on architectures other than x86_64 / aarch64 — we only enable
// SIMD on those (see CMakeLists.txt). On other arches this translation
// unit still compiles, the pointer is left as nullptr, and the
// assertion below treats that as "SIMD not applicable here, ok".
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
extern "C" void blake3_hash_many_avx2(
    const uint8_t *const *inputs, size_t num_inputs, size_t blocks,
    const uint32_t key[8], uint64_t counter, bool increment_counter,
    uint8_t flags, uint8_t flags_start, uint8_t flags_end, uint8_t *out);
static void* const simd_entry_point =
    reinterpret_cast<void*>(&blake3_hash_many_avx2);
constexpr const char* simd_entry_name = "blake3_hash_many_avx2";
constexpr bool simd_expected = true;
#elif defined(__aarch64__) || defined(_M_ARM64)
extern "C" void blake3_hash_many_neon(
    const uint8_t *const *inputs, size_t num_inputs, size_t blocks,
    const uint32_t key[8], uint64_t counter, bool increment_counter,
    uint8_t flags, uint8_t flags_start, uint8_t flags_end, uint8_t *out);
static void* const simd_entry_point =
    reinterpret_cast<void*>(&blake3_hash_many_neon);
constexpr const char* simd_entry_name = "blake3_hash_many_neon";
constexpr bool simd_expected = true;
#else
static void* const simd_entry_point = nullptr;
constexpr const char* simd_entry_name = "n/a";
constexpr bool simd_expected = false;
#endif

void test_simd_entry_point_present() {
    if (simd_expected && simd_entry_point == nullptr) {
        std::fprintf(stderr, "expected SIMD entry point %s but got null\n",
                     simd_entry_name);
        std::abort();
    }
    if (!simd_expected) {
        std::fprintf(stderr,
                     "[skip] SIMD presence check (architecture has no SIMD path)\n");
    }
}

}  // namespace

int main() {
    test_empty_input_known_answer();
    test_simd_entry_point_present();
    std::puts("cas_test_blake3_simd: ok");
    return 0;
}
```

- [ ] **Step 2: Wire the test target into `CMakeLists.txt`**

Add this block to `CMakeLists.txt` immediately after the existing `cas_test_object_store` block (currently around line 475 — find the closing `endif()` of that `if(UNIX)` block and add the new target on the next line). The new target is NOT gated by `if(UNIX)` — it compiles on every platform including MSVC.

```cmake
add_executable(cas_test_blake3_simd tests/cas/test_blake3_simd.cpp)
target_link_libraries(cas_test_blake3_simd PRIVATE cas_core)
```

- [ ] **Step 3: Configure and build — expect link failure**

Run:
```bash
cmake -B build -DAGENTVFS_EBPF=OFF
cmake --build build -j --target cas_test_blake3_simd
```

Expected outcome on Linux x86_64 / macOS x86_64: **link failure** with an unresolved-symbol error for `blake3_hash_many_avx2`. This is the RED state.

Expected outcome on macOS arm64 / Linux aarch64: **link failure** with an unresolved-symbol error for `blake3_hash_many_neon`.

Expected outcome on unrelated arches (e.g., RISC-V): the test builds and runs (`simd_expected` is false), and the run prints `[skip]` then `ok`. That's fine — the test will become meaningful once that arch's SIMD path is added later.

If you see a different failure mode (compile error, missing header, etc.), fix it before continuing.

- [ ] **Step 4: Do NOT commit yet**

The test file and the CMake test target are staged on disk but not yet committed. Committing a deliberately-broken link state to the branch would just make Task 3's commit harder to read. Task 3 will produce a single green commit that combines the test addition with the SIMD enable.

Verify nothing is staged and the working tree shows the expected uncommitted changes:
```bash
git status
```
Expected: `tests/cas/test_blake3_simd.cpp` is untracked, `CMakeLists.txt` is modified, nothing is staged.

---

### Task 3: Enable SIMD in `cas_core` — make the test pass

**Files:**
- Modify: `CMakeLists.txt:119-161` (the `CAS_CORE_PORTABLE_SOURCES` block through the `target_compile_definitions(cas_core PRIVATE BLAKE3_NO_...)` block)

- [ ] **Step 1: Replace the existing block**

Open `CMakeLists.txt` and locate lines 119 through 161 (the block that starts with `set(CAS_CORE_PORTABLE_SOURCES` and ends after the `BLAKE3_USE_NEON=0)` line). Replace that entire block with:

```cmake
set(CAS_CORE_PORTABLE_SOURCES
    src/cas/object_store.cpp
    src/cas/working_tree.cpp
    src/cas/write_buffer.cpp
    src/cas/tree_serialize.cpp
    src/cas/checkpoint.cpp
    src/cas/refs.cpp
    src/cas/daemon.cpp
    src/cas/commit.cpp
    src/cas/branch_router.cpp
    src/cas/branch_merge.cpp
    src/cas/telemetry_event.cpp
    src/cas/telemetry_registry.cpp
    src/cas/control_protocol.cpp
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
            # MSVC has no dedicated /arch:SSE4.1 — /arch:AVX is the
            # narrowest flag that turns on SSE4.1 codegen.
            set_source_files_properties(include/blake3/blake3_sse2.c
                PROPERTIES COMPILE_OPTIONS "/arch:SSE2")
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

Note: the `CAS_CORE_PORTABLE_SOURCES` list at the top is unchanged from what's already in the file — copy the existing entries exactly. If `set(CAS_CORE_PORTABLE_SOURCES ...)` in your tree has different members than what's listed above (because the codebase has evolved since this plan was written), keep the in-tree list and only change the surrounding scaffolding (the new `option(...)`, the new `if(AGENTVFS_BLAKE3_SIMD) ... endif()` block, and the removal of the unconditional `target_compile_definitions(cas_core PRIVATE BLAKE3_NO_...)`).

- [ ] **Step 2: Reconfigure with a fresh build directory**

Tearing down and reconfiguring ensures the new `set_source_files_properties` calls are picked up:
```bash
rm -rf build
cmake -B build -DAGENTVFS_EBPF=OFF
```
Expected: configure succeeds. CMake prints no warnings about the SIMD source files.

- [ ] **Step 3: Build `cas_test_blake3_simd`**

```bash
cmake --build build -j --target cas_test_blake3_simd
```
Expected: builds cleanly. No unresolved-symbol errors.

- [ ] **Step 4: Run the test**

```bash
./build/cas_test_blake3_simd
```
Expected output:
```
cas_test_blake3_simd: ok
```
Expected exit code: 0.

If the empty-input known-answer assertion fires (`std::abort`), something is fundamentally wrong with the blake3 build — investigate before continuing.

If the SIMD-entry-point assertion fires on an x86_64 or aarch64 host, the SIMD `.c` files weren't actually wired in — re-check the CMake block in step 1.

- [ ] **Step 5: Build the rest of the project to confirm no collateral damage**

```bash
cmake --build build -j
```
Expected: all targets build cleanly, no new warnings.

- [ ] **Step 6: Run the existing test suite to confirm no behavioral regression**

```bash
./build/cas_test_working_tree
./build/cas_test_write_buffer
./build/cas_test_object_store
./build/cas_test_branch_context
./build/cas_test_branch_merge
./build/cas_test_branch_merge_commit
./build/cas_test_telemetry_event
./build/cas_test_telemetry_registry
./build/cas_test_blake3_simd
```
Expected: all eight programs print their `ok`/success line and exit 0. If any fail, something in the build went wrong — investigate.

- [ ] **Step 7: Gate the test target on `AGENTVFS_BLAKE3_SIMD`**

The test references `blake3_hash_many_avx2` (or `blake3_hash_many_neon`) via `extern "C"`. When `AGENTVFS_BLAKE3_SIMD=OFF`, those symbols aren't in `cas_core`, so the test would fail to link. The clean rollback path is to omit the test target entirely when SIMD is off.

Find the test target you added in Task 2 Step 2 (currently unconditional):

```cmake
add_executable(cas_test_blake3_simd tests/cas/test_blake3_simd.cpp)
target_link_libraries(cas_test_blake3_simd PRIVATE cas_core)
```

Wrap it in an `if(AGENTVFS_BLAKE3_SIMD) ... endif()` block:

```cmake
if(AGENTVFS_BLAKE3_SIMD)
    add_executable(cas_test_blake3_simd tests/cas/test_blake3_simd.cpp)
    target_link_libraries(cas_test_blake3_simd PRIVATE cas_core)
endif()
```

- [ ] **Step 8: Verify the rollback path produces a clean build with no test**

```bash
rm -rf build
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_BLAKE3_SIMD=OFF
cmake --build build -j
test ! -f build/cas_test_blake3_simd && echo "ok: test target omitted under SIMD=OFF"
```
Expected output: `ok: test target omitted under SIMD=OFF`. The full build (no `--target` filter) succeeds. The `cas_test_blake3_simd` executable is absent, as intended.

- [ ] **Step 9: Verify the default ON path still works after the gate**

```bash
rm -rf build
cmake -B build -DAGENTVFS_EBPF=OFF
cmake --build build -j --target cas_test_blake3_simd
./build/cas_test_blake3_simd
```
Expected: builds, runs, prints `cas_test_blake3_simd: ok`.

- [ ] **Step 10: Commit (rolls up Task 2 + Task 3 changes)**

```bash
git add tests/cas/test_blake3_simd.cpp CMakeLists.txt
git commit -m "build: enable runtime-dispatched BLAKE3 SIMD in cas_core

Adds AGENTVFS_BLAKE3_SIMD CMake option (default ON), wires the
vendored SIMD C sources into cas_core with per-file -msse/-mavx
flags scoped to those translation units only. Drops the
BLAKE3_NO_AVX512 / BLAKE3_NO_AVX2 / BLAKE3_NO_SSE41 / BLAKE3_NO_SSE2
defines and the forced BLAKE3_USE_NEON=0. Adds cas_test_blake3_simd
which verifies (a) BLAKE3 of the empty input matches the canonical
spec vector and (b) the per-arch SIMD entry point is linked into
cas_core. Gates the test target on AGENTVFS_BLAKE3_SIMD=ON so the
rollback build is clean."
```

---

### Task 4: Wire `cas_test_blake3_simd` into CI

**Files:**
- Modify: `.github/workflows/ci.yml`

There are three jobs in `ci.yml`: `linux`, `macos`, `windows`. Each has a `Unit tests` step that lists the `cas_test_*` programs to run. Add `cas_test_blake3_simd` to each.

- [ ] **Step 1: Add the test to the Linux job**

Find the `linux:` job's `Unit tests` step in `.github/workflows/ci.yml`. It contains a multi-line `run: |` block that invokes `./build/cas_test_*` programs. Add `./build/cas_test_blake3_simd` to that list, alphabetically positioned (so it goes between `cas_test_branch_persistence` and `cas_test_telemetry_event` — but if the existing order isn't strictly alphabetical, just append it before `cas_test_telemetry_event` so it's near the bottom but visible).

```yaml
      - name: Unit tests
        run: |
          ./build/cas_test_working_tree
          ./build/cas_test_write_buffer
          ./build/cas_test_object_store
          ./build/cas_test_branch_context
          ./build/cas_test_branch_merge
          ./build/cas_test_branch_merge_commit
          ./build/cas_test_fh_lifecycle /tmp /tmp/test.sock || true
          ./build/cas_test_branch_merge_daemon || true
          ./build/cas_test_branch_persistence || true
          ./build/cas_test_blake3_simd
          ./build/cas_test_telemetry_event
          ./build/cas_test_telemetry_registry
```

- [ ] **Step 2: Add the test to the macOS job**

Same edit pattern, inside the `macos:` job's `Unit tests` step:

```yaml
      - name: Unit tests
        run: |
          ./build/cas_test_working_tree
          ./build/cas_test_write_buffer
          ./build/cas_test_object_store
          ./build/cas_test_branch_context
          ./build/cas_test_branch_merge
          ./build/cas_test_branch_merge_commit
          ./build/cas_test_blake3_simd
          ./build/cas_test_telemetry_event
          ./build/cas_test_telemetry_registry
```

- [ ] **Step 3: Add the test to the Windows job**

The `windows:` job's `Unit tests` step (around lines 80-86 of `ci.yml`) runs a narrower set of `cas_test_*` programs using PowerShell-style paths (the GitHub Actions Windows runner defaults to PowerShell for `run: |` blocks). Append `cas_test_blake3_simd` using the same `.\build\Release\xxx.exe` backslash style as the existing entries:

```yaml
      - name: Unit tests
        run: |
          .\build\Release\cas_test_working_tree.exe
          .\build\Release\cas_test_write_buffer.exe
          .\build\Release\cas_test_branch_context.exe
          .\build\Release\cas_test_branch_merge.exe
          .\build\Release\cas_test_blake3_simd.exe
          .\build\Release\cas_test_telemetry_event.exe
```

- [ ] **Step 4: Do NOT modify the `windows-daemon` job**

The `windows-daemon:` job (around line 88+ of `ci.yml`) uses the same MSVC toolchain as `windows:` — adding `cas_test_blake3_simd` there would be redundant SIMD coverage and tangles a SIMD test with the WinFsp daemon's runtime requirements. Skip it intentionally.

- [ ] **Step 5: Validate the workflow YAML locally**

If `yamllint` is available:
```bash
yamllint .github/workflows/ci.yml
```
Expected: no errors. If `yamllint` is unavailable, skip — GitHub's parser will catch syntax errors on push.

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: run cas_test_blake3_simd on linux / macos / windows"
```

---

### Task 5: Doc updates

**Files:**
- Modify: `README.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add MSVC version note to `README.md`**

The MSVC note belongs in the **Build from source** section's Windows entry, NOT in the Quick-start section. Find the line that currently reads:

```
# Windows — requires WinFsp 2.0+ from https://winfsp.dev
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
```

(it's around line 106 of `README.md`). Extend the comment to one additional clause noting the MSVC version requirement. Result should look like:

```
# Windows — requires WinFsp 2.0+ from https://winfsp.dev,
# and MSVC v141 (Visual Studio 2017 15.3) or newer for /arch:AVX512
# codegen on the BLAKE3 SIMD path (current VS releases all qualify).
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
```

Do NOT create a new section. Do NOT modify the Quick-start Windows block (line ~26) — that block is for users running the prebuilt installer where the MSVC requirement doesn't apply.

- [ ] **Step 2: Update `CLAUDE.md`**

Find this paragraph in `CLAUDE.md` (under the "Cross-platform conventions" subsection):

> blake3 is vendored with all SIMD paths disabled. Don't re-enable AVX2/SSE/NEON — the `BLAKE3_NO_*` defines at `CMakeLists.txt:148-153` are load-bearing because only `blake3_portable.c` is compiled in.

Replace it with:

> blake3 is vendored with runtime-dispatched SIMD enabled by default (`AGENTVFS_BLAKE3_SIMD=ON`). The SIMD `.c` files (`blake3_sse2.c`, `blake3_sse41.c`, `blake3_avx2.c`, `blake3_avx512.c`, `blake3_neon.c`) are compiled with per-file `-msse2 / -msse4.1 / -mavx2 / -mavx512f -mavx512vl` (or MSVC `/arch:` equivalents) scoped to those translation units only — the rest of `cas_core` keeps its baseline ISA. `blake3_dispatch.c` picks the best path at runtime via CPUID/`getauxval`. To force the portable path (debugging, exotic toolchains), pass `-DAGENTVFS_BLAKE3_SIMD=OFF`. No assembly variants are compiled in — C intrinsics only, no `enable_language(ASM/ASM_MASM)`.

- [ ] **Step 3: Spot-check doc consistency**

Search the rest of `CLAUDE.md`, `README.md`, and any other doc for references to `BLAKE3_NO_*` or "BLAKE3 portable" or "SIMD disabled":
```bash
grep -rn "BLAKE3_NO_\|blake3.*portable\|SIMD.*disabled\|portable.*blake3" \
    README.md CLAUDE.md docs/ 2>/dev/null
```
Expected: only the CLAUDE.md paragraph you just edited should mention these, and it should describe the NEW state. Update any stragglers.

- [ ] **Step 4: Commit**

```bash
git add README.md CLAUDE.md
git commit -m "docs: BLAKE3 SIMD is now on by default"
```

---

## Self-review checklist (executor: skip — this is what the plan author did)

1. **Spec coverage** — every section of the spec maps to a task:
   - Goal / non-goals → all tasks combined
   - Files to vendor → Task 1
   - CMake changes (option, per-arch sources, per-file flags, rollback path) → Task 3
   - Verification (CI matrix, new unit test, known-answer + dispatch check, rollback) → Tasks 2, 3, 4
   - Risk & rollback → Task 3 step 7
   - Doc updates → Task 5
   - "What changes, file by file" table → File map at top of plan
2. **Placeholder scan** — no "TBD", "TODO", "implement later", or "similar to Task N". The CMake block and the test source are written out in full.
3. **Type consistency** — `AGENTVFS_BLAKE3_SIMD` option name is used identically in tasks 3 and 4. `cas_test_blake3_simd` target name and source path are consistent across tasks 2, 3, and 4. SIMD function names (`blake3_hash_many_avx2`, `blake3_hash_many_neon`) match `blake3_impl.h`'s declarations.
