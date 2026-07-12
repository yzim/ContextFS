#!/bin/bash
set -euo pipefail

# Regression test for duplicate vmlinux.h generation when the two BPF
# skeletons are requested in parallel by the agentvfs target.
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/agentvfs-ebpf-parallel.XXXXXX")"
BUILD_LOG="$BUILD_DIR/build.log"

cleanup() {
    rm -rf "$BUILD_DIR"
}
trap cleanup EXIT

cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G "Unix Makefiles" \
      -DCMAKE_BUILD_TYPE=Release \
      -DAGENTVFS_EBPF=ON

set +e
cmake --build "$BUILD_DIR" --target agentvfs -- -j8 2>&1 | tee "$BUILD_LOG"
build_status=${PIPESTATUS[0]}
set -e

generation_count="$(grep -c "Generating vmlinux.h" "$BUILD_LOG" || true)"
echo "vmlinux_generation_count=$generation_count"

if [[ "$build_status" -ne 0 ]]; then
    echo "FAIL test_ebpf_parallel_build: agentvfs build failed"
    exit "$build_status"
fi

if [[ "$generation_count" -ne 1 ]]; then
    echo "FAIL test_ebpf_parallel_build: expected one vmlinux.h generation, got $generation_count"
    exit 1
fi

echo "PASS test_ebpf_parallel_build"
