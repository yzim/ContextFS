#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-branching}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
CG1="/sys/fs/cgroup/agentvfs-branch-A-$$"
CG2="/sys/fs/cgroup/agentvfs-branch-B-$$"
BIN="$(pwd)/build/agentvfs"
CTL="$(pwd)/build/agentvfs-ctl"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

DAEMON_PID=""

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
    rmdir "$CG1" 2>/dev/null || true
    rmdir "$CG2" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

if [[ $EUID -ne 0 ]]; then
    echo "SKIP test_cas_branching: needs root for cgroup creation"
    exit 0
fi

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "v0" > "$SRC/a.txt"

mkdir "$CG1" || { echo "SKIP test_cas_branching: cgroup v2 unavailable"; exit 0; }
mkdir "$CG2"

start_daemon || { echo "FAIL: control socket not ready"; exit 1; }

# ── 1. Branch list starts with just main ──────────────────────────
RESP="$("$CTL" --sock "$SOCK" --json branch list)"
grep -q '"name":"main"' <<<"$RESP" && pass "main branch present" || fail "main branch missing: $RESP"

# ── 2. Create two branches ────────────────────────────────────────
ID1="$("$CTL" --sock "$SOCK" branch create subagent-1)"
[[ "$ID1" =~ ^[0-9]+$ ]] && pass "create subagent-1 (id=$ID1)" || fail "create subagent-1: $ID1"

ID2="$("$CTL" --sock "$SOCK" branch create subagent-2)"
[[ "$ID2" =~ ^[0-9]+$ ]] && pass "create subagent-2 (id=$ID2)" || fail "create subagent-2: $ID2"

# ── 3. Branch list now has three ──────────────────────────────────
RESP="$("$CTL" --sock "$SOCK" --json branch list)"
grep -q '"name":"subagent-1"' <<<"$RESP" && pass "subagent-1 listed" || fail "subagent-1 not listed"
grep -q '"name":"subagent-2"' <<<"$RESP" && pass "subagent-2 listed" || fail "subagent-2 not listed"

# ── 4. Register cgroups to branches ───────────────────────────────
"$CTL" --sock "$SOCK" session register --cgroup "$CG1" --id 1 --branch subagent-1 >/dev/null \
    && pass "register cg1 → subagent-1" || fail "register cg1"
"$CTL" --sock "$SOCK" session register --cgroup "$CG2" --id 2 --branch subagent-2 >/dev/null \
    && pass "register cg2 → subagent-2" || fail "register cg2"

# ── 5. Write from cgroup 1 (uses unshare to enter the cgroup) ─────
# Move a subshell into cg1 and write
(
    echo $BASHPID > "$CG1/cgroup.procs"
    echo "agent-1-content" > "$MNT/a.txt"
    "$CTL" --sock "$SOCK" checkpoint cp1 --branch subagent-1 >/dev/null
) && pass "cg1 write + checkpoint" || fail "cg1 write + checkpoint"

# ── 6. Write from cgroup 2 ────────────────────────────────────────
(
    echo $BASHPID > "$CG2/cgroup.procs"
    echo "agent-2-content" > "$MNT/a.txt"
    "$CTL" --sock "$SOCK" checkpoint cp1 --branch subagent-2 >/dev/null
) && pass "cg2 write + checkpoint" || fail "cg2 write + checkpoint"

# ── 7. Read from each cgroup — verify isolation ───────────────────
# Use a fresh subshell for each read; it enters the cgroup then cats.
V1="$(bash -c "echo \$\$ > $CG1/cgroup.procs; cat $MNT/a.txt")"
[[ "$V1" == "agent-1-content" ]] && pass "cg1 reads its own content" || fail "cg1 read: '$V1'"

V2="$(bash -c "echo \$\$ > $CG2/cgroup.procs; cat $MNT/a.txt")"
[[ "$V2" == "agent-2-content" ]] && pass "cg2 reads its own content" || fail "cg2 read: '$V2'"

# ── 8. Unregistered read sees main (original) ─────────────────────
V0="$(cat "$MNT/a.txt")"
[[ "$V0" == "v0" ]] && pass "unregistered reads main" || fail "unregistered read: '$V0'"

# ── 9. Restart restores checkpointed branches ─────────────────────
stop_daemon
start_daemon || { echo "FAIL: control socket not ready after restart"; exit 1; }

RESP="$("$CTL" --sock "$SOCK" --json branch list)"
grep -q '"name":"subagent-1"' <<<"$RESP" && pass "subagent-1 restored after restart" || fail "subagent-1 missing after restart: $RESP"
grep -q '"name":"subagent-2"' <<<"$RESP" && pass "subagent-2 restored after restart" || fail "subagent-2 missing after restart: $RESP"

"$CTL" --sock "$SOCK" session register --cgroup "$CG1" --id 11 --branch subagent-1 >/dev/null \
    && pass "re-register cg1 after restart" || fail "re-register cg1 after restart"
"$CTL" --sock "$SOCK" session register --cgroup "$CG2" --id 12 --branch subagent-2 >/dev/null \
    && pass "re-register cg2 after restart" || fail "re-register cg2 after restart"

V1_RESTART="$(bash -c "echo \$\$ > $CG1/cgroup.procs; cat $MNT/a.txt")"
[[ "$V1_RESTART" == "agent-1-content" ]] && pass "cg1 reads checkpointed content after restart" || fail "cg1 restart read: '$V1_RESTART'"

V2_RESTART="$(bash -c "echo \$\$ > $CG2/cgroup.procs; cat $MNT/a.txt")"
[[ "$V2_RESTART" == "agent-2-content" ]] && pass "cg2 reads checkpointed content after restart" || fail "cg2 restart read: '$V2_RESTART'"

V0_RESTART="$(cat "$MNT/a.txt")"
[[ "$V0_RESTART" == "v0" ]] && pass "unregistered reads main after restart" || fail "unregistered restart read: '$V0_RESTART'"

# ── 10. Rollback subagent-1 ────────────────────────────────────────
# First checkpoint main so we have a commit to roll to later
"$CTL" --sock "$SOCK" checkpoint main-v0 >/dev/null

# Rollback subagent-1 to cp1 (should be no-op since we're at cp1 already, but tests the command path)
"$CTL" --sock "$SOCK" rollback cp1 --branch subagent-1 >/dev/null \
    && pass "rollback subagent-1 accepted" || fail "rollback subagent-1"

# ── 11. Unregister cg1 and delete subagent-1 ──────────────────────
"$CTL" --sock "$SOCK" session unregister --cgroup "$CG1" >/dev/null \
    && pass "unregister cg1" || fail "unregister cg1"

"$CTL" --sock "$SOCK" branch delete subagent-1 >/dev/null \
    && pass "delete subagent-1" || fail "delete subagent-1"

RESP="$("$CTL" --sock "$SOCK" --json branch list)"
grep -q '"name":"subagent-1"' <<<"$RESP" && fail "subagent-1 still listed" || pass "subagent-1 gone from list"

# ── 12. Cannot delete main ────────────────────────────────────────
set +e
"$CTL" --sock "$SOCK" branch delete main 2>/dev/null
RC=$?
set -e
[[ "$RC" -ne 0 ]] && pass "cannot delete main" || fail "delete main should fail"

# ── 13. Cannot delete branch with active sessions ─────────────────
set +e
"$CTL" --sock "$SOCK" branch delete subagent-2 2>/dev/null
RC=$?
set -e
[[ "$RC" -ne 0 ]] && pass "cannot delete active branch" || fail "delete active branch should fail"

# ── Summary ───────────────────────────────────────────────────────
echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]] && echo "PASS test_cas_branching" || { echo "FAIL test_cas_branching"; exit 1; }
