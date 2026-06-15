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
