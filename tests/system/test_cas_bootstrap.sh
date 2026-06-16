#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-bootstrap}"
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
mkdir -p "$SRC/a/b/c" "$MNT"
for i in $(seq 1 20); do
    printf "content-%d\n" "$i" > "$SRC/file_${i}.txt"
done
printf "deep\n" > "$SRC/a/b/c/deep.txt"
ln -s "file_1.txt" "$SRC/symlink"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

[[ "$(cat "$MNT/a/b/c/deep.txt")" == "deep" ]] || { echo "FAIL: deep lazy ingest"; exit 1; }

[[ "$(readlink "$MNT/symlink")" == "file_1.txt" ]] || { echo "FAIL: symlink readlink"; exit 1; }

if ls -la "$MNT" | grep -q "\.agentvfs-store"; then
    echo "FAIL: .agentvfs-store visible in readdir"; exit 1
fi
if [[ -e "$MNT/.agentvfs-store" ]]; then
    echo "FAIL: .agentvfs-store accessible via path"; exit 1
fi

for _ in $(seq 1 100); do
    STATUS="$(printf 'status\n' | nc -U -w 2 "$SOCK")"
    if grep -q '"bootstrap_pending":false' <<<"$STATUS"; then break; fi
    sleep 0.1
done
grep -q '"bootstrap_pending":false' <<<"$STATUS" || {
    echo "FAIL: background walker never finished: $STATUS"; exit 1; }

for i in $(seq 1 20); do
    [[ "$(cat "$MNT/file_${i}.txt")" == "content-${i}" ]] \
        || { echo "FAIL: read file_${i}.txt"; exit 1; }
done

echo "PASS test_cas_bootstrap"
