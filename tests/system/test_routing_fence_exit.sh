#!/bin/bash
set -euo pipefail

BUILD_DIR="${AGENTVFS_BUILD_DIR:-$(pwd)/build-ebpf}"
BIN="${AGENTVFS_ROUTING_FENCE_EXIT_BIN:-$BUILD_DIR/tests/cas_test_routing_fence_exit}"
REQUIRE_ROUTING_FENCE="${AGENTVFS_REQUIRE_ROUTING_FENCE:-0}"

skip_or_fail() {
    local reason="$1"
    if [[ "$REQUIRE_ROUTING_FENCE" == "1" ]]; then
        echo "FAIL test_routing_fence_exit: $reason"
        exit 1
    fi
    echo "SKIP test_routing_fence_exit: $reason"
    exit 0
}

if [[ $EUID -ne 0 ]]; then
    skip_or_fail "needs root"
fi
if [[ ! -x "$BIN" ]]; then
    skip_or_fail "missing $BIN (configure with AGENTVFS_EBPF=ON)"
fi
if ! command -v unshare >/dev/null 2>&1; then
    skip_or_fail "unshare is unavailable"
fi

echo "=== routing-fence exit: initial PID namespace ==="
set +e
"$BIN"
initial_status=$?
set -e
if [[ $initial_status -eq 77 ]]; then
    skip_or_fail "routing fence did not load and attach"
fi
if [[ $initial_status -ne 0 ]]; then
    exit "$initial_status"
fi

echo "=== routing-fence exit: nested PID namespace ==="
if ! unshare --pid --fork --mount-proc true 2>/dev/null; then
    skip_or_fail "nested PID namespace unavailable"
fi
set +e
unshare --pid --fork --mount-proc "$BIN"
nested_status=$?
set -e
if [[ $nested_status -eq 77 ]]; then
    skip_or_fail "routing fence did not load in a nested PID namespace"
fi
if [[ $nested_status -ne 0 ]]; then
    echo "FAIL test_routing_fence_exit: nested PID namespace test failed"
    exit "$nested_status"
fi

echo "PASS test_routing_fence_exit"
