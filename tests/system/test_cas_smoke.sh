#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-smoke}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build/agentvfs"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "hello" > "$SRC/a.txt"
mkdir -p "$SRC/dir"
echo "world" > "$SRC/dir/b.txt"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
FUSE_PID=$!

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: mount not ready"; exit 1; }

control() { printf '%s\n' "$1" | nc -U -w 2 "$SOCK"; }

[[ "$(cat "$MNT/a.txt")" == "hello" ]] || { echo "FAIL: bootstrap read a.txt"; exit 1; }
[[ "$(cat "$MNT/dir/b.txt")" == "world" ]] || { echo "FAIL: bootstrap read dir/b.txt"; exit 1; }

echo "v2" > "$MNT/a.txt"
[[ "$(cat "$MNT/a.txt")" == "v2" ]] || { echo "FAIL: post-write read"; exit 1; }

RESP1="$(control 'checkpoint first')"
grep -q '"ok":true' <<<"$RESP1" || { echo "FAIL: checkpoint #1 resp=$RESP1"; exit 1; }
COMMIT1="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP1")"

echo "v3" > "$MNT/a.txt"
RESP2="$(control 'checkpoint second')"
grep -q '"ok":true' <<<"$RESP2" || { echo "FAIL: checkpoint #2 resp=$RESP2"; exit 1; }
COMMIT2="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP2")"

# HEAD=COMMIT2, label "first" is ancestor-reachable → test label rollback.
RESP3="$(control 'rollback first')"
grep -q '"ok":true' <<<"$RESP3" || { echo "FAIL: rollback-by-label resp=$RESP3"; exit 1; }
[[ "$(cat "$MNT/a.txt")" == "v2" ]] || { echo "FAIL: post-label-rollback read"; exit 1; }

# HEAD=COMMIT1. Roll forward to COMMIT2 by hex (label-based forward lookup is
# intentionally unsupported per the spec).
RESP4="$(control 'rollback '"$COMMIT2")"
grep -q '"ok":true' <<<"$RESP4" || { echo "FAIL: rollback-by-hex resp=$RESP4"; exit 1; }
[[ "$(cat "$MNT/a.txt")" == "v3" ]] || { echo "FAIL: post-hex-rollback read"; exit 1; }

echo "PASS test_cas_smoke"
