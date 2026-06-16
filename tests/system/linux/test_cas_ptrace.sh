#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_ptrace: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
if ! grep -q "ptrace" <<<"$HELP"; then
    echo "SKIP test_cas_ptrace: pending runtime --telemetry ptrace support"
    exit 0
fi

has_cap() {
    local bit="$1"
    local cap_eff
    cap_eff="$(awk '/^CapEff:/ {print $2}' /proc/self/status 2>/dev/null || true)"
    [[ -n "$cap_eff" ]] && (( (16#$cap_eff & (1 << bit)) != 0 ))
}

ptrace_runtime_usable() {
    [[ "$(uname -m)" == "x86_64" ]] || return 1
    if [[ -r /proc/sys/kernel/yama/ptrace_scope ]]; then
        local scope
        scope="$(cat /proc/sys/kernel/yama/ptrace_scope)"
        if [[ "$scope" != "0" ]] && ! has_cap 19; then
            return 1
        fi
    fi
    return 0
}

if ! ptrace_runtime_usable; then
    echo "SKIP test_cas_ptrace: requires x86_64 and ptrace attach permission"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-ptrace}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
TARGET_GO="$ROOT/target.go"
TARGET_DONE="$ROOT/target.done"
TARGET_STOP="$ROOT/target.stop"
TARGET_PID=""
DAEMON_PID=""

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
    fi
    touch "$TARGET_STOP" 2>/dev/null || true
    if [[ -n "$TARGET_PID" ]]; then
        kill "$TARGET_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "seed" > "$SRC/read.txt"

(
    while [[ ! -e "$TARGET_GO" ]]; do
        sleep 0.05
    done
    cat "$MNT/read.txt" > /dev/null
    echo "write" > "$MNT/write.txt"
    mv "$MNT/write.txt" "$MNT/renamed.txt"
    rm -f "$MNT/renamed.txt"
    touch "$TARGET_DONE"
    while [[ ! -e "$TARGET_STOP" ]]; do
        sleep 0.1
    done
) &
TARGET_PID="$!"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" --telemetry=ptrace \
       --telemetry-ptrace-pids="$TARGET_PID" -f -s &
DAEMON_PID="$!"

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

if ! mountpoint -q "$MNT"; then
    echo "FAIL test_cas_ptrace: mount did not become ready"
    exit 1
fi

touch "$TARGET_GO"
for _ in $(seq 1 50); do
    if [[ -e "$TARGET_DONE" ]]; then break; fi
    sleep 0.1
done
if [[ ! -e "$TARGET_DONE" ]]; then
    echo "FAIL test_cas_ptrace: target process did not complete activity"
    exit 1
fi
sleep 1

shopt -s nullglob
telemetry=("$STORE"/telemetry/*.ndjson)
if [[ "${#telemetry[@]}" -eq 0 ]]; then
    echo "FAIL test_cas_ptrace: no telemetry files found"
    exit 1
fi

grep -h -q '"backend":"ptrace"' "${telemetry[@]}" \
    || { echo "FAIL test_cas_ptrace: no ptrace events"; exit 1; }

echo "PASS test_cas_ptrace"
