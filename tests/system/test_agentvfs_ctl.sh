#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-ctl}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
SRC_NOF="$ROOT/src-no-f"
MNT_NOF="$ROOT/mnt-no-f"
STORE_NOF="$ROOT/store-no-f"
SOCK_NOF="$ROOT/control-no-f.sock"
BIN="$(pwd)/build/agentvfs"
CTL="$(pwd)/build/agentvfs-ctl"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    fusermount3 -u "$MNT_NOF" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT"
echo "hello" > "$SRC/a.txt"

"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }

# 1. status returns JSON with ok:true
STATUS="$("$CTL" --sock "$SOCK" status)"
grep -q '"ok":true' <<<"$STATUS" || { echo "FAIL: status resp=$STATUS"; exit 1; }
grep -q '"version"' <<<"$STATUS" || { echo "FAIL: status missing version; resp=$STATUS"; exit 1; }

# 2. checkpoint prints the commit hash on stdout, exit 0
COMMIT="$("$CTL" --sock "$SOCK" checkpoint my-label)"
[[ "$COMMIT" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: checkpoint output '$COMMIT' is not a 64-hex hash"; exit 1; }

# 3. rollback prints the rolled-back-to hash on stdout, exit 0
ROLLED="$("$CTL" --sock "$SOCK" rollback my-label)"
[[ "$ROLLED" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: rollback output '$ROLLED' is not a 64-hex hash"; exit 1; }

# 4. --json emits raw JSON
RAW_STATUS="$("$CTL" --sock "$SOCK" --json status)"
grep -q '"ok":true' <<<"$RAW_STATUS" || { echo "FAIL: --json status resp=$RAW_STATUS"; exit 1; }

# 5. $AGENTVFS_SOCK env var is respected
ENV_STATUS="$(AGENTVFS_SOCK="$SOCK" "$CTL" status)"
grep -q '"ok":true' <<<"$ENV_STATUS" || { echo "FAIL: env socket resp=$ENV_STATUS"; exit 1; }

# 6. raw subcommand forwards a line verbatim
RAW="$("$CTL" --sock "$SOCK" raw status)"
grep -q '"ok":true' <<<"$RAW" || { echo "FAIL: raw status resp=$RAW"; exit 1; }

# 7. branch merge prints the merge commit hash on stdout, exit 0
FEATURE_ID="$("$CTL" --sock "$SOCK" branch create feature-cli)"
[[ "$FEATURE_ID" =~ ^[0-9]+$ ]] || { echo "FAIL: branch create feature-cli output '$FEATURE_ID' is not numeric"; exit 1; }
MERGE_COMMIT="$("$CTL" --sock "$SOCK" branch merge feature-cli --into main)"
[[ "$MERGE_COMMIT" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: branch merge output '$MERGE_COMMIT' is not a 64-hex hash"; exit 1; }

# 8. branch merge requires --into before attempting a socket request
MERGE_MISSING_INTO_OUT="$ROOT/merge-missing-into.out"
set +e
"$CTL" --sock /nonexistent/path branch merge feature-cli >"$MERGE_MISSING_INTO_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: branch merge without --into should exit 2 (got $RC)"; cat "$MERGE_MISSING_INTO_OUT"; exit 1; }
rm -f "$MERGE_MISSING_INTO_OUT"

# 9. branch merge rejects extra positional args before attempting a socket request
MERGE_EXTRA_OUT="$ROOT/merge-extra-positional.out"
set +e
"$CTL" --sock /nonexistent/path branch merge feature-cli extra --into main >"$MERGE_EXTRA_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: branch merge extra positional should exit 2 (got $RC)"; cat "$MERGE_EXTRA_OUT"; exit 1; }
grep -q "branch merge takes exactly one <source>" "$MERGE_EXTRA_OUT" || { echo "FAIL: branch merge extra positional missing clear error"; cat "$MERGE_EXTRA_OUT"; exit 1; }
rm -f "$MERGE_EXTRA_OUT"

# 10. branch merge rejects unknown flags before attempting a socket request
MERGE_UNKNOWN_FLAG_OUT="$ROOT/merge-unknown-flag.out"
set +e
"$CTL" --sock /nonexistent/path branch merge feature-cli --bad value --into main >"$MERGE_UNKNOWN_FLAG_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: branch merge unknown flag should exit 2 (got $RC)"; cat "$MERGE_UNKNOWN_FLAG_OUT"; exit 1; }
grep -q "branch merge unknown option --bad" "$MERGE_UNKNOWN_FLAG_OUT" || { echo "FAIL: branch merge unknown flag missing clear error"; cat "$MERGE_UNKNOWN_FLAG_OUT"; exit 1; }
rm -f "$MERGE_UNKNOWN_FLAG_OUT"

# runtime snapshot requires a runtime id before attempting a socket request
RUNTIME_SNAPSHOT_OUT="$ROOT/runtime-snapshot.out"
set +e
"$CTL" --sock /nonexistent/path runtime snapshot >"$RUNTIME_SNAPSHOT_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime snapshot without id should exit 2 (got $RC)"; cat "$RUNTIME_SNAPSHOT_OUT"; exit 1; }
grep -q "runtime snapshot requires <runtime-id>" "$RUNTIME_SNAPSHOT_OUT" || { echo "FAIL: runtime snapshot missing clear error"; cat "$RUNTIME_SNAPSHOT_OUT"; exit 1; }
rm -f "$RUNTIME_SNAPSHOT_OUT"

# runtime snapshot rejects a non-64-hex --agent-state before attempting a
# socket request (the validation must fire client-side, so a deliberately
# bad socket path is fine and proves no socket round-trip happened).
RUNTIME_SNAPSHOT_BADSTATE_OUT="$ROOT/runtime-snapshot-badstate.out"
set +e
"$CTL" --sock /nonexistent/path runtime snapshot rt-test --agent-state not-a-hash >"$RUNTIME_SNAPSHOT_BADSTATE_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime snapshot bad --agent-state should exit 2 (got $RC)"; cat "$RUNTIME_SNAPSHOT_BADSTATE_OUT"; exit 1; }
grep -q "runtime snapshot requires 64-hex --agent-state" "$RUNTIME_SNAPSHOT_BADSTATE_OUT" || { echo "FAIL: runtime snapshot bad --agent-state missing clear error"; cat "$RUNTIME_SNAPSHOT_BADSTATE_OUT"; exit 1; }
rm -f "$RUNTIME_SNAPSHOT_BADSTATE_OUT"

RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT="$ROOT/runtime-snapshot-overlong-state.out"
set +e
"$CTL" --sock /nonexistent/path runtime snapshot rt-test --agent-state "$(printf 'a%.0s' {1..65})" >"$RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime snapshot overlong --agent-state should exit 2 (got $RC)"; cat "$RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT"; exit 1; }
grep -q "runtime snapshot requires 64-hex --agent-state" "$RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT" || { echo "FAIL: runtime snapshot overlong --agent-state missing clear error"; cat "$RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT"; exit 1; }
rm -f "$RUNTIME_SNAPSHOT_OVERLONG_STATE_OUT"

# runtime restore requires a union state id before attempting a socket request
RUNTIME_RESTORE_OUT="$ROOT/runtime-restore.out"
set +e
"$CTL" --sock /nonexistent/path runtime restore >"$RUNTIME_RESTORE_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime restore without id should exit 2 (got $RC)"; cat "$RUNTIME_RESTORE_OUT"; exit 1; }
grep -q "runtime restore requires <union-state-id>" "$RUNTIME_RESTORE_OUT" || { echo "FAIL: runtime restore missing clear error"; cat "$RUNTIME_RESTORE_OUT"; exit 1; }
rm -f "$RUNTIME_RESTORE_OUT"

RUNTIME_RESTORE_OVERLONG_OUT="$ROOT/runtime-restore-overlong.out"
set +e
"$CTL" --sock /nonexistent/path runtime restore "$(printf 'a%.0s' {1..65})" >"$RUNTIME_RESTORE_OVERLONG_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime restore overlong id should exit 2 (got $RC)"; cat "$RUNTIME_RESTORE_OVERLONG_OUT"; exit 1; }
grep -q "runtime restore requires 64-hex <union-state-id>" "$RUNTIME_RESTORE_OVERLONG_OUT" || { echo "FAIL: runtime restore overlong id missing clear error"; cat "$RUNTIME_RESTORE_OVERLONG_OUT"; exit 1; }
rm -f "$RUNTIME_RESTORE_OVERLONG_OUT"

# runtime create is not a public manual-registration path; callers must use
# agentvfs-run so the runtime gets an unguessable control token.
RUNTIME_CREATE_UNSUPPORTED_OUT="$ROOT/runtime-create-unsupported.out"
set +e
"$CTL" --sock /nonexistent/path runtime create --id x --pid 1 --pgid 1 >"$RUNTIME_CREATE_UNSUPPORTED_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime create should exit 2 (got $RC)"; cat "$RUNTIME_CREATE_UNSUPPORTED_OUT"; exit 1; }
grep -q "runtime create is unsupported; use agentvfs-run" "$RUNTIME_CREATE_UNSUPPORTED_OUT" || { echo "FAIL: runtime create missing clear error"; cat "$RUNTIME_CREATE_UNSUPPORTED_OUT"; exit 1; }
rm -f "$RUNTIME_CREATE_UNSUPPORTED_OUT"

# runtime snapshot rejects a non-numeric --timeout-ms before attempting a socket request
RUNTIME_SNAPSHOT_BADTMO_OUT="$ROOT/runtime-snapshot-badtimeout.out"
set +e
"$CTL" --sock /nonexistent/path runtime snapshot rt --timeout-ms notanumber >"$RUNTIME_SNAPSHOT_BADTMO_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime snapshot non-numeric --timeout-ms should exit 2 (got $RC)"; cat "$RUNTIME_SNAPSHOT_BADTMO_OUT"; exit 1; }
grep -q "runtime snapshot requires numeric --timeout-ms" "$RUNTIME_SNAPSHOT_BADTMO_OUT" || { echo "FAIL: runtime snapshot non-numeric --timeout-ms missing clear error"; cat "$RUNTIME_SNAPSHOT_BADTMO_OUT"; exit 1; }
rm -f "$RUNTIME_SNAPSHOT_BADTMO_OUT"

# runtime restore rejects a non-numeric --timeout-ms before attempting a socket request
RUNTIME_RESTORE_BADTMO_OUT="$ROOT/runtime-restore-badtimeout.out"
set +e
"$CTL" --sock /nonexistent/path runtime restore "$(printf 'b%.0s' {1..64})" --timeout-ms 5s >"$RUNTIME_RESTORE_BADTMO_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: runtime restore non-numeric --timeout-ms should exit 2 (got $RC)"; cat "$RUNTIME_RESTORE_BADTMO_OUT"; exit 1; }
grep -q "runtime restore requires numeric --timeout-ms" "$RUNTIME_RESTORE_BADTMO_OUT" || { echo "FAIL: runtime restore non-numeric --timeout-ms missing clear error"; cat "$RUNTIME_RESTORE_BADTMO_OUT"; exit 1; }
rm -f "$RUNTIME_RESTORE_BADTMO_OUT"

# --- state command: syntax validation (exit 2 BEFORE any socket request) ---
# A deliberately bad socket path is fine here and proves no round-trip happened.

# state requires a subcommand
STATE_NO_SUB_OUT="$ROOT/state-no-sub.out"
set +e
"$CTL" --sock /nonexistent/path state >"$STATE_NO_SUB_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state without sub should exit 2 (got $RC)"; cat "$STATE_NO_SUB_OUT"; exit 1; }
rm -f "$STATE_NO_SUB_OUT"

# state append requires --agent
STATE_APPEND_NO_AGENT_OUT="$ROOT/state-append-no-agent.out"
set +e
"$CTL" --sock /nonexistent/path state append --kind session --schema s --payload '{}' >"$STATE_APPEND_NO_AGENT_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append without --agent should exit 2 (got $RC)"; cat "$STATE_APPEND_NO_AGENT_OUT"; exit 1; }
grep -q "state append requires --agent" "$STATE_APPEND_NO_AGENT_OUT" || { echo "FAIL: state append without --agent missing clear error"; cat "$STATE_APPEND_NO_AGENT_OUT"; exit 1; }
rm -f "$STATE_APPEND_NO_AGENT_OUT"

# state append requires --kind
STATE_APPEND_NO_KIND_OUT="$ROOT/state-append-no-kind.out"
set +e
"$CTL" --sock /nonexistent/path state append --agent codex --schema s --payload '{}' >"$STATE_APPEND_NO_KIND_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append without --kind should exit 2 (got $RC)"; cat "$STATE_APPEND_NO_KIND_OUT"; exit 1; }
grep -q "state append requires --kind" "$STATE_APPEND_NO_KIND_OUT" || { echo "FAIL: state append without --kind missing clear error"; cat "$STATE_APPEND_NO_KIND_OUT"; exit 1; }
rm -f "$STATE_APPEND_NO_KIND_OUT"

# state append requires --schema
STATE_APPEND_NO_SCHEMA_OUT="$ROOT/state-append-no-schema.out"
set +e
"$CTL" --sock /nonexistent/path state append --agent codex --kind session --payload '{}' >"$STATE_APPEND_NO_SCHEMA_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append without --schema should exit 2 (got $RC)"; cat "$STATE_APPEND_NO_SCHEMA_OUT"; exit 1; }
grep -q "state append requires --schema" "$STATE_APPEND_NO_SCHEMA_OUT" || { echo "FAIL: state append without --schema missing clear error"; cat "$STATE_APPEND_NO_SCHEMA_OUT"; exit 1; }
rm -f "$STATE_APPEND_NO_SCHEMA_OUT"

# state append requires --payload
STATE_APPEND_NO_PAYLOAD_OUT="$ROOT/state-append-no-payload.out"
set +e
"$CTL" --sock /nonexistent/path state append --agent codex --kind session --schema s >"$STATE_APPEND_NO_PAYLOAD_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append without --payload should exit 2 (got $RC)"; cat "$STATE_APPEND_NO_PAYLOAD_OUT"; exit 1; }
grep -q "state append requires --payload" "$STATE_APPEND_NO_PAYLOAD_OUT" || { echo "FAIL: state append without --payload missing clear error"; cat "$STATE_APPEND_NO_PAYLOAD_OUT"; exit 1; }
rm -f "$STATE_APPEND_NO_PAYLOAD_OUT"

# state append --sync requires --parent and --snapshot-base BEFORE any socket
# request (the service rejects sync without anchors; the CLI mirrors that
# client-side so the user gets a clear hint rather than a generic server error).
STATE_SYNC_NO_ANCHORS_OUT="$ROOT/state-sync-no-anchors.out"
set +e
"$CTL" --sock /nonexistent/path state append --agent codex --kind session \
    --schema s --payload '{}' --sync >"$STATE_SYNC_NO_ANCHORS_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append --sync without anchors should exit 2 (got $RC)"; cat "$STATE_SYNC_NO_ANCHORS_OUT"; exit 1; }
grep -q "state append --sync requires --parent" "$STATE_SYNC_NO_ANCHORS_OUT" || { echo "FAIL: state append --sync without --parent missing clear error"; cat "$STATE_SYNC_NO_ANCHORS_OUT"; exit 1; }
rm -f "$STATE_SYNC_NO_ANCHORS_OUT"

STATE_SYNC_NO_BASE_OUT="$ROOT/state-sync-no-base.out"
set +e
"$CTL" --sock /nonexistent/path state append --agent codex --kind session \
    --schema s --payload '{}' --parent deadbeef --sync >"$STATE_SYNC_NO_BASE_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state append --sync without --snapshot-base should exit 2 (got $RC)"; cat "$STATE_SYNC_NO_BASE_OUT"; exit 1; }
grep -q "state append --sync requires --snapshot-base" "$STATE_SYNC_NO_BASE_OUT" || { echo "FAIL: state append --sync without --snapshot-base missing clear error"; cat "$STATE_SYNC_NO_BASE_OUT"; exit 1; }
rm -f "$STATE_SYNC_NO_BASE_OUT"

# state describe requires <state-id>
STATE_DESCRIBE_NO_ID_OUT="$ROOT/state-describe-no-id.out"
set +e
"$CTL" --sock /nonexistent/path state describe >"$STATE_DESCRIBE_NO_ID_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state describe without id should exit 2 (got $RC)"; cat "$STATE_DESCRIBE_NO_ID_OUT"; exit 1; }
grep -q "state describe requires <state-id>" "$STATE_DESCRIBE_NO_ID_OUT" || { echo "FAIL: state describe without id missing clear error"; cat "$STATE_DESCRIBE_NO_ID_OUT"; exit 1; }
rm -f "$STATE_DESCRIBE_NO_ID_OUT"

# state latest requires --agent
STATE_LATEST_NO_AGENT_OUT="$ROOT/state-latest-no-agent.out"
set +e
"$CTL" --sock /nonexistent/path state latest >"$STATE_LATEST_NO_AGENT_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state latest without --agent should exit 2 (got $RC)"; cat "$STATE_LATEST_NO_AGENT_OUT"; exit 1; }
grep -q "state latest requires --agent" "$STATE_LATEST_NO_AGENT_OUT" || { echo "FAIL: state latest without --agent missing clear error"; cat "$STATE_LATEST_NO_AGENT_OUT"; exit 1; }
rm -f "$STATE_LATEST_NO_AGENT_OUT"

# state restore requires <state-id>
STATE_RESTORE_NO_ID_OUT="$ROOT/state-restore-no-id.out"
set +e
"$CTL" --sock /nonexistent/path state restore >"$STATE_RESTORE_NO_ID_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state restore without id should exit 2 (got $RC)"; cat "$STATE_RESTORE_NO_ID_OUT"; exit 1; }
grep -q "state restore requires <state-id>" "$STATE_RESTORE_NO_ID_OUT" || { echo "FAIL: state restore without id missing clear error"; cat "$STATE_RESTORE_NO_ID_OUT"; exit 1; }
rm -f "$STATE_RESTORE_NO_ID_OUT"

# state restore rejects a non-numeric --timeout-ms before attempting a socket request
STATE_RESTORE_BADTMO_OUT="$ROOT/state-restore-badtimeout.out"
set +e
"$CTL" --sock /nonexistent/path state restore deadbeef --timeout-ms 5s >"$STATE_RESTORE_BADTMO_OUT" 2>&1
RC=$?
set -e
[[ "$RC" -eq 2 ]] || { echo "FAIL: state restore non-numeric --timeout-ms should exit 2 (got $RC)"; cat "$STATE_RESTORE_BADTMO_OUT"; exit 1; }
grep -q "state restore requires numeric --timeout-ms" "$STATE_RESTORE_BADTMO_OUT" || { echo "FAIL: state restore non-numeric --timeout-ms missing clear error"; cat "$STATE_RESTORE_BADTMO_OUT"; exit 1; }
rm -f "$STATE_RESTORE_BADTMO_OUT"

# --- state command: end-to-end integration against the live daemon ---
# The service requires parent_state_id + snapshot_base_state_id (both referencing
# readable states) for sync=true, so build a chain: a logical-only base first,
# then a synced append anchored on it. synced append -> latest reflects it ->
# describe matches -> restore walks the chain.
BASE_ID="$("$CTL" --sock "$SOCK" state append --agent codex --kind session \
    --schema agentvfs.session.v1 --payload '{"turn":1}')"
[[ "$BASE_ID" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: state append (logical) output '$BASE_ID' is not a 64-hex hash"; exit 1; }

STATE_ID="$("$CTL" --sock "$SOCK" state append --agent codex --kind session \
    --schema agentvfs.session.v1 --payload '{"turn":2}' \
    --parent "$BASE_ID" --snapshot-base "$BASE_ID" --sync)"
[[ "$STATE_ID" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: state append (--sync) output '$STATE_ID' is not a 64-hex hash"; exit 1; }

# --json mode of append echoes the full JSON response including state_id + durability.
STATE_APPEND_JSON="$("$CTL" --sock "$SOCK" --json state append --agent codex \
    --kind session --schema agentvfs.session.v1 --payload '{"turn":3}' \
    --parent "$STATE_ID" --snapshot-base "$BASE_ID" --sync)"
grep -q '"ok":true' <<<"$STATE_APPEND_JSON" || { echo "FAIL: state append --json resp=$STATE_APPEND_JSON"; exit 1; }
grep -q '"state_id"' <<<"$STATE_APPEND_JSON" || { echo "FAIL: state append --json missing state_id; resp=$STATE_APPEND_JSON"; exit 1; }
grep -q '"durability":"durable"' <<<"$STATE_APPEND_JSON" || { echo "FAIL: state append --sync --json missing durability=durable; resp=$STATE_APPEND_JSON"; exit 1; }

# state latest (default branch main) reflects the most recently synced state.
STATE_LATEST_OUT="$("$CTL" --sock "$SOCK" state latest --agent codex --branch main)"
grep -q '"ok":true' <<<"$STATE_LATEST_OUT" || { echo "FAIL: state latest resp=$STATE_LATEST_OUT"; exit 1; }
grep -q "\"agent_id\":\"codex\"" <<<"$STATE_LATEST_OUT" || { echo "FAIL: state latest missing agent_id; resp=$STATE_LATEST_OUT"; exit 1; }

# state describe echoes the same state_id we got from the synced append.
STATE_DESCRIBE_OUT="$("$CTL" --sock "$SOCK" state describe "$STATE_ID")"
grep -q '"ok":true' <<<"$STATE_DESCRIBE_OUT" || { echo "FAIL: state describe resp=$STATE_DESCRIBE_OUT"; exit 1; }
grep -q "\"state_id\":\"$STATE_ID\"" <<<"$STATE_DESCRIBE_OUT" || { echo "FAIL: state describe missing state_id; resp=$STATE_DESCRIBE_OUT"; exit 1; }
grep -q '"payload_inline":"{\\"turn\\":2}"' <<<"$STATE_DESCRIBE_OUT" || { echo "FAIL: state describe did not round-trip JSON payload; resp=$STATE_DESCRIBE_OUT"; exit 1; }

# state restore (session mode) walks the chain and includes our synced state.
STATE_RESTORE_OUT="$("$CTL" --sock "$SOCK" state restore "$STATE_ID" --mode session)"
grep -q '"ok":true' <<<"$STATE_RESTORE_OUT" || { echo "FAIL: state restore resp=$STATE_RESTORE_OUT"; exit 1; }
grep -q "\"mode\":\"session\"" <<<"$STATE_RESTORE_OUT" || { echo "FAIL: state restore missing mode; resp=$STATE_RESTORE_OUT"; exit 1; }

# 11. branch merge conflict output preserves ] inside conflict paths
FAKE_SOCK="$ROOT/fake-control.sock"
MERGE_CONFLICT_OUT="$ROOT/merge-conflict.out"
MERGE_CONFLICT_ERR="$ROOT/merge-conflict.err"
FAKE_LISTENER_ERR="$ROOT/fake-control.err"
rm -f "$FAKE_SOCK"
python3 - "$FAKE_SOCK" > /dev/null 2>"$FAKE_LISTENER_ERR" <<'PY' &
import os
import socket
import sys

path = sys.argv[1]
response = b'{"ok":false,"error":"merge conflicts","conflicts":["/dir/a]b.txt"]}\n'

try:
    os.unlink(path)
except FileNotFoundError:
    pass

server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    server.bind(path)
    server.listen(1)
    server.settimeout(10.0)
    conn, _ = server.accept()
    with conn:
        conn.settimeout(2.0)
        try:
            while True:
                chunk = conn.recv(4096)
                if not chunk or chunk.endswith(b"\n"):
                    break
        except (TimeoutError, socket.timeout):
            pass
        conn.sendall(response)
finally:
    server.close()
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
PY
FAKE_LISTENER_PID=$!
for _ in $(seq 1 50); do
    if [[ -S "$FAKE_SOCK" ]]; then break; fi
    sleep 0.1
done
[[ -S "$FAKE_SOCK" ]] || { echo "FAIL: fake control socket not ready"; cat "$FAKE_LISTENER_ERR"; kill "$FAKE_LISTENER_PID" 2>/dev/null || true; wait "$FAKE_LISTENER_PID" 2>/dev/null || true; exit 1; }
set +e
"$CTL" --sock "$FAKE_SOCK" branch merge feature-cli --into main >"$MERGE_CONFLICT_OUT" 2>"$MERGE_CONFLICT_ERR"
RC=$?
set -e
kill "$FAKE_LISTENER_PID" 2>/dev/null || true
wait "$FAKE_LISTENER_PID" 2>/dev/null || true
[[ "$RC" -eq 1 ]] || { echo "FAIL: branch merge conflicts should exit 1 (got $RC)"; cat "$MERGE_CONFLICT_OUT" "$MERGE_CONFLICT_ERR"; exit 1; }
grep -qx "agentvfs-ctl: merge conflicts" "$MERGE_CONFLICT_ERR" || { echo "FAIL: branch merge conflict missing error"; cat "$MERGE_CONFLICT_ERR"; exit 1; }
grep -qx "  /dir/a]b.txt" "$MERGE_CONFLICT_ERR" || { echo "FAIL: branch merge conflict path with ] not preserved"; cat "$MERGE_CONFLICT_ERR"; exit 1; }
rm -f "$FAKE_SOCK" "$MERGE_CONFLICT_OUT" "$MERGE_CONFLICT_ERR" "$FAKE_LISTENER_ERR"

# 12. unknown subcommand exits non-zero
set +e
"$CTL" --sock "$SOCK" nonexistent >/dev/null 2>&1
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: unknown subcommand should exit non-zero (got $RC)"; exit 1; }

# 13. bad socket path exits with code 3
set +e
"$CTL" --sock /nonexistent/path status >/dev/null 2>&1
RC=$?
set -e
[[ "$RC" -eq 3 ]] || { echo "FAIL: bad socket should exit 3 (got $RC)"; exit 1; }

# 14. without -f, daemonization must not leave a dead listening socket
mkdir -p "$SRC_NOF" "$MNT_NOF"
echo "daemonized" > "$SRC_NOF/a.txt"
"$BIN" --source "$SRC_NOF" --mountpoint "$MNT_NOF" --store "$STORE_NOF" \
       --control-sock "$SOCK_NOF" --telemetry=none -s

for _ in $(seq 1 50); do
    if [[ -S "$SOCK_NOF" ]] && mountpoint -q "$MNT_NOF"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK_NOF" ]] || { echo "FAIL: no-f control socket not ready"; exit 1; }
mountpoint -q "$MNT_NOF" || { echo "FAIL: no-f mount not ready"; exit 1; }

NOF_STATUS="$(timeout 5 "$CTL" --sock "$SOCK_NOF" status)"
grep -q '"ok":true' <<<"$NOF_STATUS" || { echo "FAIL: no-f status resp=$NOF_STATUS"; exit 1; }
fusermount3 -u "$MNT_NOF"

echo "PASS test_agentvfs_ctl"
