#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_bpftime: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q "bpftime" <<<"$HELP"; then
    echo "SKIP test_cas_bpftime: pending runtime --telemetry bpftime support"
    exit 0
fi

if ! command -v bpftime >/dev/null 2>&1; then
    echo "SKIP test_cas_bpftime: bpftime runtime command is unavailable"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-bpftime}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
DAEMON_PID=""

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "seed" > "$SRC/read.txt"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" --telemetry=bpftime -f -s &
DAEMON_PID="$!"

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

if ! mountpoint -q "$MNT"; then
    echo "FAIL test_cas_bpftime: mount did not become ready"
    exit 1
fi

cat "$MNT/read.txt" > /dev/null
echo "write" > "$MNT/write.txt"
mv "$MNT/write.txt" "$MNT/renamed.txt"
rm -f "$MNT/renamed.txt"
sleep 1

shopt -s nullglob
telemetry=("$STORE"/telemetry/*.ndjson)
if [[ "${#telemetry[@]}" -eq 0 ]]; then
    echo "FAIL test_cas_bpftime: no telemetry files found"
    exit 1
fi

grep -h -q '"backend":"bpftime"' "${telemetry[@]}" \
    || { echo "FAIL test_cas_bpftime: no bpftime events"; exit 1; }

echo "PASS test_cas_bpftime"
