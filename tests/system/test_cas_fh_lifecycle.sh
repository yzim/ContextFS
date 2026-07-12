#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-fh}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build/agentvfs"
HARNESS="$(pwd)/build/tests/cas_test_fh_lifecycle"
FUSE_PID=""

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$FUSE_PID" ]]; then
        kill "$FUSE_PID" 2>/dev/null || true
        wait "$FUSE_PID" 2>/dev/null || true
    fi
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
FUSE_PID=$!
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: mount not ready"; exit 1; }

"$HARNESS" "$MNT" "$SOCK"

# ── fd leak check: open/close lifecycle.txt 1000 times, then verify the
#    daemon's open-fd count did not grow. BlobView fds must be released on
#    close(), not retained. A small slack (2) tolerates background fds.
before=$(find "/proc/$FUSE_PID/fd" -mindepth 1 -maxdepth 1 | wc -l)
for _ in $(seq 1 1000); do
    exec {fd}<"$MNT/lifecycle.txt"
    exec {fd}<&-
done
sleep 1
after=$(find "/proc/$FUSE_PID/fd" -mindepth 1 -maxdepth 1 | wc -l)
[[ "$after" -le $((before + 2)) ]] || {
    echo "FAIL: daemon fd growth before=$before after=$after"
    exit 1
}

echo "PASS test_cas_fh_lifecycle"
