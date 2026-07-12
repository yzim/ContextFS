#!/bin/bash
set -euo pipefail

# Verifies strict routing under cgroup migration with the eBPF fence
# active: the exact scenario Phase 1 serves with per-request reads must
# still hold when the fence-gated pid cache is in use.
ROOT="${1:-/tmp/agentvfs-routing-fence}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build-ebpf/agentvfs"
CTL="$(pwd)/build-ebpf/agentvfs-ctl"
CG_BASE="/sys/fs/cgroup/agentvfs-fence-$$"
REQUIRE_ROUTING_FENCE="${AGENTVFS_REQUIRE_ROUTING_FENCE:-0}"

skip_or_fail() {
    local reason="$1"
    if [[ "$REQUIRE_ROUTING_FENCE" == "1" ]]; then
        echo "FAIL test_routing_fence: $reason"
        exit 1
    fi
    echo "SKIP test_routing_fence: $reason"
    exit 0
}

if [[ $EUID -ne 0 ]]; then
    skip_or_fail "needs root for cgroup creation and BPF"
fi
[[ -x "$BIN" && -x "$CTL" ]] || {
    skip_or_fail "build-ebpf binaries missing (AGENTVFS_EBPF=ON build)"
}

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rmdir "$CG_BASE/br1" "$CG_BASE/br2" "$CG_BASE" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
if ! mkdir "$CG_BASE" "$CG_BASE/br1" "$CG_BASE/br2"; then
    skip_or_fail "cgroup v2 is unavailable or not writable"
fi

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s 2>"$ROOT/daemon.err" &
for _ in $(seq 1 50); do
    [[ -S "$SOCK" ]] && mountpoint -q "$MNT" && break
    sleep 0.1
done
mountpoint -q "$MNT" || { echo "FAIL: mount not ready"; exit 1; }

grep -q "routing fence active" "$ROOT/daemon.err" || {
    if [[ "$REQUIRE_ROUTING_FENCE" == "1" ]]; then
        echo "FAIL test_routing_fence: fence did not load"
        cat "$ROOT/daemon.err"
        exit 1
    fi
    echo "SKIP test_routing_fence: fence did not load"
    cat "$ROOT/daemon.err"
    exit 0
}

"$CTL" --sock "$SOCK" branch create br1 >/dev/null
"$CTL" --sock "$SOCK" branch create br2 >/dev/null
"$CTL" --sock "$SOCK" session register --cgroup "$CG_BASE/br1" --id 1 --branch br1 >/dev/null
"$CTL" --sock "$SOCK" session register --cgroup "$CG_BASE/br2" --id 2 --branch br2 >/dev/null

for b in 1 2; do
    (
        echo $BASHPID > "$CG_BASE/br$b/cgroup.procs"
        printf "content-%s" "$b" > "$MNT/fence.txt"
    )
done

(
    echo $BASHPID > "$CG_BASE/br1/cgroup.procs"
    python3 - "$MNT/fence.txt" "$CG_BASE/br2/cgroup.procs" <<'PY'
import os, pathlib, sys
path, next_cgroup = sys.argv[1:]
# Warm the fence cache in br1, migrate, and require a strictly fresh route.
for _ in range(3):
    assert pathlib.Path(path).read_bytes() == b"content-1"
pathlib.Path(next_cgroup).write_text(str(os.getpid()), encoding="ascii")
assert pathlib.Path(path).read_bytes() == b"content-2"
PY
)

"$CTL" --sock "$SOCK" session unregister --cgroup "$CG_BASE/br1" >/dev/null
"$CTL" --sock "$SOCK" session unregister --cgroup "$CG_BASE/br2" >/dev/null

echo "PASS test_routing_fence"
