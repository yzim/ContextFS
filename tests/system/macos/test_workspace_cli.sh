#!/bin/bash
set -euo pipefail

if [[ "$(uname)" != "Darwin" ]]; then
    echo "test_workspace_cli: skipping (not macOS)"
    exit 0
fi

if ! [[ -d /Library/Filesystems/fuse-t.fs ]]; then
    brew install --cask macos-fuse-t/cask/fuse-t
fi

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BIN="$REPO_ROOT/build/agentvfs"
WS_ROOT="$(mktemp -d /tmp/agentvfs-workspace-macos.XXXXXX)"
WS_NAME="macos-workspace-cli"
WS_MNT="$WS_ROOT/$WS_NAME/mount"

cleanup() {
    "$BIN" workspace stop "$WS_NAME" --root "$WS_ROOT" --no-checkpoint >/dev/null 2>&1 || true
    umount -f "$WS_MNT" 2>/dev/null || diskutil unmount force "$WS_MNT" 2>/dev/null || true
    rm -rf "$WS_ROOT"
}
trap cleanup EXIT

START_OUT="$("$BIN" workspace start "$WS_NAME" --root "$WS_ROOT")"
echo "$START_OUT"
WS_SOCK="$(grep '^socket=' <<<"$START_OUT" | cut -d= -f2-)"
WS_MNT_FROM_START="$(grep '^mount=' <<<"$START_OUT" | cut -d= -f2-)"
WS_STORE="$(grep '^store=' <<<"$START_OUT" | cut -d= -f2-)"
WS_STATUS="$(grep '^status=' <<<"$START_OUT" | cut -d= -f2-)"
WS_TELEMETRY="$(grep '^telemetry=' <<<"$START_OUT" | cut -d= -f2-)"

[[ -S "$WS_SOCK" ]] || { echo "FAIL: workspace socket missing"; exit 1; }
[[ "$WS_MNT_FROM_START" == "$WS_MNT" ]] || { echo "FAIL: workspace mount path mismatch"; exit 1; }
[[ -d "$WS_STORE" ]] || { echo "FAIL: workspace store missing"; exit 1; }
[[ "$WS_STATUS" == "started" ]] || { echo "FAIL: workspace status not started"; exit 1; }
[[ "$WS_TELEMETRY" == "auto" || "$WS_TELEMETRY" == "none" ]] || {
    echo "FAIL: unexpected workspace telemetry '$WS_TELEMETRY'"
    exit 1
}
mount | grep -q " on $WS_MNT " || { echo "FAIL: workspace mount missing"; exit 1; }

STATUS_OUT="$("$BIN" workspace status "$WS_NAME" --root "$WS_ROOT")"
grep -q '^status=started$' <<<"$STATUS_OUT" || { echo "FAIL: workspace status command"; exit 1; }
grep -q "^mount=$WS_MNT\$" <<<"$STATUS_OUT" || { echo "FAIL: workspace status mount"; exit 1; }

LIST_OUT="$("$BIN" workspace list --root "$WS_ROOT")"
grep -q "^$WS_NAME[[:space:]]\\+started[[:space:]]\\+$WS_MNT\$" <<<"$LIST_OUT" || {
    echo "FAIL: workspace list missing started session"
    exit 1
}

[[ "$(cat "$WS_MNT/hello")" == "hello" ]] || { echo "FAIL: workspace bootstrap read"; exit 1; }
echo "v1" > "$WS_MNT/data.txt"
FIRST_CP="$("$BIN" workspace checkpoint "$WS_NAME" baseline --root "$WS_ROOT")"
[[ "$FIRST_CP" =~ ^[0-9a-f]{64}$ ]] || { echo "FAIL: workspace checkpoint"; exit 1; }

echo "v2" > "$WS_MNT/data.txt"
ROLLBACK_OUT="$("$BIN" workspace rollback "$WS_NAME" baseline --root "$WS_ROOT")"
grep -q '^rolled_back_to=' <<<"$ROLLBACK_OUT" || { echo "FAIL: workspace rollback response"; exit 1; }
[[ "$(cat "$WS_MNT/data.txt")" == "v1" ]] || { echo "FAIL: workspace rollback content"; exit 1; }

STOP_OUT="$("$BIN" workspace stop "$WS_NAME" --root "$WS_ROOT")"
grep -q '^stopped=true$' <<<"$STOP_OUT" || { echo "FAIL: workspace stop response"; exit 1; }
mount | grep -q " on $WS_MNT " && { echo "FAIL: workspace mount still present after stop"; exit 1; }

LIST_AFTER_STOP="$("$BIN" workspace list --root "$WS_ROOT")"
grep -q "^$WS_NAME[[:space:]]\\+stopped[[:space:]]\\+$WS_MNT\$" <<<"$LIST_AFTER_STOP" || {
    echo "FAIL: workspace list missing stopped session"
    exit 1
}

echo "PASS test_workspace_cli"
