#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-fh}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build/agentvfs"
HARNESS="$(pwd)/build/tests/cas_test_fh_lifecycle"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

"$HARNESS" "$MNT" "$SOCK"
