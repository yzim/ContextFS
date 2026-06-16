#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_fanotify: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q "fanotify" <<<"$HELP"; then
    echo "SKIP test_cas_fanotify: pending runtime --telemetry fanotify support"
    exit 0
fi

if [[ "$(id -u)" -ne 0 ]]; then
    echo "SKIP test_cas_fanotify: fanotify backend requires root"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-fanotify}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "seed" > "$SRC/read.txt"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" --telemetry=fanotify -f -s &
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

if ! mountpoint -q "$MNT"; then
    echo "FAIL test_cas_fanotify: mount did not become ready"
    exit 1
fi

cat "$MNT/read.txt" > /dev/null
echo "write" > "$MNT/write.txt"
sleep 1

shopt -s nullglob
telemetry=("$STORE"/telemetry/*.ndjson)
if [[ "${#telemetry[@]}" -eq 0 ]]; then
    echo "FAIL test_cas_fanotify: no telemetry files found"
    exit 1
fi

grep -h -q '"backend":"fanotify"' "${telemetry[@]}" \
    || { echo "FAIL test_cas_fanotify: no fanotify events"; exit 1; }

echo "PASS test_cas_fanotify"
