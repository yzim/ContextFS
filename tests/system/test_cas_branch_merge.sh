#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-branch-merge}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
CG1="/sys/fs/cgroup/agentvfs-branch-merge-$$"
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
    rmdir "$CG1" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

assert_eq() {
    local actual="$1"
    local expected="$2"
    local label="$3"
    [[ "$actual" == "$expected" ]] && pass "$label" || fail "$label: '$actual'"
}

assert_file_eq() {
    local path="$1"
    local expected="$2"
    local label="$3"
    local actual
    if actual="$(cat "$path" 2>/dev/null)"; then
        assert_eq "$actual" "$expected" "$label"
    else
        fail "$label: cannot read $path"
    fi
}

if [[ $EUID -ne 0 ]]; then
    echo "SKIP test_cas_branch_merge: needs root for cgroup creation"
    exit 0
fi

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"

mkdir "$CG1" || { echo "SKIP test_cas_branch_merge: cgroup v2 unavailable"; exit 0; }

echo "base" > "$SRC/base.txt"

start_daemon || { echo "FAIL: control socket not ready"; exit 1; }

"$CTL" --sock "$SOCK" checkpoint base >/dev/null \
    && pass "checkpoint base" || fail "checkpoint base"

FEATURE_ID="$("$CTL" --sock "$SOCK" branch create feature)"
[[ "$FEATURE_ID" =~ ^[0-9]+$ ]] && pass "create feature branch (id=$FEATURE_ID)" || fail "create feature branch: $FEATURE_ID"

"$CTL" --sock "$SOCK" session register --cgroup "$CG1" --id 1 --branch feature >/dev/null \
    && pass "register cg1 to feature" || fail "register cg1 to feature"

(
    echo $BASHPID > "$CG1/cgroup.procs" || exit 1
    echo "source" > "$MNT/source.txt"
) && pass "feature writes source.txt" || fail "feature writes source.txt"

echo "target" > "$MNT/target.txt" \
    && pass "main writes target.txt" || fail "main writes target.txt"

if [[ ! -e "$MNT/source.txt" ]]; then
    pass "main does not see source.txt before merge"
else
    fail "main sees source.txt before merge: '$(cat "$MNT/source.txt" 2>/dev/null || true)'"
fi

if FEATURE_SOURCE="$(
    echo $BASHPID > "$CG1/cgroup.procs" || exit 1
    cat "$MNT/source.txt"
)"; then
    assert_eq "$FEATURE_SOURCE" "source" "feature sees source.txt before merge"
else
    fail "feature cannot read source.txt before merge"
fi

assert_file_eq "$MNT/target.txt" "target" "main sees target.txt before merge"

if (
    echo $BASHPID > "$CG1/cgroup.procs" || exit 1
    [[ ! -e "$MNT/target.txt" ]]
); then
    pass "feature does not see target.txt before merge"
else
    fail "feature sees target.txt before merge"
fi

MERGE_COMMIT="$("$CTL" --sock "$SOCK" branch merge feature --into main)"
[[ "$MERGE_COMMIT" =~ ^[0-9a-f]{64}$ ]] && pass "merge feature into main ($MERGE_COMMIT)" || fail "merge feature into main: $MERGE_COMMIT"

assert_file_eq "$MNT/source.txt" "source" "main sees merged source.txt"
assert_file_eq "$MNT/target.txt" "target" "main keeps target.txt"

CONFLICT_ID="$("$CTL" --sock "$SOCK" branch create conflict)"
[[ "$CONFLICT_ID" =~ ^[0-9]+$ ]] && pass "create conflict branch (id=$CONFLICT_ID)" || fail "create conflict branch: $CONFLICT_ID"

"$CTL" --sock "$SOCK" session register --cgroup "$CG1" --id 2 --branch conflict >/dev/null \
    && pass "register cg1 to conflict" || fail "register cg1 to conflict"

(
    echo $BASHPID > "$CG1/cgroup.procs" || exit 1
    echo "source-conflict" > "$MNT/base.txt"
) && pass "conflict branch edits base.txt" || fail "conflict branch edits base.txt"

echo "target-conflict" > "$MNT/base.txt" \
    && pass "main edits base.txt" || fail "main edits base.txt"

MERGE_OUT="$ROOT/conflict.out"
set +e
"$CTL" --sock "$SOCK" branch merge conflict --into main >"$MERGE_OUT" 2>&1
RC=$?
set -e

[[ "$RC" -eq 1 ]] && pass "conflicting merge exits 1" || fail "conflicting merge exit code: $RC"
grep -q "merge conflicts" "$MERGE_OUT" && pass "conflicting merge reports conflicts" || fail "conflicting merge missing conflicts message: $(cat "$MERGE_OUT")"
grep -q "/base.txt" "$MERGE_OUT" && pass "conflicting merge reports /base.txt" || fail "conflicting merge missing /base.txt: $(cat "$MERGE_OUT")"
assert_file_eq "$MNT/base.txt" "target-conflict" "main base.txt remains target version"

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ "$FAIL" -eq 0 ]] && echo "PASS test_cas_branch_merge" || { echo "FAIL test_cas_branch_merge"; exit 1; }
