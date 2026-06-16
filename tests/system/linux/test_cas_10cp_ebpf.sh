#!/bin/bash
set -euo pipefail

if [[ ! -e /sys/kernel/btf/vmlinux ]]; then
    echo "SKIP test_cas_10cp_ebpf: no /sys/kernel/btf/vmlinux"
    exit 0
fi

ROOT="${1:-/tmp/agentvfs-10cp-ebpf}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
CG="/sys/fs/cgroup/agentvfs-10cp-$$"
BIN="$(pwd)/build/agentvfs"

PASS=0
FAIL=0

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

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
echo "v0" > "$SRC/a.txt"
echo "base" > "$SRC/stable.txt"

if ! mkdir "$CG" 2>/dev/null; then
    echo "SKIP test_cas_10cp_ebpf: cannot create cgroup (need root/cgroup v2)"
    exit 0
fi

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: mount not ready"; exit 1; }

control() { printf '%s\n' "$1" | nc -U -w 2 "$SOCK"; }

# ── 0. eBPF setup ─────────────────────────────────────────────────
echo "=== eBPF setup ==="
STATUS="$(control 'status')"
if ! grep -q '"ebpf_available":true' <<<"$STATUS"; then
    echo "SKIP test_cas_10cp_ebpf: ebpf_available=false"
    exit 0
fi
pass "ebpf_available=true"

RESP="$(control "session.register {\"cgroup_path\":\"$CG\",\"session_id\":99,\"telemetry_verbosity\":1}")"
grep -q '"ok":true' <<<"$RESP" && pass "session.register" || { fail "session.register resp=$RESP"; }

echo $$ > "$CG/cgroup.procs"

RESP="$(control 'policy.install {"rules":[{"path_pattern":"**","soft_watch":"all"}]}')"
grep -q '"ok":true' <<<"$RESP" && pass "policy.install (watch all)" || { fail "policy.install resp=$RESP"; }

# ── 1. Bootstrap read ─────────────────────────────────────────────
echo "=== Bootstrap ==="
[[ "$(cat "$MNT/a.txt")" == "v0" ]] && pass "bootstrap a.txt" || fail "bootstrap a.txt"
[[ "$(cat "$MNT/stable.txt")" == "base" ]] && pass "bootstrap stable.txt" || fail "bootstrap stable.txt"

# ── 2. 10 checkpoints, each mutates a.txt and creates a new file ──
echo "=== 10 checkpoints ==="
declare -a COMMITS
for i in $(seq 1 10); do
    echo "v$i" > "$MNT/a.txt"
    echo "file-$i-content" > "$MNT/file_$i.txt"

    RESP="$(control "checkpoint cp$i")"
    if grep -q '"ok":true' <<<"$RESP"; then
        COMMITS[$i]="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP")"
        pass "checkpoint cp$i (${COMMITS[$i]:0:12}…)"
    else
        fail "checkpoint cp$i resp=$RESP"
    fi
done

# ── 3. Verify state after all 10 checkpoints ──────────────────────
echo "=== Post-checkpoint state ==="
[[ "$(cat "$MNT/a.txt")" == "v10" ]] && pass "a.txt == v10" || fail "a.txt == v10 (got $(cat "$MNT/a.txt"))"
[[ "$(cat "$MNT/stable.txt")" == "base" ]] && pass "stable.txt unchanged" || fail "stable.txt changed"
for i in $(seq 1 10); do
    [[ "$(cat "$MNT/file_$i.txt")" == "file-$i-content" ]] \
        && pass "file_$i.txt present" || fail "file_$i.txt missing or wrong"
done

# ── 4. Rollback by label to cp5 ───────────────────────────────────
echo "=== Rollback to cp5 (by label) ==="
RESP="$(control "rollback cp5")"
grep -q '"ok":true' <<<"$RESP" && pass "rollback cp5 accepted" || fail "rollback cp5 resp=$RESP"

[[ "$(cat "$MNT/a.txt")" == "v5" ]] && pass "a.txt == v5" || fail "a.txt after rollback cp5 (got $(cat "$MNT/a.txt"))"
[[ "$(cat "$MNT/stable.txt")" == "base" ]] && pass "stable.txt unchanged" || fail "stable.txt changed"
for i in $(seq 1 5); do
    [[ "$(cat "$MNT/file_$i.txt")" == "file-$i-content" ]] \
        && pass "file_$i.txt still present" || fail "file_$i.txt missing after rollback to cp5"
done
for i in $(seq 6 10); do
    [[ ! -e "$MNT/file_$i.txt" ]] \
        && pass "file_$i.txt gone after rollback" || fail "file_$i.txt should not exist after rollback to cp5"
done

# ── 5. Rollback by label to cp1 ───────────────────────────────────
echo "=== Rollback to cp1 (by label) ==="
RESP="$(control "rollback cp1")"
grep -q '"ok":true' <<<"$RESP" && pass "rollback cp1 accepted" || fail "rollback cp1 resp=$RESP"

[[ "$(cat "$MNT/a.txt")" == "v1" ]] && pass "a.txt == v1" || fail "a.txt after rollback cp1 (got $(cat "$MNT/a.txt"))"
[[ -e "$MNT/file_1.txt" ]] && pass "file_1.txt present" || fail "file_1.txt missing"
for i in $(seq 2 10); do
    [[ ! -e "$MNT/file_$i.txt" ]] \
        && pass "file_$i.txt gone" || fail "file_$i.txt should not exist after rollback to cp1"
done

# ── 6. Roll forward by hash to cp10 ───────────────────────────────
echo "=== Roll forward to cp10 (by hash) ==="
RESP="$(control "rollback ${COMMITS[10]}")"
grep -q '"ok":true' <<<"$RESP" && pass "rollback to cp10 hash accepted" || fail "rollback to cp10 hash resp=$RESP"

[[ "$(cat "$MNT/a.txt")" == "v10" ]] && pass "a.txt == v10" || fail "a.txt after forward rollback (got $(cat "$MNT/a.txt"))"
for i in $(seq 1 10); do
    [[ "$(cat "$MNT/file_$i.txt")" == "file-$i-content" ]] \
        && pass "file_$i.txt restored" || fail "file_$i.txt missing after forward rollback"
done

# ── 7. Rollback to cp3 by hash ────────────────────────────────────
echo "=== Rollback to cp3 (by hash) ==="
RESP="$(control "rollback ${COMMITS[3]}")"
grep -q '"ok":true' <<<"$RESP" && pass "rollback to cp3 hash accepted" || fail "rollback to cp3 hash resp=$RESP"

[[ "$(cat "$MNT/a.txt")" == "v3" ]] && pass "a.txt == v3" || fail "a.txt after rollback cp3 (got $(cat "$MNT/a.txt"))"

# ── 8. Mutate after rollback, checkpoint, verify new branch ───────
echo "=== Mutate after rollback ==="
echo "diverged" > "$MNT/a.txt"
echo "new-after-rollback" > "$MNT/new.txt"
RESP="$(control "checkpoint post-rollback")"
grep -q '"ok":true' <<<"$RESP" && pass "checkpoint after rollback" || fail "checkpoint after rollback resp=$RESP"

[[ "$(cat "$MNT/a.txt")" == "diverged" ]] && pass "a.txt == diverged" || fail "a.txt diverged (got $(cat "$MNT/a.txt"))"
[[ "$(cat "$MNT/new.txt")" == "new-after-rollback" ]] && pass "new.txt present" || fail "new.txt missing"

# ── 9. Telemetry verification ─────────────────────────────────────
echo "=== Telemetry ==="
sleep 1

NDJSON="$(ls "$STORE/telemetry"/*.ndjson 2>/dev/null | head -n1)"
if [[ -n "$NDJSON" ]]; then
    pass "telemetry file exists"
    LINES="$(wc -l < "$NDJSON")"
    [[ "$LINES" -gt 0 ]] && pass "telemetry has $LINES events" || fail "telemetry file is empty"
    grep -q '"op":"read"' "$NDJSON" && pass "read events captured" || fail "no read events"
    grep -q '"op":"write"' "$NDJSON" && pass "write events captured" || fail "no write events"
    grep -q '"verdict":"soft_watch"' "$NDJSON" && pass "soft_watch verdicts present" || fail "no soft_watch verdicts"
    echo "  (sample: $(head -1 "$NDJSON"))"
else
    fail "no telemetry NDJSON file found"
fi

# ── Summary ───────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]] && echo "PASS test_cas_10cp_ebpf" || { echo "FAIL test_cas_10cp_ebpf"; exit 1; }
