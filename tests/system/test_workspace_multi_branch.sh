#!/bin/bash
# 5 branches x 5 checkpoints + rollback + merge through `agentvfs workspace`.
# Run: bash tests/system/test_workspace_multi_branch.sh
set -euo pipefail

BIN_DIR="${BIN_DIR:-$(pwd)/build}"
AGENTVFS="$BIN_DIR/agentvfs"
CTL="$BIN_DIR/agentvfs-ctl"
ROOT="${ROOT:-/tmp/agentvfs-ws-test}"
NAME="cas-test"
CG_BASE="/sys/fs/cgroup/agentvfs-ws-test-$$"

if [[ $EUID -ne 0 ]]; then
    echo "SKIP test_workspace_multi_branch: needs root for cgroup creation"
    exit 0
fi
[[ -x "$AGENTVFS" ]] || { echo "missing $AGENTVFS — build first: cmake -B build && cmake --build build -j"; exit 1; }
[[ -x "$CTL"      ]] || { echo "missing $CTL"; exit 1; }

PASS=0; FAIL=0
pass(){ echo "  PASS: $1"; PASS=$((PASS+1)); }
fail(){ echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

cleanup() {
    "$AGENTVFS" workspace stop "$NAME" --root "$ROOT" --no-checkpoint >/dev/null 2>&1 || true
    for b in 1 2 3 4 5; do rmdir "$CG_BASE/br$b" 2>/dev/null || true; done
    rmdir "$CG_BASE" 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$ROOT" "$CG_BASE"
for b in 1 2 3 4 5; do mkdir "$CG_BASE/br$b"; done

# 1. Start workspace with the default telemetry backend (auto: ebpf -> fanotify -> none).
START_OUT="$("$AGENTVFS" workspace start "$NAME" --root "$ROOT")"
echo "$START_OUT"
SOCK="$(grep '^socket='    <<<"$START_OUT" | cut -d= -f2-)"
MNT="$( grep '^mount='     <<<"$START_OUT" | cut -d= -f2-)"
TEL="$( grep '^telemetry=' <<<"$START_OUT" | cut -d= -f2-)"
[[ -S "$SOCK" && -d "$MNT" ]] && mountpoint -q "$MNT" \
    && pass "workspace started (telemetry=$TEL)" \
    || { fail "workspace did not come up"; exit 1; }

# Seed disjoint per-branch dirs on main, then snapshot so branches fork from a known point.
for b in 1 2 3 4 5; do mkdir -p "$MNT/br$b"; done
"$CTL" --sock "$SOCK" checkpoint initial >/dev/null \
    && pass "main checkpoint initial" || fail "main checkpoint initial"

# 2. Create 5 branches; bind a cgroup to each so per-branch writes route correctly.
for b in 1 2 3 4 5; do
    "$CTL" --sock "$SOCK" branch create "br$b" >/dev/null \
        && pass "branch create br$b" || fail "branch create br$b"
    "$CTL" --sock "$SOCK" session register \
        --cgroup "$CG_BASE/br$b" --id "$b" --branch "br$b" >/dev/null \
        && pass "session register br$b" || fail "session register br$b"
done

# 3. 5 checkpoints per branch — each branch only touches its own dir.
declare -A LAST_CP
for b in 1 2 3 4 5; do
    for cp in 1 2 3 4 5; do
        # The subshell joins br$b's cgroup so its writes route to br$b's working tree.
        ( echo $BASHPID > "$CG_BASE/br$b/cgroup.procs"
          echo "br${b}-cp${cp}" > "$MNT/br${b}/data.txt"
          echo "f${cp}-content"  > "$MNT/br${b}/f${cp}.txt" )
        OUT="$("$CTL" --sock "$SOCK" checkpoint "br${b}-cp${cp}" --branch "br$b")"
        if [[ "$OUT" =~ ^[0-9a-f]{64}$ ]]; then
            pass "br$b cp$cp (${OUT:0:12}…)"
            LAST_CP[$b]="$OUT"
        else
            fail "br$b cp$cp: $OUT"
        fi
    done
done

# 4. Rollback test — br3 -> cp2; verify f1/f2 present, f3..f5 gone, then forward to cp5.
"$CTL" --sock "$SOCK" rollback "br3-cp2" --branch br3 >/dev/null \
    && pass "rollback br3 → cp2" || fail "rollback br3 → cp2"
GOT="$( ( echo $BASHPID > "$CG_BASE/br3/cgroup.procs"; cat "$MNT/br3/data.txt" ) )"
[[ "$GOT" == "br3-cp2" ]] && pass "br3/data.txt == br3-cp2" \
                          || fail "br3/data.txt got '$GOT'"
for cp in 1 2; do
    ( echo $BASHPID > "$CG_BASE/br3/cgroup.procs"; [[ -e "$MNT/br3/f${cp}.txt" ]] ) \
        && pass "br3 f${cp}.txt present" || fail "br3 f${cp}.txt missing"
done
for cp in 3 4 5; do
    ( echo $BASHPID > "$CG_BASE/br3/cgroup.procs"; [[ ! -e "$MNT/br3/f${cp}.txt" ]] ) \
        && pass "br3 f${cp}.txt gone" || fail "br3 f${cp}.txt unexpectedly present"
done
"$CTL" --sock "$SOCK" rollback "${LAST_CP[3]}" --branch br3 >/dev/null \
    && pass "br3 forward to cp5" || fail "br3 forward to cp5"

# 5. Merge each branch into main. Disjoint paths -> conflict-free.
for b in 1 2 3 4 5; do
    OUT="$("$CTL" --sock "$SOCK" branch merge "br$b" --into main)"
    if [[ "$OUT" =~ ^[0-9a-f]{64}$ ]]; then
        pass "merge br$b → main (${OUT:0:12}…)"
    else
        fail "merge br$b → main: $OUT"
    fi
    GOT="$(cat "$MNT/br${b}/data.txt")"
    [[ "$GOT" == "br${b}-cp5" ]] \
        && pass "main sees br${b}/data.txt == br${b}-cp5" \
        || fail "main sees br${b}/data.txt == '$GOT'"
    for cp in 1 2 3 4 5; do
        GOT="$(cat "$MNT/br${b}/f${cp}.txt" 2>/dev/null || true)"
        [[ "$GOT" == "f${cp}-content" ]] \
            && pass "main sees br${b}/f${cp}.txt" \
            || fail "main missing br${b}/f${cp}.txt (got '$GOT')"
    done
done

# 6. Inspect telemetry backend status, then stop. `stop` checkpoints by default
#    and refuses to unmount if the final checkpoint fails.
echo
echo "telemetry.status:"
"$CTL" --sock "$SOCK" raw telemetry.status || true
echo

"$AGENTVFS" workspace stop "$NAME" --root "$ROOT" >/dev/null \
    && pass "workspace stop" || fail "workspace stop"

echo
echo "=== Results: $PASS passed, $FAIL failed ==="
[[ $FAIL -eq 0 ]] && exit 0 || exit 1
