#!/bin/bash
# Multi-branch correctness test: 10 branches × 10 checkpoints × rollback × isolation.
# Requires root for cgroup-based branch isolation through FUSE.
set -euo pipefail

NUM_BRANCHES=10
NUM_CHECKPOINTS=10

ROOT="${1:-/tmp/agentvfs-multi-branch}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build/agentvfs"
CTL="$(pwd)/build/agentvfs-ctl"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

DAEMON_PID=""
CG_BASE="/sys/fs/cgroup/agentvfs-multi-$$"

start_daemon() {
    "$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
           --control-sock "$SOCK" -f -s &
    DAEMON_PID=$!
    for _ in $(seq 1 50); do
        if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then return 0; fi
        sleep 0.1
    done
    return 1
}

stop_daemon() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    rm -f "$SOCK"
}

cleanup() {
    stop_daemon
    for b in $(seq 1 $NUM_BRANCHES); do
        rmdir "$CG_BASE/br$b" 2>/dev/null || true
    done
    rmdir "$CG_BASE" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

# ── Preflight ─────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "SKIP test_cas_multi_branch: needs root for cgroup creation"
    exit 0
fi

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"

# Seed source with a file per branch so bootstrap has something.
for b in $(seq 1 $NUM_BRANCHES); do
    mkdir -p "$SRC/br${b}"
    echo "base-br${b}" > "$SRC/br${b}/data.txt"
done
echo "shared-v0" > "$SRC/shared.txt"

# Create cgroups — one per branch.
mkdir -p "$CG_BASE"
for b in $(seq 1 $NUM_BRANCHES); do
    mkdir "$CG_BASE/br$b" || { echo "SKIP: cgroup v2 unavailable"; exit 0; }
done

start_daemon || { echo "FAIL: daemon not ready"; exit 1; }

# ── 1. Create 10 branches ────────────────────────────────────────
echo "=== Creating $NUM_BRANCHES branches ==="
declare -a BRANCH_IDS
for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json branch create "br$b")"
    if grep -q '"ok":true' <<<"$RESP"; then
        BRANCH_IDS[$b]="$(sed -E 's/.*"branch_id":([0-9]+).*/\1/' <<<"$RESP")"
        pass "create br$b (id=${BRANCH_IDS[$b]})"
    else
        fail "create br$b: $RESP"
    fi
done

# Verify branch list.
RESP="$("$CTL" --sock "$SOCK" --json branch list)"
for b in $(seq 1 $NUM_BRANCHES); do
    grep -q "\"name\":\"br$b\"" <<<"$RESP" \
        && pass "br$b listed" || fail "br$b not listed"
done

# ── 2. Register cgroups ──────────────────────────────────────────
echo "=== Registering cgroups ==="
for b in $(seq 1 $NUM_BRANCHES); do
    "$CTL" --sock "$SOCK" session register \
        --cgroup "$CG_BASE/br$b" --id "$b" --branch "br$b" >/dev/null \
        && pass "register cg br$b" || fail "register cg br$b"
done

# ── 3. Per-branch: write unique files + 10 checkpoints ───────────
echo "=== Writing files and creating checkpoints ==="

# COMMITS[b,cp] stores commit hashes.  Bash doesn't have 2D arrays,
# so we use COMMITS_b_cp naming via eval.
for b in $(seq 1 $NUM_BRANCHES); do
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        # Enter cgroup, write branch-specific files, checkpoint.
        (
            echo $BASHPID > "$CG_BASE/br$b/cgroup.procs"

            # Each checkpoint mutates the branch's own data file and
            # creates a checkpoint-specific file.
            echo "br${b}-cp${cp}" > "$MNT/br${b}/data.txt"
            echo "br${b}-file${cp}" > "$MNT/br${b}/f${cp}.txt"
        )
        # Checkpoint via CLI (doesn't need to be in the cgroup).
        RESP="$("$CTL" --sock "$SOCK" --json checkpoint "br${b}-cp${cp}" --branch "br$b")"
        if grep -q '"ok":true' <<<"$RESP"; then
            HASH="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP")"
            eval "COMMIT_${b}_${cp}=\"$HASH\""
            pass "br$b cp$cp (${HASH:0:12}…)"
        else
            fail "br$b cp$cp resp=$RESP"
        fi
    done
done

# ── 4. Verify isolation: each branch sees only its own files ──────
echo "=== Verifying branch isolation ==="
for b in $(seq 1 $NUM_BRANCHES); do
    # Read from this branch's cgroup — should see its own latest data.
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    [[ "$GOT" == "br${b}-cp${NUM_CHECKPOINTS}" ]] \
        && pass "br$b sees own data" || fail "br$b data: expected 'br${b}-cp${NUM_CHECKPOINTS}', got '$GOT'"

    # Verify all checkpoint files exist on this branch.
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f${cp}.txt" || true)"
        [[ "$GOT" == "br${b}-file${cp}" ]] \
            && pass "br$b f${cp}.txt" || fail "br$b f${cp}.txt: '$GOT'"
    done

    # Verify this branch does NOT see another branch's checkpoint files.
    OTHER=$(( (b % NUM_BRANCHES) + 1 ))
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${OTHER}/data.txt 2>&1" || true)"
    # The other branch's dir exists from bootstrap, but its data should
    # still be the base version (not the other branch's mutations).
    [[ "$GOT" == "base-br${OTHER}" ]] \
        && pass "br$b doesn't see br$OTHER mutations" \
        || fail "br$b sees br$OTHER data: '$GOT'"
done

# ── 5. Rollback tests per branch ─────────────────────────────────
echo "=== Rollback tests ==="

# Roll each branch back to cp5, verify state.
for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json rollback "br${b}-cp5" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "rollback br$b → cp5" || fail "rollback br$b → cp5: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    [[ "$GOT" == "br${b}-cp5" ]] \
        && pass "br$b data == cp5" || fail "br$b data after rollback: '$GOT'"

    # Files f1..f5 should exist, f6..f10 should be gone.
    for cp in $(seq 1 5); do
        GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f${cp}.txt 2>/dev/null" || true)"
        [[ "$GOT" == "br${b}-file${cp}" ]] \
            && pass "br$b f${cp}.txt present" || fail "br$b f${cp}.txt missing after rollback to cp5"
    done
    for cp in $(seq 6 $NUM_CHECKPOINTS); do
        GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f${cp}.txt 2>/dev/null" || true)"
        [[ -z "$GOT" ]] \
            && pass "br$b f${cp}.txt gone" || fail "br$b f${cp}.txt should not exist after rollback to cp5 (got '$GOT')"
    done
done

# ── 6. Roll back to cp1, then forward to cp10 by hash ────────────
echo "=== Rollback to cp1, then forward to cp10 ==="
for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json rollback "br${b}-cp1" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "rollback br$b → cp1" || fail "rollback br$b → cp1: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    [[ "$GOT" == "br${b}-cp1" ]] \
        && pass "br$b data == cp1" || fail "br$b data after rollback to cp1: '$GOT'"

    # Only f1 should exist.
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f1.txt 2>/dev/null" || true)"
    [[ "$GOT" == "br${b}-file1" ]] \
        && pass "br$b f1.txt present" || fail "br$b f1.txt missing"
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f2.txt 2>/dev/null" || true)"
    [[ -z "$GOT" ]] \
        && pass "br$b f2.txt gone" || fail "br$b f2.txt should not exist (got '$GOT')"

    # Roll forward to cp10 by hash.
    HASH="$(eval echo "\$COMMIT_${b}_${NUM_CHECKPOINTS}")"
    RESP="$("$CTL" --sock "$SOCK" --json rollback "$HASH" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "forward br$b → cp10" || fail "forward br$b → cp10: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    [[ "$GOT" == "br${b}-cp${NUM_CHECKPOINTS}" ]] \
        && pass "br$b data restored to cp10" || fail "br$b data after forward: '$GOT'"
done

# ── 7. Cross-branch isolation after rollbacks ─────────────────────
echo "=== Cross-branch isolation after rollbacks ==="
# Roll odd branches to cp3, even branches stay at cp10.
for b in $(seq 1 2 $NUM_BRANCHES); do
    "$CTL" --sock "$SOCK" --json rollback "br${b}-cp3" --branch "br$b" >/dev/null
done

for b in $(seq 1 $NUM_BRANCHES); do
    if (( b % 2 == 1 )); then
        EXPECT="br${b}-cp3"
    else
        EXPECT="br${b}-cp${NUM_CHECKPOINTS}"
    fi
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    [[ "$GOT" == "$EXPECT" ]] \
        && pass "br$b at expected state" || fail "br$b: expected '$EXPECT', got '$GOT'"
done

# ── 8. Main branch is untouched ──────────────────────────────────
echo "=== Main branch unaffected ==="
GOT="$(cat "$MNT/shared.txt")"
[[ "$GOT" == "shared-v0" ]] \
    && pass "main shared.txt unchanged" || fail "main shared.txt: '$GOT'"

for b in $(seq 1 $NUM_BRANCHES); do
    GOT="$(cat "$MNT/br${b}/data.txt")"
    [[ "$GOT" == "base-br${b}" ]] \
        && pass "main sees base br$b" || fail "main br$b data: '$GOT'"
done

# ── 9. Cleanup: unregister + delete branches ─────────────────────
echo "=== Cleanup ==="
for b in $(seq 1 $NUM_BRANCHES); do
    "$CTL" --sock "$SOCK" session unregister --cgroup "$CG_BASE/br$b" >/dev/null \
        && pass "unregister br$b" || fail "unregister br$b"
    "$CTL" --sock "$SOCK" branch delete "br$b" >/dev/null \
        && pass "delete br$b" || fail "delete br$b"
done

RESP="$("$CTL" --sock "$SOCK" --json branch list)"
for b in $(seq 1 $NUM_BRANCHES); do
    grep -q "\"name\":\"br$b\"" <<<"$RESP" \
        && fail "br$b still listed" || pass "br$b removed"
done

# ── Summary ───────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]] && echo "PASS test_cas_multi_branch" || { echo "FAIL test_cas_multi_branch"; exit 1; }
