#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"
PRELOAD_LIB="${CAS_PRELOAD_LIB:-$(pwd)/build/libcas_preload.so}"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_ldpreload: $BIN is not built"
    exit 0
fi

if [[ ! -f "$PRELOAD_LIB" ]]; then
    echo "SKIP test_cas_ldpreload: $PRELOAD_LIB is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q "ldpreload" <<<"$HELP"; then
    echo "SKIP test_cas_ldpreload: pending runtime --telemetry ldpreload support"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-ldpreload}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
PRELOAD_SOCK="$ROOT/cas_preload.sock"
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
       --control-sock "$SOCK" --telemetry=ldpreload \
       --telemetry-ldpreload-socket="$PRELOAD_SOCK" -f -s &
DAEMON_PID="$!"

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

if ! mountpoint -q "$MNT"; then
    echo "FAIL test_cas_ldpreload: mount did not become ready"
    exit 1
fi

CAS_PRELOAD_SOCKET="$PRELOAD_SOCK" LD_PRELOAD="$PRELOAD_LIB" \
    bash -c 'cat "$1/read.txt" >/dev/null && echo write > "$1/write.txt"' \
    _ "$MNT"
sleep 1

shopt -s nullglob
telemetry=("$STORE"/telemetry/*.ndjson)
if [[ "${#telemetry[@]}" -eq 0 ]]; then
    echo "FAIL test_cas_ldpreload: no telemetry files found"
    exit 1
fi

grep -h -q '"backend":"ldpreload"' "${telemetry[@]}" \
    || { echo "FAIL test_cas_ldpreload: no ldpreload events"; exit 1; }

echo "PASS test_cas_ldpreload"
