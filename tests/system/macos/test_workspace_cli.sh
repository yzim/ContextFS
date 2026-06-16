#!/bin/bash
set -euo pipefail

TEST_NAME="test_workspace_cli"

if [[ "$(uname)" != "Darwin" ]]; then
    echo "$TEST_NAME: skipping (not macOS)"
    exit 0
fi

if ! [[ -d /Library/Filesystems/fuse-t.fs ]]; then
    brew install --cask macos-fuse-t/cask/fuse-t
fi

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BIN_DIR="${BIN_DIR:-$REPO_ROOT/build}"
BIN="$BIN_DIR/agentvfs"
WS_ROOT="$(mktemp -d /tmp/agentvfs-workspace-macos.XXXXXX)"
# macOS /tmp is a symlink to /private/tmp; resolve once so mount(8)
# output and the paths reported by the daemon stay comparable.
WS_ROOT="$(cd "$WS_ROOT" && pwd -P)"
WS_NAME="macos-workspace-cli"
WS_MNT="$WS_ROOT/$WS_NAME/mount"
SEED_DIR="$WS_ROOT/seed"

fail() {
    echo "FAIL: $*"
    exit 1
}

note() {
    echo
    echo "== $* =="
}

get_kv() {
    local key="$1"
    local text="${2:-}"
    sed -n "s/^${key}=//p" <<<"$text"
}

expect_eq() {
    local actual="$1"
    local expected="$2"
    local label="$3"
    [[ "$actual" == "$expected" ]] || fail "$label: expected '$expected', got '$actual'"
}

expect_hex_hash() {
    local value="$1"
    local label="$2"
    [[ "$value" =~ ^[0-9a-f]{64}$ ]] || fail "$label: expected 64-char hex hash, got '$value'"
}

expect_started_listing() {
    local list_out="$1"
    if ! grep -q "^$WS_NAME[[:space:]]\\+started[[:space:]]\\+$WS_MNT\$" <<<"$list_out"; then
        fail "workspace list missing started session"
    fi
}

expect_stopped_listing() {
    local list_out="$1"
    if ! grep -q "^$WS_NAME[[:space:]]\\+stopped[[:space:]]\\+$WS_MNT\$" <<<"$list_out"; then
        fail "workspace list missing stopped session"
    fi
}

expect_mount_present() {
    if ! mount | grep -q " on $WS_MNT "; then
        fail "workspace mount missing"
    fi
}

expect_mount_absent() {
    if mount | grep -q " on $WS_MNT "; then
        fail "workspace mount still present after stop"
    fi
}

cleanup() {
    "$BIN" workspace stop "$WS_NAME" --root "$WS_ROOT" --no-checkpoint >/dev/null 2>&1 || true
    umount -f "$WS_MNT" 2>/dev/null || diskutil unmount force "$WS_MNT" 2>/dev/null || true
    rm -rf "$WS_ROOT"
}
trap cleanup EXIT

note "Initialize Workspace"
mkdir -p "$SEED_DIR"
echo "hello" > "$SEED_DIR/hello"
"$BIN" workspace init "$WS_NAME" --from "$SEED_DIR" --root "$WS_ROOT" >/dev/null

note "Start Workspace"
START_OUT="$("$BIN" workspace start "$WS_NAME" --root "$WS_ROOT")"
echo "$START_OUT"
WS_SOCK="$(get_kv socket "$START_OUT")"
WS_MNT_FROM_START="$(get_kv mount "$START_OUT")"
WS_STORE="$(get_kv store "$START_OUT")"
WS_STATUS="$(get_kv status "$START_OUT")"
WS_TELEMETRY="$(get_kv telemetry "$START_OUT")"

[[ -S "$WS_SOCK" ]] || fail "workspace socket missing: $WS_SOCK"
[[ -d "$WS_STORE" ]] || fail "workspace store missing: $WS_STORE"
expect_eq "$WS_MNT_FROM_START" "$WS_MNT" "workspace mount path mismatch"
expect_eq "$WS_STATUS" "started" "workspace status"
[[ "$WS_TELEMETRY" == "auto" || "$WS_TELEMETRY" == "none" ]] \
    || fail "unexpected workspace telemetry '$WS_TELEMETRY'"
expect_mount_present

note "Inspect Workspace"
STATUS_OUT="$("$BIN" workspace status "$WS_NAME" --root "$WS_ROOT")"
expect_eq "$(get_kv status "$STATUS_OUT")" "started" "workspace status command"
expect_eq "$(get_kv mount "$STATUS_OUT")" "$WS_MNT" "workspace status mount"

LIST_OUT="$("$BIN" workspace list --root "$WS_ROOT")"
expect_started_listing "$LIST_OUT"

note "Checkpoint And Rollback"
expect_eq "$(cat "$WS_MNT/hello")" "hello" "workspace bootstrap read"
echo "v1" > "$WS_MNT/data.txt"
FIRST_CP="$("$BIN" workspace checkpoint "$WS_NAME" baseline --root "$WS_ROOT")"
expect_hex_hash "$FIRST_CP" "workspace checkpoint"

echo "v2" > "$WS_MNT/data.txt"
ROLLBACK_OUT="$("$BIN" workspace rollback "$WS_NAME" baseline --root "$WS_ROOT")"
expect_hex_hash "$ROLLBACK_OUT" "workspace rollback response"
expect_eq "$(cat "$WS_MNT/data.txt")" "v1" "workspace rollback content"

note "Stop Workspace"
STOP_OUT="$("$BIN" workspace stop "$WS_NAME" --root "$WS_ROOT")"
echo "$STOP_OUT"
expect_eq "$(get_kv status "$STOP_OUT")" "stopped" "workspace stop response"
expect_mount_absent

LIST_AFTER_STOP="$("$BIN" workspace list --root "$WS_ROOT")"
expect_stopped_listing "$LIST_AFTER_STOP"

echo
echo "PASS $TEST_NAME"
