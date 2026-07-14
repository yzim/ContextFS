#!/bin/bash
# System test 10 (mem-and-gc): GC live-mount age-fence race.
#
# Launches a mounted agentvfs filesystem, runs a concurrent writer that
# continuously creates/overwrites files, and fires gc.run + gc.verify in a
# loop. Proves:
#   1. GC never eats data the writer produced (age fence + live-root
#      marking under the daemon's GC locks).
#   2. gc.verify stays ok=true across every round.
#   3. No surviving writer file is empty or unreadable (no eaten blobs).
#
# FUSE-only (no cgroup, no session routing) → runs non-root via the
# setuid fusermount3 helper. Modeled on tests/system/test_cas_branching.sh.
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-gc-live}"
SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
WORK="$ROOT/work"
BIN="$(pwd)/build/agentvfs"

DAEMON_PID=""
WRITER_PID=""

cleanup() {
    # Signal the writer to stop first so it doesn't race the unmount.
    touch "$WORK/stop" 2>/dev/null || true
    if [[ -n "$WRITER_PID" ]]; then
        kill "$WRITER_PID" 2>/dev/null || true
        wait "$WRITER_PID" 2>/dev/null || true
        WRITER_PID=""
    fi
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
        DAEMON_PID=""
    fi
    rm -f "$SOCK"
    rm -rf "$ROOT"
}
trap cleanup EXIT

# ctl <command-line>: dependency-free raw control-socket client. Opens a
# fresh AF_UNIX connection, sends one newline-terminated command, reads one
# JSON response line, and prints it. Exits non-zero if the response is not
# valid JSON (so callers can tell protocol errors apart).
ctl() {
    python3 - "$SOCK" "$1" <<'PY'
import json, socket, sys
sock_path, line = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    s.connect(sock_path)
    s.sendall((line + "\n").encode())
    buf = b""
    while not buf.endswith(b"\n"):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
finally:
    s.close()
resp = buf.decode().strip()
try:
    json.loads(resp)
except Exception as e:
    sys.stderr.write("ctl: malformed JSON for %r: %s (%s)\n" % (line, resp, e))
    sys.exit(2)
print(resp)
PY
}

# ok_true <json-str>: bash predicate that parses one JSON line and returns 0
# iff obj["ok"] is true.
ok_true() {
    python3 - "$1" <<'PY'
import json, sys
print("yes" if json.loads(sys.argv[1]).get("ok") else "no")
PY
}

require_ok() {
    local operation="$1"
    local response="$2"
    if [[ "$(ok_true "$response")" != "yes" ]]; then
        echo "FAIL: test_cas_gc_live: $operation not ok: $response"
        exit 1
    fi
}

json_uint() {
    python3 - "$1" "$2" <<'PY'
import json, sys
value = json.loads(sys.argv[1]).get(sys.argv[2])
if isinstance(value, bool) or not isinstance(value, int) or value < 0:
    sys.stderr.write("invalid non-negative integer %s=%r\n" % (sys.argv[2], value))
    sys.exit(2)
print(value)
PY
}

# ── Setup ────────────────────────────────────────────────────────────
rm -rf "$ROOT"
mkdir -p "$SRC" "$MNT" "$WORK"
echo "seed" > "$SRC/seed.txt"

# Launch the daemon in foreground mode with FUSE's default multithreaded loop,
# so writes can race a GC request instead of being serialized by -s.
"$BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f &
DAEMON_PID=$!
mounted=0
for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then mounted=1; break; fi
    sleep 0.1
done
if [[ "$mounted" -ne 1 ]]; then
    echo "FAIL: test_cas_gc_live: daemon did not expose the control socket / mount in time"
    exit 1
fi

# Publish two versions before checkpointing. The first blob becomes an orphan,
# and checkpoint drains pending_ without retaining it. Age it beyond the
# two-second fence so a live-writer GC round must perform a real deletion.
echo "orphan-v1" > "$MNT/gc-orphan-seed.txt"
echo "orphan-v2" > "$MNT/gc-orphan-seed.txt"
base_resp="$(ctl "checkpoint gc-live-base")"
require_ok "checkpoint gc-live-base" "$base_resp"
sleep 3

# ── Concurrent writer loop ───────────────────────────────────────────
# Continuously creates/overwrites 50 writer files through the FUSE mount
# until a $WORK/stop flag appears. Each redirection goes through the FUSE
# create/write/release path, exercising the exact blob-publish path that
# GC's age fence + live-root marking must protect.
(
    attempts=0
    successes=0
    errors=0
    while [[ ! -f "$WORK/stop" ]]; do
        if echo "w$attempts" > "$MNT/writer-$((attempts % 50)).txt"; then
            successes=$((successes + 1))
        else
            errors=$((errors + 1))
        fi
        attempts=$((attempts + 1))
    done
    echo "$attempts" > "$WORK/writer-attempts"
    echo "$successes" > "$WORK/writer-successes"
    echo "$errors" > "$WORK/writer-errors"
    [[ "$errors" -eq 0 ]]
) &
WRITER_PID=$!

# ── Five rounds of GC + verify under live writes ─────────────────────
total_swept_objects=0
for round in 1 2 3 4 5; do
    sleep 1
    gresp="$(ctl "gc.run")"
    echo "$gresp" > "$WORK/gc-$round.json"
    require_ok "gc.run round $round" "$gresp"
    swept_objects="$(json_uint "$gresp" swept_objects)"
    total_swept_objects=$((total_swept_objects + swept_objects))
    vresp="$(ctl "gc.verify")"
    echo "$vresp" > "$WORK/verify-$round.json"
    require_ok "gc.verify round $round" "$vresp"
    echo "  round $round gc.run:   $gresp"
    echo "  round $round gc.verify: $vresp"
done
if [[ "$total_swept_objects" -le 0 ]]; then
    echo "FAIL: test_cas_gc_live: live GC rounds swept no objects"
    exit 1
fi

# ── Stop the writer and let it record its count ──────────────────────
touch "$WORK/stop"
if wait "$WRITER_PID"; then
    writer_status=0
else
    writer_status=$?
fi
WRITER_PID=""

writer_attempts="$(cat "$WORK/writer-attempts" 2>/dev/null || echo invalid)"
writer_successes="$(cat "$WORK/writer-successes" 2>/dev/null || echo invalid)"
writer_errors="$(cat "$WORK/writer-errors" 2>/dev/null || echo invalid)"
if [[ ! "$writer_attempts" =~ ^[0-9]+$ ||
      ! "$writer_successes" =~ ^[0-9]+$ ||
      ! "$writer_errors" =~ ^[0-9]+$ ]]; then
    echo "FAIL: test_cas_gc_live: writer did not record valid counters"
    exit 1
fi
if [[ "$writer_status" -ne 0 || "$writer_errors" -ne 0 ]]; then
    echo "FAIL: test_cas_gc_live: writer status=$writer_status, errors=$writer_errors"
    exit 1
fi
if [[ "$writer_successes" -le 0 ]]; then
    echo "FAIL: test_cas_gc_live: writer completed no successful writes"
    exit 1
fi

# ── Assert no eaten blobs: every writer file non-empty and readable ──
eaten=0
checked=0
shopt -s nullglob
for f in "$MNT"/writer-*.txt; do
    checked=$((checked + 1))
    if [[ ! -s "$f" ]]; then
        echo "FAIL: test_cas_gc_live: empty (eaten) blob survived: $f"
        eaten=$((eaten + 1))
        continue
    fi
    if ! cat "$f" >/dev/null 2>&1; then
        echo "FAIL: test_cas_gc_live: unreadable blob: $f"
        eaten=$((eaten + 1))
    fi
done
shopt -u nullglob

if [[ "$eaten" -ne 0 ]]; then
    echo "FAIL: test_cas_gc_live: $eaten eaten/unreadable blobs of $checked checked"
    exit 1
fi
if [[ "$checked" -le 0 ]]; then
    echo "FAIL: test_cas_gc_live: no writer files were checked"
    exit 1
fi

# ── Final checkpoint + verify ────────────────────────────────────────
final_cp_resp="$(ctl "checkpoint gc-live-final")"
require_ok "checkpoint gc-live-final" "$final_cp_resp"
fresp="$(ctl "gc.verify")"
echo "$fresp" > "$WORK/verify-final.json"
require_ok "final gc.verify" "$fresp"

echo "writer attempts/successes/errors: $writer_attempts/$writer_successes/$writer_errors"
echo "gc.run swept objects: $total_swept_objects"
echo "writer files checked: $checked (all non-empty + readable)"
echo "PASS: test_cas_gc_live"
