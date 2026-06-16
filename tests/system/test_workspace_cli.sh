#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-workspace-test}"
LIST_ROOT="${ROOT}-list"
BIN="${BIN:-$(pwd)/build/agentvfs}"
SESSION="default"

cleanup() {
    "$BIN" workspace stop "$SESSION" --root "$ROOT" --no-checkpoint >/dev/null 2>&1 || true
    # Defensive: unmount any FUSE mounts we left behind. workspace stop will
    # normally handle this once Task 5 lands, but the trap still needs to
    # work when stop fails or isn't implemented yet.
    if [[ -d "$ROOT" ]]; then
        for mnt in "$ROOT"/*/mount; do
            [[ -d "$mnt" ]] || continue
            mountpoint -q "$mnt" && fusermount3 -u "$mnt" 2>/dev/null || true
        done
        # --mount override targets (placed at the root, not under <name>/mount).
        for mnt in "$ROOT"/custom-mnt-target "$ROOT"/init-custom-mnt; do
            [[ -d "$mnt" ]] || continue
            mountpoint -q "$mnt" && fusermount3 -u "$mnt" 2>/dev/null || true
        done
    fi
    rm -rf "$ROOT"
    if [[ -d "$LIST_ROOT" ]]; then
        for mnt in "$LIST_ROOT"/*/mount; do
            [[ -d "$mnt" ]] || continue
            mountpoint -q "$mnt" && fusermount3 -u "$mnt" 2>/dev/null || true
        done
        rm -rf "$LIST_ROOT"
    fi
}
trap cleanup EXIT

get_kv() {
    local key="$1"
    sed -n "s/^${key}=//p"
}

rm -rf "$ROOT"

START1="$("$BIN" workspace start "$SESSION" --root "$ROOT" --telemetry none)"
echo "$START1"
grep -q '^name=default$' <<<"$START1" || { echo "FAIL: missing name"; exit 1; }
grep -q '^status=started$' <<<"$START1" || { echo "FAIL: missing started status"; exit 1; }
grep -q '^telemetry=none$' <<<"$START1" || { echo "FAIL: telemetry none not reported"; exit 1; }

MNT="$(get_kv mount <<<"$START1")"
SOCK="$(get_kv socket <<<"$START1")"
STORE="$(get_kv store <<<"$START1")"
[[ -d "$MNT" ]] || { echo "FAIL: mount dir missing"; exit 1; }
[[ -S "$SOCK" ]] || { echo "FAIL: socket missing"; exit 1; }
[[ -d "$STORE" ]] || { echo "FAIL: store dir missing"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: mountpoint not active"; exit 1; }

START2="$("$BIN" workspace start "$SESSION" --root "$ROOT" --telemetry none)"
MNT2="$(get_kv mount <<<"$START2")"
SOCK2="$(get_kv socket <<<"$START2")"
[[ "$MNT2" == "$MNT" ]] || { echo "FAIL: idempotent start changed mount"; exit 1; }
[[ "$SOCK2" == "$SOCK" ]] || { echo "FAIL: idempotent start changed socket"; exit 1; }

STATUS="$("$BIN" workspace status "$SESSION" --root "$ROOT")"
grep -q '^status=started$' <<<"$STATUS" || { echo "FAIL: status did not report started"; echo "$STATUS"; exit 1; }
grep -q "^mount=$MNT$" <<<"$STATUS" || { echo "FAIL: status mount mismatch"; echo "$STATUS"; exit 1; }

echo "v1" > "$MNT/a.txt"
FIRST="$("$BIN" workspace checkpoint "$SESSION" first --root "$ROOT")"
[[ "$FIRST" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: checkpoint output '$FIRST'"; exit 1; }

echo "v2" > "$MNT/a.txt"
SECOND="$("$BIN" workspace checkpoint "$SESSION" second --root "$ROOT")"
[[ "$SECOND" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: checkpoint output '$SECOND'"; exit 1; }
[[ "$SECOND" != "$FIRST" ]] || { echo "FAIL: checkpoint hashes should differ"; exit 1; }

ROLLED="$("$BIN" workspace rollback "$SESSION" first --root "$ROOT")"
[[ "$ROLLED" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: rollback output '$ROLLED'"; exit 1; }
[[ "$(cat "$MNT/a.txt")" == "v1" ]] || { echo "FAIL: rollback did not restore v1"; exit 1; }

STOP="$("$BIN" workspace stop "$SESSION" --root "$ROOT")"
echo "$STOP"
grep -q '^status=stopped$' <<<"$STOP" || { echo "FAIL: stop did not report stopped"; exit 1; }
grep -q '^checkpoint=' <<<"$STOP" || { echo "FAIL: stop did not report final checkpoint"; exit 1; }
mountpoint -q "$MNT" && { echo "FAIL: mount still active after stop"; exit 1; }

AUTO_SESSION="auto-fallback"
AUTO_START="$("$BIN" workspace start "$AUTO_SESSION" --root "$ROOT")"
grep -q '^warning=no usable telemetry backend found; mounted without telemetry$' <<<"$AUTO_START" || {
    echo "FAIL: auto fallback warning missing"; echo "$AUTO_START"; exit 1;
}
grep -q '^telemetry=none$' <<<"$AUTO_START" || {
    echo "FAIL: auto fallback telemetry=none missing"; echo "$AUTO_START"; exit 1;
}
AUTO_MNT="$(get_kv mount <<<"$AUTO_START")"
mountpoint -q "$AUTO_MNT" || { echo "FAIL: auto fallback mount not active"; exit 1; }
"$BIN" workspace stop "$AUTO_SESSION" --root "$ROOT" --no-checkpoint >/dev/null

STALE="stale-session"
STALE_DIR="$ROOT/$STALE"
mkdir -p "$STALE_DIR"
cat > "$STALE_DIR/session.json" <<EOF
{
  "name":"$STALE",
  "pid":999999,
  "root":"$STALE_DIR",
  "source":"$STALE_DIR/source",
  "mount":"$STALE_DIR/mount",
  "store":"$STALE_DIR/store",
  "socket":"$STALE_DIR/control.sock",
  "log":"$STALE_DIR/daemon.log",
  "telemetry":"none",
  "status":"started",
  "created_at":"2026-04-29T12:34:56Z"
}
EOF

STALE_START="$("$BIN" workspace start "$STALE" --root "$ROOT" --telemetry none)"
grep -q '^status=started$' <<<"$STALE_START" || { echo "FAIL: stale start did not recover"; echo "$STALE_START"; exit 1; }
"$BIN" workspace stop "$STALE" --root "$ROOT" --no-checkpoint >/dev/null

RACE_SESSION="race"
"$BIN" workspace start "$RACE_SESSION" --root "$ROOT" --telemetry none >"$ROOT/race-1.out" 2>&1 &
P1=$!
"$BIN" workspace start "$RACE_SESSION" --root "$ROOT" --telemetry none >"$ROOT/race-2.out" 2>&1 &
P2=$!
wait "$P1" || { echo "FAIL: race start 1 exited non-zero"; cat "$ROOT/race-1.out"; exit 1; }
wait "$P2" || { echo "FAIL: race start 2 exited non-zero"; cat "$ROOT/race-2.out"; exit 1; }

RACE_MNT_LINES="$(grep -hE '^mount=' "$ROOT/race-1.out" "$ROOT/race-2.out" | sort -u)"
RACE_MNT_COUNT="$(echo "$RACE_MNT_LINES" | wc -l)"
[[ "$RACE_MNT_COUNT" == "1" ]] || { echo "FAIL: concurrent start produced different mount paths"; echo "$RACE_MNT_LINES"; exit 1; }

RACE_MNT="$(echo "$RACE_MNT_LINES" | sed 's/^mount=//')"
RACE_DAEMONS="$(pgrep -af -- "--mountpoint $RACE_MNT " | wc -l)"
[[ "$RACE_DAEMONS" == "1" ]] || {
    echo "FAIL: concurrent start spawned $RACE_DAEMONS daemons";
    pgrep -af -- "--mountpoint $RACE_MNT " || true;
    exit 1;
}

"$BIN" workspace stop "$RACE_SESSION" --root "$ROOT" --no-checkpoint >/dev/null

KILL_SESSION="kill-recovery"
KILL_START="$("$BIN" workspace start "$KILL_SESSION" --root "$ROOT" --telemetry none)"
KILL_MNT="$(get_kv mount <<<"$KILL_START")"
KILL_PID="$(grep -E '"pid":' "$ROOT/$KILL_SESSION/session.json" | tr -dc '0-9')"
[[ -n "$KILL_PID" ]] || { echo "FAIL: pid missing from session.json"; exit 1; }

kill -9 "$KILL_PID"
for _ in 1 2 3 4 5 6 7 8 9 10; do
    kill -0 "$KILL_PID" 2>/dev/null || break
    sleep 0.1
done

mountpoint -q "$KILL_MNT" || { echo "FAIL: SIGKILL should leave mount stuck until fusermount3"; exit 1; }

set +e
KILL_STOP_OUT="$("$BIN" workspace stop "$KILL_SESSION" --root "$ROOT" 2>&1)"
KILL_STOP_RC=$?
set -e
[[ "$KILL_STOP_RC" -eq 1 ]] || { echo "FAIL: stop on unhealthy should exit 1, got $KILL_STOP_RC"; echo "$KILL_STOP_OUT"; exit 1; }
grep -q "cannot checkpoint unhealthy" <<<"$KILL_STOP_OUT" || { echo "FAIL: missing refusal message"; echo "$KILL_STOP_OUT"; exit 1; }
mountpoint -q "$KILL_MNT" || { echo "FAIL: refused stop should leave mount mounted"; exit 1; }

"$BIN" workspace stop "$KILL_SESSION" --root "$ROOT" --no-checkpoint >/dev/null
mountpoint -q "$KILL_MNT" && { echo "FAIL: --no-checkpoint stop should unmount"; exit 1; } || true

STATUS_AFTER_KILL="$("$BIN" workspace status "$KILL_SESSION" --root "$ROOT" || true)"
grep -qE '^status=(stale|stopped)$' <<<"$STATUS_AFTER_KILL" || {
    echo "FAIL: status after crash should be stale or stopped"; echo "$STATUS_AFTER_KILL"; exit 1;
}

RECOVER_OUT="$("$BIN" workspace start "$KILL_SESSION" --root "$ROOT" --telemetry none)"
echo "$RECOVER_OUT"
grep -q '^status=started$' <<<"$RECOVER_OUT" || { echo "FAIL: kill-recovery start did not succeed"; echo "$RECOVER_OUT"; exit 1; }
RECOVER_MNT="$(get_kv mount <<<"$RECOVER_OUT")"
[[ "$RECOVER_MNT" == "$KILL_MNT" ]] || { echo "FAIL: kill-recovery start changed mount path"; exit 1; }
mountpoint -q "$RECOVER_MNT" || { echo "FAIL: kill-recovery start did not remount"; exit 1; }

"$BIN" workspace stop "$KILL_SESSION" --root "$ROOT" --no-checkpoint >/dev/null

INIT_SESSION="seeded"
INIT_SRC="$ROOT/seed-src"
mkdir -p "$INIT_SRC/sub"
echo "alpha" > "$INIT_SRC/a.txt"
echo "beta" > "$INIT_SRC/sub/b.txt"

INIT_OUT="$("$BIN" workspace init "$INIT_SESSION" --from "$INIT_SRC" --root "$ROOT")"
echo "$INIT_OUT"
grep -q "^name=$INIT_SESSION$" <<<"$INIT_OUT" || { echo "FAIL: init missing name"; exit 1; }
grep -q "^seeded_from=$INIT_SRC$" <<<"$INIT_OUT" || { echo "FAIL: init missing seeded_from"; exit 1; }
grep -q '^status=initialized$' <<<"$INIT_OUT" || { echo "FAIL: init missing status"; exit 1; }

[[ -f "$ROOT/$INIT_SESSION/source/a.txt" ]] || { echo "FAIL: init did not copy a.txt"; exit 1; }
[[ -f "$ROOT/$INIT_SESSION/source/sub/b.txt" ]] || { echo "FAIL: init did not copy sub/b.txt"; exit 1; }

INIT_START="$("$BIN" workspace start "$INIT_SESSION" --root "$ROOT" --telemetry none)"
INIT_MNT="$(get_kv mount <<<"$INIT_START")"
[[ "$(cat "$INIT_MNT/a.txt")" == "alpha" ]] || { echo "FAIL: mount missing seeded a.txt"; exit 1; }
[[ "$(cat "$INIT_MNT/sub/b.txt")" == "beta" ]] || { echo "FAIL: mount missing seeded sub/b.txt"; exit 1; }

set +e
"$BIN" workspace init "$INIT_SESSION" --from "$INIT_SRC" --root "$ROOT" >"$ROOT/init-reuse.out" 2>&1
RC_REUSE=$?
set -e
[[ "$RC_REUSE" -ne 0 ]] || { echo "FAIL: init on existing session should refuse"; cat "$ROOT/init-reuse.out"; exit 1; }
grep -q "already exists" "$ROOT/init-reuse.out" || { echo "FAIL: init refusal missing message"; cat "$ROOT/init-reuse.out"; exit 1; }

"$BIN" workspace stop "$INIT_SESSION" --root "$ROOT" --no-checkpoint >/dev/null

# --mount override: CLI flag wins, persists to workspace.json, sticky next start.
OVERRIDE_SESSION="mount-elsewhere"
OVERRIDE_MNT="$ROOT/custom-mnt-target"
rm -rf "$OVERRIDE_MNT"

OVERRIDE_START="$("$BIN" workspace start "$OVERRIDE_SESSION" --root "$ROOT" --mount "$OVERRIDE_MNT" --telemetry none)"
echo "$OVERRIDE_START"
OVERRIDE_REPORTED="$(get_kv mount <<<"$OVERRIDE_START")"
[[ "$OVERRIDE_REPORTED" == "$OVERRIDE_MNT" ]] || {
    echo "FAIL: --mount override not reported (got '$OVERRIDE_REPORTED')"; exit 1;
}
mountpoint -q "$OVERRIDE_MNT" || { echo "FAIL: override mount not active"; exit 1; }
grep -q "\"mount\":\"$OVERRIDE_MNT\"" "$ROOT/$OVERRIDE_SESSION/session.json" || {
    echo "FAIL: session.json does not record override"; exit 1;
}
grep -q "\"mount_override\":\"$OVERRIDE_MNT\"" "$ROOT/$OVERRIDE_SESSION/workspace.json" || {
    echo "FAIL: workspace.json missing mount_override"; exit 1;
}

OVERRIDE_STATUS="$("$BIN" workspace status "$OVERRIDE_SESSION" --root "$ROOT")"
grep -q "^mount=$OVERRIDE_MNT$" <<<"$OVERRIDE_STATUS" || {
    echo "FAIL: status mount mismatch"; echo "$OVERRIDE_STATUS"; exit 1;
}

# Re-running start with a *different* --mount on a healthy session must not
# silently change the mount; it should warn and keep the existing mount.
CONFLICT_OUT="$("$BIN" workspace start "$OVERRIDE_SESSION" --root "$ROOT" --mount "$ROOT/should-be-ignored" --telemetry none 2>&1)"
grep -q "is already running at $OVERRIDE_MNT" <<<"$CONFLICT_OUT" || {
    echo "FAIL: missing conflict warning for --mount on healthy session";
    echo "$CONFLICT_OUT"; exit 1;
}
grep -q "^mount=$OVERRIDE_MNT$" <<<"$CONFLICT_OUT" || {
    echo "FAIL: idempotent start should still report existing mount";
    echo "$CONFLICT_OUT"; exit 1;
}
[[ ! -d "$ROOT/should-be-ignored" ]] || {
    echo "FAIL: ignored --mount path should not be created"; exit 1;
}

"$BIN" workspace stop "$OVERRIDE_SESSION" --root "$ROOT" --no-checkpoint >/dev/null
mountpoint -q "$OVERRIDE_MNT" && { echo "FAIL: stop did not unmount override"; exit 1; } || true

# Restart without --mount; must inherit the persisted override.
STICKY_START="$("$BIN" workspace start "$OVERRIDE_SESSION" --root "$ROOT" --telemetry none)"
STICKY_REPORTED="$(get_kv mount <<<"$STICKY_START")"
[[ "$STICKY_REPORTED" == "$OVERRIDE_MNT" ]] || {
    echo "FAIL: persisted override not reused (got '$STICKY_REPORTED')"; exit 1;
}
mountpoint -q "$OVERRIDE_MNT" || { echo "FAIL: sticky restart did not mount override"; exit 1; }
"$BIN" workspace stop "$OVERRIDE_SESSION" --root "$ROOT" --no-checkpoint >/dev/null
rm -rf "$OVERRIDE_MNT"

# --mount on init: persist before any start.
INIT_MNT_SESSION="initmount"
INIT_MNT_TARGET="$ROOT/init-custom-mnt"
rm -rf "$INIT_MNT_TARGET"

"$BIN" workspace init "$INIT_MNT_SESSION" --from "$INIT_SRC" --root "$ROOT" --mount "$INIT_MNT_TARGET" >/dev/null
grep -q "\"mount_override\":\"$INIT_MNT_TARGET\"" "$ROOT/$INIT_MNT_SESSION/workspace.json" || {
    echo "FAIL: init --mount did not persist workspace.json"; exit 1;
}

INIT_MNT_START="$("$BIN" workspace start "$INIT_MNT_SESSION" --root "$ROOT" --telemetry none)"
INIT_MNT_REPORTED="$(get_kv mount <<<"$INIT_MNT_START")"
[[ "$INIT_MNT_REPORTED" == "$INIT_MNT_TARGET" ]] || {
    echo "FAIL: start after init --mount did not honor override (got '$INIT_MNT_REPORTED')"; exit 1;
}
"$BIN" workspace stop "$INIT_MNT_SESSION" --root "$ROOT" --no-checkpoint >/dev/null
rm -rf "$INIT_MNT_TARGET"

rm -rf "$LIST_ROOT"

EMPTY_LIST="$("$BIN" workspace list --root "$LIST_ROOT")"
[[ -z "$EMPTY_LIST" ]] || { echo "FAIL: list of empty root should be empty"; echo "$EMPTY_LIST"; exit 1; }

LIST_A_OUT="$("$BIN" workspace start ws-a --root "$LIST_ROOT" --telemetry none)"
LIST_A_MNT="$(get_kv mount <<<"$LIST_A_OUT")"
LIST_B_OUT="$("$BIN" workspace start ws-b --root "$LIST_ROOT" --telemetry none)"
LIST_B_MNT="$(get_kv mount <<<"$LIST_B_OUT")"

LIST_OUT="$("$BIN" workspace list --root "$LIST_ROOT")"
echo "$LIST_OUT"
LIST_LINES="$(echo "$LIST_OUT" | wc -l)"
[[ "$LIST_LINES" == "2" ]] || { echo "FAIL: list should report 2 lines, got $LIST_LINES"; exit 1; }
grep -qP "^ws-a\tstarted\t$LIST_A_MNT$" <<<"$LIST_OUT" || { echo "FAIL: ws-a missing"; exit 1; }
grep -qP "^ws-b\tstarted\t$LIST_B_MNT$" <<<"$LIST_OUT" || { echo "FAIL: ws-b missing"; exit 1; }

"$BIN" workspace stop ws-a --root "$LIST_ROOT" --no-checkpoint >/dev/null
"$BIN" workspace stop ws-b --root "$LIST_ROOT" --no-checkpoint >/dev/null

LIST_AFTER_STOP="$("$BIN" workspace list --root "$LIST_ROOT")"
grep -qP "^ws-a\tstopped\t" <<<"$LIST_AFTER_STOP" || { echo "FAIL: stopped ws-a missing"; echo "$LIST_AFTER_STOP"; exit 1; }
grep -qP "^ws-b\tstopped\t" <<<"$LIST_AFTER_STOP" || { echo "FAIL: stopped ws-b missing"; echo "$LIST_AFTER_STOP"; exit 1; }

rm -rf "$LIST_ROOT"

echo "PASS workspace lifecycle"
