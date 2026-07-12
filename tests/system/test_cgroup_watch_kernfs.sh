#!/bin/bash
set -euo pipefail

BUILD_DIR="${AGENTVFS_BUILD_DIR:-$(pwd)/build}"
BIN="${AGENTVFS_CGROUP_WATCH_BIN:-$BUILD_DIR/tests/cas_test_cgroup_watch_kernfs}"
REQUIRE_CGROUP_WATCH="${AGENTVFS_REQUIRE_CGROUP_WATCH:-0}"

skip_or_fail() {
    local reason="$1"
    if [[ "$REQUIRE_CGROUP_WATCH" == "1" ]]; then
        echo "FAIL test_cgroup_watch_kernfs: $reason"
        exit 1
    fi
    echo "SKIP test_cgroup_watch_kernfs: $reason"
    exit 0
}

if [[ $EUID -ne 0 ]]; then
    skip_or_fail "needs root (cgroup mkdir/rmdir)"
fi
if [[ ! -x "$BIN" ]]; then
    skip_or_fail "missing $BIN"
fi
if [[ ! -d /sys/fs/cgroup ]] || ! grep -q cgroup2 /proc/mounts; then
    skip_or_fail "cgroup v2 not mounted"
fi

set +e
"$BIN"
status=$?
set -e
if [[ $status -eq 77 ]]; then
    skip_or_fail "environment could not run the kernfs check"
fi
exit "$status"
