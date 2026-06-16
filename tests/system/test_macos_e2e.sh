#!/bin/bash
# macOS end-to-end: install fuse-t (idempotent), mount, write,
# checkpoint, modify, rollback, assert, unmount.

set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "test_macos_e2e: skipping (not macOS)"
    exit 0
fi

if ! [[ -d /Library/Filesystems/fuse-t.fs ]]; then
    brew install --cask macos-fuse-t/cask/fuse-t
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WORK="$(mktemp -d /tmp/agentvfs-e2e.XXXXXX)"
# macOS /tmp is a symlink to /private/tmp; mktemp returns the /tmp path
# but mount(8) reports the canonical /private/tmp form. Resolve once up
# front so SRC / MNT comparisons against `mount` output match.
WORK="$(cd "$WORK" && pwd -P)"
SRC="$WORK/src"; MNT="$WORK/mnt"; SOCK="$WORK/ctl.sock"; LOG="$WORK/daemon.log"
BIN="$REPO_ROOT/build/agentvfs"
CTL="$REPO_ROOT/build/agentvfs-ctl"

mkdir -p "$SRC" "$MNT"
echo "hello" > "$SRC/greeting.txt"

DAEMON_PID=""
cleanup() {
    if [[ -n "$DAEMON_PID" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -TERM "$DAEMON_PID" || true
        for _ in $(seq 1 40); do
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.25
        done
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
    fi
    umount -f "$MNT" 2>/dev/null || diskutil unmount force "$MNT" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

"$BIN" --source "$SRC" --mountpoint "$MNT" --sock "$SOCK" > "$LOG" 2>&1 &
DAEMON_PID=$!

# Wait up to 30s for mount + control socket. fuse-t's NFS-loopback init
# is noticeably slower than libfuse3's /dev/fuse handshake, especially
# on a cold CI runner.
for _ in $(seq 1 120); do
    if [[ -S "$SOCK" ]] && mount | grep -q " on $MNT "; then break; fi
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        echo "FAIL: daemon exited before mount"
        echo "--- daemon log ---"; cat "$LOG"
        exit 1
    fi
    sleep 0.25
done
if ! mount | grep -q " on $MNT "; then
    echo "FAIL: mount missing"
    echo "--- expected mountpoint ---"; echo "$MNT"
    echo "--- mount(8) output ---"; mount
    echo "--- daemon log ---"; cat "$LOG"
    exit 1
fi
[[ -S "$SOCK" ]] || { echo "FAIL: control socket missing"; exit 1; }

[[ "$(cat "$MNT/greeting.txt")" == "hello" ]] \
    || { echo "FAIL: bootstrap read"; exit 1; }

echo "v1" > "$MNT/data.txt"
[[ "$(cat "$MNT/data.txt")" == "v1" ]] \
    || { echo "FAIL: post-write read"; exit 1; }

"$CTL" --sock "$SOCK" checkpoint baseline \
    || { echo "FAIL: checkpoint"; exit 1; }

echo "v2" > "$MNT/data.txt"
"$CTL" --sock "$SOCK" rollback baseline \
    || { echo "FAIL: rollback"; exit 1; }

ROLLBACK_CONTENT="$(cat "$MNT/data.txt")"
if [[ "$ROLLBACK_CONTENT" != "v1" ]]; then
    echo "FAIL: rollback content: got '$ROLLBACK_CONTENT'"
    echo "--- stat after rollback ---"; stat "$MNT/data.txt" || true
    echo "--- ctl status ---";          "$CTL" --sock "$SOCK" status || true
    echo "--- daemon log ---";          cat "$LOG"
    exit 1
fi

kill -TERM "$DAEMON_PID"
for _ in $(seq 1 40); do
    kill -0 "$DAEMON_PID" 2>/dev/null || break
    sleep 0.25
done
kill -0 "$DAEMON_PID" 2>/dev/null \
    && { echo "FAIL: daemon didn't exit on SIGTERM"; exit 1; }
mount | grep -q " on $MNT " \
    && { echo "FAIL: mount still present after shutdown"; exit 1; }

echo "PASS test_macos_e2e"
