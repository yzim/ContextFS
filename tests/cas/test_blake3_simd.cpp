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
#include <cstdlib>
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
// — input "", hash = af1349b9f5f9a1a6a0404dea36dcc9499bcb25c9adc112b7cc9a93cae41f3262
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

// SIMD entry-point presence check. We take the address of the per-arch
// SIMD entry point and write it through a volatile pointer so the
// compiler cannot optimize the reference away. If the translation unit
// (blake3_avx2.c / blake3_neon.c / ...) is not compiled into cas_core,
// the link step fails with an unresolved-symbol error — exactly the
// regression we want to catch.
//
// Skipped on architectures other than x86_64 / aarch64 — we only enable
// SIMD on those (see CMakeLists.txt).
static volatile void* g_simd_entry_volatile = nullptr;

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
extern "C" void blake3_hash_many_avx2(
    const uint8_t *const *inputs, size_t num_inputs, size_t blocks,
    const uint32_t key[8], uint64_t counter, bool increment_counter,
    uint8_t flags, uint8_t flags_start, uint8_t flags_end, uint8_t *out);
// The volatile store forces the compiler to emit an actual reference to
// blake3_hash_many_avx2 that the linker must resolve.
static void* compute_simd_entry() {
    void* p = reinterpret_cast<void*>(&blake3_hash_many_avx2);
    g_simd_entry_volatile = p;  // volatile store: not optimizable
    return p;
}
static void* const simd_entry_point = compute_simd_entry();
constexpr const char* simd_entry_name = "blake3_hash_many_avx2";
constexpr bool simd_expected = true;
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(_M_ARM64EC)
extern "C" void blake3_hash_many_neon(
    const uint8_t *const *inputs, size_t num_inputs, size_t blocks,
    const uint32_t key[8], uint64_t counter, bool increment_counter,
    uint8_t flags, uint8_t flags_start, uint8_t flags_end, uint8_t *out);
static void* compute_simd_entry() {
    void* p = reinterpret_cast<void*>(&blake3_hash_many_neon);
    g_simd_entry_volatile = p;  // volatile store: not optimizable
    return p;
}
static void* const simd_entry_point = compute_simd_entry();
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
