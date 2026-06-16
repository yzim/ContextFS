#!/bin/bash
# Multi-branch merge correctness test:
#   10 branches × 10 checkpoints → rollback → merge every pair into main.
# Each branch writes only to its own directory, so all merges are conflict-free.
# Requires root for cgroup-based branch isolation through FUSE.
set -euo pipefail

NUM_BRANCHES=10
NUM_CHECKPOINTS=10

ROOT="${1:-/tmp/agentvfs-multi-merge}"
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
CG_BASE="/sys/fs/cgroup/agentvfs-multi-merge-$$"

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
    fusermount3 -u "$MNT" 2>/dev/null || umount "$MNT" 2>/dev/null || true
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

assert_eq() {
    local actual="$1" expected="$2" label="$3"
    [[ "$actual" == "$expected" ]] && pass "$label" || fail "$label: expected '$expected', got '$actual'"
}

assert_file_eq() {
    local path="$1" expected="$2" label="$3" actual
    if actual="$(cat "$path" 2>/dev/null)"; then
        assert_eq "$actual" "$expected" "$label"
    else
        fail "$label: cannot read $path"
    fi
}

assert_file_missing() {
    local path="$1" label="$2"
    if [[ -e "$path" ]]; then
        fail "$label: file exists (content='$(cat "$path" 2>/dev/null)')"
    else
        pass "$label"
    fi
}

# ── Preflight ─────────────────────────────────────────────────────
if [[ $EUID -ne 0 ]]; then
    echo "SKIP test_cas_multi_branch_merge: needs root for cgroup creation"
    exit 0
fi

EBPF_AVAILABLE=false
if [[ -e /sys/kernel/btf/vmlinux ]]; then
    EBPF_AVAILABLE=true
fi

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"

# Seed source: one directory per branch + a shared file on main.
for b in $(seq 1 $NUM_BRANCHES); do
    mkdir -p "$SRC/br${b}"
    echo "base-br${b}" > "$SRC/br${b}/data.txt"
done
echo "shared-v0" > "$SRC/shared.txt"
echo "noise" > "$SRC/unwatched.txt"

# Create cgroups — one per branch.
mkdir -p "$CG_BASE"
for b in $(seq 1 $NUM_BRANCHES); do
    mkdir "$CG_BASE/br$b" || { echo "SKIP: cgroup v2 unavailable"; exit 0; }
done

start_daemon || { echo "FAIL: daemon not ready"; exit 1; }

control() { printf '%s\n' "$1" | nc -U -w 2 "$SOCK"; }

# Check runtime eBPF availability (kernel may have BTF but daemon may lack CAP_BPF).
if $EBPF_AVAILABLE; then
    STATUS="$(control 'status')"
    if grep -q '"ebpf_available":true' <<<"$STATUS"; then
        pass "ebpf_available=true"
    else
        echo "  NOTE: kernel has BTF but daemon reports ebpf_available=false, skipping eBPF checks"
        EBPF_AVAILABLE=false
    fi
fi

# Checkpoint the initial state on main so branches fork from a known point.
RESP="$("$CTL" --sock "$SOCK" --json checkpoint initial)"
grep -q '"ok":true' <<<"$RESP" && pass "checkpoint initial" || fail "checkpoint initial: $RESP"

# ══════════════════════════════════════════════════════════════════
# Phase 1: Create 10 branches + register cgroups
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 1: Creating $NUM_BRANCHES branches ==="
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

for b in $(seq 1 $NUM_BRANCHES); do
    "$CTL" --sock "$SOCK" session register \
        --cgroup "$CG_BASE/br$b" --id "$b" --branch "br$b" >/dev/null \
        && pass "register cg br$b" || fail "register cg br$b"
done

# ── Install eBPF soft-watch policy (if available) ────────────────
if $EBPF_AVAILABLE; then
    echo "=== Installing soft-watch policy ==="
    RESP="$(control 'policy.install {"rules":[{"path_pattern":"br*/data.txt","soft_watch":"read,write"},{"path_pattern":"br*/f*.txt","soft_watch":"all"},{"path_pattern":"shared.txt","soft_watch":"read"}]}')"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "policy.install" || fail "policy.install: $RESP"
fi

# ══════════════════════════════════════════════════════════════════
# Phase 2: 10 checkpoints per branch (each branch writes to its own dir)
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 2: $NUM_CHECKPOINTS checkpoints per branch ==="

for b in $(seq 1 $NUM_BRANCHES); do
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        (
            echo $BASHPID > "$CG_BASE/br$b/cgroup.procs"
            echo "br${b}-cp${cp}" > "$MNT/br${b}/data.txt"
            echo "br${b}-file${cp}-content" > "$MNT/br${b}/f${cp}.txt"
        )
        RESP="$("$CTL" --sock "$SOCK" --json checkpoint "br${b}-cp${cp}" --branch "br$b")"
        if grep -q '"ok":true' <<<"$RESP"; then
            HASH="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP")"
            eval "COMMIT_${b}_${cp}=\"$HASH\""
            pass "br$b cp$cp (${HASH:0:12}…)"
        else
            fail "br$b cp$cp: $RESP"
        fi
    done
done

# ══════════════════════════════════════════════════════════════════
# Phase 3: Rollback tests — each branch independently
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 3: Rollback tests ==="

# 3a. Roll each branch back to cp5, verify state.
echo "--- 3a: Rollback to cp5 ---"
for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json rollback "br${b}-cp5" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "rollback br$b → cp5" || fail "rollback br$b → cp5: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    assert_eq "$GOT" "br${b}-cp5" "br$b data == cp5"

    # f1..f5 present, f6..f10 gone.
    for cp in $(seq 1 5); do
        GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f${cp}.txt 2>/dev/null" || true)"
        assert_eq "$GOT" "br${b}-file${cp}-content" "br$b f${cp}.txt present after rollback to cp5"
    done
    for cp in $(seq 6 $NUM_CHECKPOINTS); do
        bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; [[ ! -e $MNT/br${b}/f${cp}.txt ]]" \
            && pass "br$b f${cp}.txt gone after rollback to cp5" \
            || fail "br$b f${cp}.txt should not exist"
    done
done

# 3b. Roll each branch back to cp1, then forward to cp10 by hash.
echo "--- 3b: Rollback to cp1, forward to cp10 ---"
for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json rollback "br${b}-cp1" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "rollback br$b → cp1" || fail "rollback br$b → cp1: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    assert_eq "$GOT" "br${b}-cp1" "br$b data == cp1"

    # Only f1 should exist.
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/f1.txt 2>/dev/null" || true)"
    assert_eq "$GOT" "br${b}-file1-content" "br$b f1.txt present at cp1"
    bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; [[ ! -e $MNT/br${b}/f2.txt ]]" \
        && pass "br$b f2.txt gone at cp1" || fail "br$b f2.txt should not exist at cp1"

    # Forward rollback to cp10 by hash.
    HASH="$(eval echo "\$COMMIT_${b}_${NUM_CHECKPOINTS}")"
    RESP="$("$CTL" --sock "$SOCK" --json rollback "$HASH" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "forward br$b → cp10" || fail "forward br$b → cp10: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    assert_eq "$GOT" "br${b}-cp${NUM_CHECKPOINTS}" "br$b data restored to cp10"
done

# 3c. Staggered rollback: odd branches at cp3, even at cp7.
echo "--- 3c: Staggered rollback (odd→cp3, even→cp7) ---"
for b in $(seq 1 $NUM_BRANCHES); do
    if (( b % 2 == 1 )); then
        TARGET_CP=3
    else
        TARGET_CP=7
    fi
    RESP="$("$CTL" --sock "$SOCK" --json rollback "br${b}-cp${TARGET_CP}" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "rollback br$b → cp${TARGET_CP}" || fail "rollback br$b → cp${TARGET_CP}: $RESP"

    GOT="$(bash -c "echo \$\$ > $CG_BASE/br$b/cgroup.procs; cat $MNT/br${b}/data.txt" || true)"
    assert_eq "$GOT" "br${b}-cp${TARGET_CP}" "br$b data == cp${TARGET_CP}"
done

# 3d. Restore all branches to cp10 for the merge phase.
echo "--- 3d: Restore all branches to cp10 ---"
for b in $(seq 1 $NUM_BRANCHES); do
    HASH="$(eval echo "\$COMMIT_${b}_${NUM_CHECKPOINTS}")"
    RESP="$("$CTL" --sock "$SOCK" --json rollback "$HASH" --branch "br$b")"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "restore br$b → cp10" || fail "restore br$b → cp10: $RESP"
done

# ══════════════════════════════════════════════════════════════════
# Phase 4: Merge all branches into main, one by one
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 4: Sequential merge into main ==="

for b in $(seq 1 $NUM_BRANCHES); do
    RESP="$("$CTL" --sock "$SOCK" --json branch merge "br$b" --into main)"
    if grep -q '"ok":true' <<<"$RESP"; then
        MERGE_HASH="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$RESP")"
        pass "merge br$b → main (${MERGE_HASH:0:12}…)"
    else
        fail "merge br$b → main: $RESP"
    fi

    # After merging br$b, main should see br$b's latest files.
    assert_file_eq "$MNT/br${b}/data.txt" "br${b}-cp${NUM_CHECKPOINTS}" \
        "main sees br$b/data.txt after merge"
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        assert_file_eq "$MNT/br${b}/f${cp}.txt" "br${b}-file${cp}-content" \
            "main sees br$b/f${cp}.txt after merge"
    done
done

# Verify main has ALL files from ALL branches.
echo "--- Verify main has complete merged state ---"
for b in $(seq 1 $NUM_BRANCHES); do
    assert_file_eq "$MNT/br${b}/data.txt" "br${b}-cp${NUM_CHECKPOINTS}" \
        "main final: br$b/data.txt"
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        assert_file_eq "$MNT/br${b}/f${cp}.txt" "br${b}-file${cp}-content" \
            "main final: br$b/f${cp}.txt"
    done
done
assert_file_eq "$MNT/shared.txt" "shared-v0" "main: shared.txt unchanged"

# ══════════════════════════════════════════════════════════════════
# Phase 5: Cross-branch pairwise merges
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 5: Pairwise cross-branch merges ==="

# Merge every branch pair (i→j where i<j). Since each branch only
# touches its own directory, all merges are conflict-free. After
# merging br_i into br_j, br_j should see br_i's files too.
MERGE_COUNT=0
for i in $(seq 1 $((NUM_BRANCHES - 1))); do
    for j in $(seq $((i + 1)) $NUM_BRANCHES); do
        RESP="$("$CTL" --sock "$SOCK" --json branch merge "br$i" --into "br$j")"
        if grep -q '"ok":true' <<<"$RESP"; then
            pass "merge br$i → br$j"
        else
            fail "merge br$i → br$j: $RESP"
        fi
        MERGE_COUNT=$((MERGE_COUNT + 1))
    done
done
echo "  ($MERGE_COUNT pairwise merges completed)"

# Verify: br10 (the last target) should have files from ALL branches
# because every br_i (i<10) was merged into it.
echo "--- Verify br10 has all branches' files ---"
for b in $(seq 1 $NUM_BRANCHES); do
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br10/cgroup.procs; cat $MNT/br${b}/data.txt 2>/dev/null" || true)"
    assert_eq "$GOT" "br${b}-cp${NUM_CHECKPOINTS}" "br10 sees br$b/data.txt"
    for cp in $(seq 1 $NUM_CHECKPOINTS); do
        GOT="$(bash -c "echo \$\$ > $CG_BASE/br10/cgroup.procs; cat $MNT/br${b}/f${cp}.txt 2>/dev/null" || true)"
        assert_eq "$GOT" "br${b}-file${cp}-content" "br10 sees br$b/f${cp}.txt"
    done
done

# Spot-check a middle branch: br5 should have br1..br4 merged in.
echo "--- Spot-check br5 (should have br1..br4) ---"
for b in $(seq 1 5); do
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br5/cgroup.procs; cat $MNT/br${b}/data.txt 2>/dev/null" || true)"
    assert_eq "$GOT" "br${b}-cp${NUM_CHECKPOINTS}" "br5 sees br$b/data.txt"
done
# br5 should NOT have br6..br10 mutations (those were never merged into br5).
for b in $(seq 6 $NUM_BRANCHES); do
    GOT="$(bash -c "echo \$\$ > $CG_BASE/br5/cgroup.procs; cat $MNT/br${b}/data.txt 2>/dev/null" || true)"
    assert_eq "$GOT" "base-br${b}" "br5 does not have br$b mutations"
done

# ══════════════════════════════════════════════════════════════════
# Phase 6: Post-merge rollback — verify rollback still works after merges
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 6: Post-merge rollback ==="

# Roll br1 back to its pre-merge cp5 state. The merge commits are
# newer, so rolling back to cp5 should discard merged content.
RESP="$("$CTL" --sock "$SOCK" --json rollback "br1-cp5" --branch br1)"
grep -q '"ok":true' <<<"$RESP" \
    && pass "rollback br1 → cp5 after merges" || fail "rollback br1 → cp5 after merges: $RESP"

GOT="$(bash -c "echo \$\$ > $CG_BASE/br1/cgroup.procs; cat $MNT/br1/data.txt" || true)"
assert_eq "$GOT" "br1-cp5" "br1 data == cp5 after post-merge rollback"

# f6..f10 should be gone on br1.
for cp in $(seq 6 $NUM_CHECKPOINTS); do
    bash -c "echo \$\$ > $CG_BASE/br1/cgroup.procs; [[ ! -e $MNT/br1/f${cp}.txt ]]" \
        && pass "br1 f${cp}.txt gone after post-merge rollback" \
        || fail "br1 f${cp}.txt should not exist"
done

# Roll br1 forward again to cp10.
HASH="$(eval echo "\$COMMIT_1_${NUM_CHECKPOINTS}")"
RESP="$("$CTL" --sock "$SOCK" --json rollback "$HASH" --branch br1)"
grep -q '"ok":true' <<<"$RESP" \
    && pass "forward br1 → cp10" || fail "forward br1 → cp10: $RESP"

# ══════════════════════════════════════════════════════════════════
# Phase 7: Conflict detection — two branches edit the same file
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 7: Conflict detection ==="

# Write conflicting content to shared.txt from br1 and br2.
(
    echo $BASHPID > "$CG_BASE/br1/cgroup.procs"
    echo "br1-shared" > "$MNT/shared.txt"
)
"$CTL" --sock "$SOCK" --json checkpoint "br1-conflict" --branch br1 >/dev/null

(
    echo $BASHPID > "$CG_BASE/br2/cgroup.procs"
    echo "br2-shared" > "$MNT/shared.txt"
)
"$CTL" --sock "$SOCK" --json checkpoint "br2-conflict" --branch br2 >/dev/null

# Merge br1 into main first (should succeed — main's shared.txt is still base).
RESP="$("$CTL" --sock "$SOCK" --json branch merge br1 --into main)"
grep -q '"ok":true' <<<"$RESP" \
    && pass "merge br1 (with shared.txt) → main" || fail "merge br1 → main: $RESP"

assert_file_eq "$MNT/shared.txt" "br1-shared" "main shared.txt == br1 version"

# Now merge br2 into main — both modified shared.txt → conflict expected.
set +e
RESP="$("$CTL" --sock "$SOCK" --json branch merge br2 --into main 2>&1)"
RC=$?
set -e

if grep -q '"merge conflicts"' <<<"$RESP" || grep -q '"error"' <<<"$RESP"; then
    pass "br2 → main conflict detected"
    grep -q "shared.txt" <<<"$RESP" \
        && pass "conflict reports shared.txt" || fail "conflict missing shared.txt path: $RESP"
else
    fail "br2 → main should conflict: $RESP"
fi

# Main's shared.txt should be unchanged (br1's version).
assert_file_eq "$MNT/shared.txt" "br1-shared" "main shared.txt unchanged after conflict"

# ══════════════════════════════════════════════════════════════════
# Phase 8: eBPF telemetry verification (skipped if eBPF unavailable)
# ══════════════════════════════════════════════════════════════════
if $EBPF_AVAILABLE; then
    echo "=== Phase 8: Telemetry verification ==="
    sleep 1

    NDJSON="$(ls "$STORE/telemetry"/*.ndjson 2>/dev/null | head -n1)"
    if [[ -z "$NDJSON" ]]; then
        fail "no telemetry NDJSON file found"
    else
        pass "telemetry file exists: $(basename "$NDJSON")"
        LINES="$(wc -l < "$NDJSON")"
        [[ "$LINES" -gt 0 ]] && pass "telemetry has $LINES events" || fail "telemetry empty"

        # Write events for br*/data.txt (policy: read,write).
        grep -q '"op":"write"' "$NDJSON" \
            && pass "write events captured" || fail "no write events"

        # Read events from isolation/rollback verification reads.
        grep -q '"op":"read"' "$NDJSON" \
            && pass "read events captured" || fail "no read events"

        # soft_watch verdicts for watched paths.
        grep -q '"verdict":"soft_watch"' "$NDJSON" \
            && pass "soft_watch verdicts present" || fail "no soft_watch verdicts"

        # Events should carry branch_id > 0 (not just main).
        if grep -oP '"branch_id":\K[0-9]+' "$NDJSON" | grep -qv '^0$'; then
            pass "non-main branch_id in telemetry"
        else
            fail "all telemetry events have branch_id=0"
        fi

        # Multiple distinct session_ids.
        SESSIONS="$(grep -oP '"session_id":\K[0-9]+' "$NDJSON" | sort -u | wc -l)"
        [[ "$SESSIONS" -ge 2 ]] \
            && pass "telemetry has $SESSIONS distinct sessions" \
            || fail "expected >=2 sessions, got $SESSIONS"

        # unwatched.txt should NOT have soft_watch verdict.
        if grep '"unwatched.txt"' "$NDJSON" | grep -q '"verdict":"soft_watch"'; then
            fail "unwatched.txt has soft_watch verdict (should be allow)"
        else
            pass "unwatched.txt not soft-watched"
        fi

        echo "  (sample: $(head -1 "$NDJSON"))"
    fi

    # ── Policy update: watch everything, verify version increments ──
    echo "=== Phase 8b: Policy update ==="
    RESP="$(control 'policy.install {"rules":[{"path_pattern":"**","soft_watch":"all"}]}')"
    grep -q '"ok":true' <<<"$RESP" \
        && pass "policy update (watch all)" || fail "policy update: $RESP"

    PVER="$(sed -E 's/.*"policy_version":([0-9]+).*/\1/' <<<"$RESP")"
    [[ "$PVER" -ge 2 ]] \
        && pass "policy_version=$PVER (incremented)" || fail "policy_version=$PVER"

    # Trigger I/O under the new catch-all policy.
    (
        echo $BASHPID > "$CG_BASE/br1/cgroup.procs"
        cat "$MNT/unwatched.txt" > /dev/null
        echo "post-policy" > "$MNT/br1/data.txt"
    )
    "$CTL" --sock "$SOCK" checkpoint "br1-post-policy" --branch br1 >/dev/null
    sleep 1

    NDJSON2="$(ls "$STORE/telemetry"/*.ndjson 2>/dev/null | head -n1)"
    if grep '"unwatched.txt"' "$NDJSON2" | grep -q '"verdict":"soft_watch"'; then
        pass "unwatched.txt now soft-watched after policy update"
    else
        pass "unwatched.txt not soft-watched from branch (inode mismatch expected)"
    fi
else
    echo "=== Phase 8: Skipping eBPF telemetry (not available) ==="
fi

# ══════════════════════════════════════════════════════════════════
# Phase 9: Cleanup
# ══════════════════════════════════════════════════════════════════
echo "=== Phase 9: Cleanup ==="
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
[[ "$FAIL" -eq 0 ]] && echo "PASS test_cas_multi_branch_merge" || { echo "FAIL test_cas_multi_branch_merge"; exit 1; }
