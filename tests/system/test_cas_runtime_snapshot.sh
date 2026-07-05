#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-runtime-snapshot}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
FIFO="$ROOT/commands.fifo"
BIN="$(pwd)/build/agentvfs"
CTL="$(pwd)/build/agentvfs-ctl"
RUN="$(pwd)/build/agentvfs-run"
# The fixture is a test executable, so CMake emits it under build/tests/
# (like every other tests/ target), not build/. The agentvfs* tools above
# are top-level targets, hence the split.
FIXTURE="$(pwd)/build/tests/agentvfs_runtime_counter_fixture"
[[ -x "$FIXTURE" ]] || FIXTURE="$(pwd)/build/agentvfs_runtime_counter_fixture"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 %2 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
mkfifo "$FIFO"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }

"$RUN" --sock "$SOCK" --branch main -- "$FIXTURE" "$FIFO" &

for _ in $(seq 1 50); do
    RUNTIMES="$("$CTL" --sock "$SOCK" runtime list || true)"
    if grep -q '"runtime_id"' <<<"$RUNTIMES"; then break; fi
    sleep 0.1
done
RUNTIME_ID="$(python3 - <<'PY' "$RUNTIMES"
import json, sys
data=json.loads(sys.argv[1])
print(data["runtimes"][0]["runtime_id"])
PY
)"
[[ -n "$RUNTIME_ID" ]] || { echo "FAIL: runtime id missing"; exit 1; }

{
    echo "inc"
    echo "write $MNT/counter.txt"
} > "$FIFO" &

for _ in $(seq 1 50); do
    [[ -f "$MNT/counter.txt" ]] && grep -q "counter=1" "$MNT/counter.txt" && break
    sleep 0.1
done
grep -q "counter=1" "$MNT/counter.txt" || { echo "FAIL: initial counter write"; exit 1; }

SNAPSHOT_OUT="$ROOT/snapshot.json"
"$CTL" --sock "$SOCK" --json runtime snapshot "$RUNTIME_ID" --boundary manual --timeout-ms 2000 >"$SNAPSHOT_OUT" &
SNAPSHOT_PID=$!

sleep 0.1
{
    echo "boundary manual"
} > "$FIFO" &

wait "$SNAPSHOT_PID"
SNAPSHOT="$(cat "$SNAPSHOT_OUT")"
grep -q '"ok":true' <<<"$SNAPSHOT" || { echo "FAIL: snapshot response $SNAPSHOT"; exit 1; }
UNION_ID="$(python3 - <<'PY' "$SNAPSHOT"
import json, sys
print(json.loads(sys.argv[1])["union_state_id"])
PY
)"
[[ "$UNION_ID" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: bad union id '$UNION_ID'"; exit 1; }

{
    echo "inc"
    echo "write $MNT/counter.txt"
} > "$FIFO" &
for _ in $(seq 1 50); do
    grep -q "counter=2" "$MNT/counter.txt" && break
    sleep 0.1
done
grep -q "counter=2" "$MNT/counter.txt" || { echo "FAIL: counter did not advance"; exit 1; }

RESTORE_OUT="$ROOT/restore.json"
"$CTL" --sock "$SOCK" --json runtime restore "$UNION_ID" >"$RESTORE_OUT" &
RESTORE_PID=$!

for _ in $(seq 1 50); do
    if [[ -s "$RESTORE_OUT" ]]; then break; fi
    sleep 0.1
done
wait "$RESTORE_PID"
RESTORE="$(cat "$RESTORE_OUT")"
grep -q '"ok":true' <<<"$RESTORE" || { echo "FAIL: restore response $RESTORE"; exit 1; }

{
    echo "write $MNT/counter.txt"
    echo "exit"
} > "$FIFO" &
for _ in $(seq 1 50); do
    grep -q "counter=1" "$MNT/counter.txt" && break
    sleep 0.1
done
grep -q "counter=1" "$MNT/counter.txt" || { echo "FAIL: restored counter did not return to 1"; cat "$MNT/counter.txt"; exit 1; }

echo "PASS test_cas_runtime_snapshot"
