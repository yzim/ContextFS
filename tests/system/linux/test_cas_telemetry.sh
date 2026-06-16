#!/bin/bash
set -euo pipefail

if [[ ! -e /sys/kernel/btf/vmlinux ]]; then
    echo "SKIP test_cas_telemetry: no /sys/kernel/btf/vmlinux"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-telemetry}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
CG="/sys/fs/cgroup/agentvfs-test-$$"
BIN="$(pwd)/build/agentvfs"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rmdir "$CG" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "seed" > "$SRC/watched.txt"
echo "seed" > "$SRC/unwatched.txt"

if ! mkdir "$CG" 2>/dev/null; then
    echo "SKIP test_cas_telemetry: cannot create cgroup (need root/cgroup v2)"
    exit 0
fi

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done

STATUS="$(printf 'status\n' | nc -U -w 2 "$SOCK")"
if ! grep -q '"ebpf_available":true' <<<"$STATUS"; then
    echo "SKIP test_cas_telemetry: ebpf_available=false"
    exit 0
fi

REQ="session.register {\"cgroup_path\":\"$CG\",\"session_id\":42,\"telemetry_verbosity\":1}"
RESP="$(printf '%s\n' "$REQ" | nc -U -w 2 "$SOCK")"
grep -q '"ok":true' <<<"$RESP" || { echo "FAIL: register $RESP"; exit 1; }

POLICY='policy.install {"rules":[{"path_pattern":"watched.txt","soft_watch":"read"}]}'
RESP="$(printf '%s\n' "$POLICY" | nc -U -w 2 "$SOCK")"
grep -q '"ok":true' <<<"$RESP" || { echo "FAIL: policy.install $RESP"; exit 1; }

echo $$ > "$CG/cgroup.procs"
cat "$MNT/watched.txt" > /dev/null
cat "$MNT/unwatched.txt" > /dev/null
sleep 1

NDJSON_DAEMON="$(ls "$STORE/telemetry"/*.ndjson | head -n1)"
grep -q '"verdict":"soft_watch"' "$NDJSON_DAEMON" \
    || { echo "FAIL: no soft_watch events"; exit 1; }
grep -q '"op":"read"' "$NDJSON_DAEMON" \
    || { echo "FAIL: no read events"; exit 1; }

echo "PASS test_cas_telemetry"
