#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"
PRELOAD_LIB="${CAS_PRELOAD_LIB:-$(pwd)/build/libcas_preload.so}"

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_multi_backend: $BIN is not built"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
BACKEND_LINE="$(grep -E "Backends:" <<<"$HELP" | head -n1 || true)"

backend_compiled() {
    local backend="$1"
    grep -Eq "(^|[[:space:],])${backend}([[:space:],]|$)" <<<"$BACKEND_LINE"
}

has_cap() {
    local bit="$1"
    local cap_eff
    cap_eff="$(awk '/^CapEff:/ {print $2}' /proc/self/status 2>/dev/null || true)"
    [[ -n "$cap_eff" ]] && (( (16#$cap_eff & (1 << bit)) != 0 ))
}

ptrace_runtime_usable() {
    [[ "$(uname -m)" == "x86_64" ]] || return 1
    if [[ -r /proc/sys/kernel/yama/ptrace_scope ]]; then
        local scope
        scope="$(cat /proc/sys/kernel/yama/ptrace_scope)"
        if [[ "$scope" != "0" ]] && ! has_cap 19; then
            return 1
        fi
    fi
    return 0
}

compiled=()
usable=()
skip_reasons=()

for backend in fanotify ptrace ldpreload bpftime wasm lua ebpf; do
    if backend_compiled "$backend"; then
        compiled+=("$backend")
    fi
done

if backend_compiled ldpreload; then
    if [[ -f "$PRELOAD_LIB" ]]; then
        usable+=("ldpreload")
    else
        skip_reasons+=("ldpreload missing $PRELOAD_LIB")
    fi
fi

if backend_compiled fanotify; then
    if [[ "$(id -u)" -eq 0 ]] && has_cap 21; then
        usable+=("fanotify")
    else
        skip_reasons+=("fanotify requires root with CAP_SYS_ADMIN")
    fi
fi

if backend_compiled ptrace; then
    if ptrace_runtime_usable; then
        usable+=("ptrace")
    else
        skip_reasons+=("ptrace requires x86_64 and ptrace attach permission")
    fi
fi

usable_has() {
    local backend="$1"
    local item
    for item in "${usable[@]}"; do
        [[ "$item" == "$backend" ]] && return 0
    done
    return 1
}

SELECTED=()
if usable_has ldpreload && usable_has fanotify; then
    SELECTED=("ldpreload" "fanotify")
elif usable_has ldpreload && usable_has ptrace; then
    SELECTED=("ldpreload" "ptrace")
elif usable_has fanotify && usable_has ptrace; then
    SELECTED=("fanotify" "ptrace")
fi

if [[ "${#SELECTED[@]}" -lt 2 ]]; then
    compiled_display="${compiled[*]:-none}"
    usable_display="${usable[*]:-none}"
    reason_display="${skip_reasons[*]:-no two emitter backends available}"
    echo "SKIP test_cas_multi_backend: need two usable emitter backends (compiled: $compiled_display; usable: $usable_display; $reason_display)"
    exit 0
fi

selected_has() {
    local backend="$1"
    local item
    for item in "${SELECTED[@]}"; do
        [[ "$item" == "$backend" ]] && return 0
    done
    return 1
}

join_csv() {
    local IFS=,
    echo "$*"
}

BACKENDS_CSV="$(join_csv "${SELECTED[@]}")"

fail() {
    echo "FAIL test_cas_multi_backend: $*"
    exit 1
}

TMP_PARENT="${TMPDIR:-/tmp}"
ROOT_OWNED=0
if [[ "$#" -gt 0 ]]; then
    ROOT="$1"
    if [[ -e "$ROOT" ]]; then
        fail "explicit test root already exists: $ROOT"
    fi
    mkdir "$ROOT"
    ROOT_OWNED=1
else
    ROOT="$(mktemp -d "${TMP_PARENT%/}/agentvfs-multi-backend.XXXXXX")"
    ROOT_OWNED=1
fi

SRC="$ROOT/src"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
CONTROL_SOCK="$ROOT/control.sock"
PRELOAD_SOCK="$ROOT/cas_preload.sock"
TARGET_GO="$ROOT/target.go"
TARGET_DONE="$ROOT/target.done"
TARGET_STOP="$ROOT/target.stop"
TARGET_PID=""
DAEMON_PID=""

unmount_mountpoint() {
    fusermount3 -u "$MNT" 2>/dev/null || \
        fusermount -u "$MNT" 2>/dev/null || \
        umount "$MNT" 2>/dev/null || true
}

remove_owned_root() {
    [[ "$ROOT_OWNED" -eq 1 ]] || return 0
    if [[ -d "$MNT" ]] && mountpoint -q "$MNT"; then
        echo "WARN test_cas_multi_backend: leaving $ROOT because $MNT is still mounted" >&2
        return 0
    fi
    rm -rf -- "$ROOT"
}

cleanup() {
    set +e
    touch "$TARGET_STOP" 2>/dev/null || true
    unmount_mountpoint
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
    fi
    if [[ -n "$TARGET_PID" ]]; then
        kill "$TARGET_PID" 2>/dev/null || true
    fi
    wait 2>/dev/null || true
    unmount_mountpoint
    remove_owned_root
}
trap cleanup EXIT

mkdir -p "$SRC" "$MNT"
echo "seed" > "$SRC/read.txt"

query_telemetry_status() {
    local python_bin=""
    if command -v python3 >/dev/null 2>&1; then
        python_bin="python3"
    elif command -v python >/dev/null 2>&1 &&
         python -c 'import sys; raise SystemExit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
        python_bin="python"
    else
        fail "telemetry.status check requires Python 3"
    fi

    "$python_bin" - "$CONTROL_SOCK" <<'PY'
import json
import socket
import sys

sock_path = sys.argv[1]
try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(2.0)
        sock.connect(sock_path)
        sock.sendall(b"telemetry.status\n")
        line = sock.makefile("r", encoding="utf-8").readline()
    if not line:
        raise RuntimeError("empty response")
    data = json.loads(line)
    if not isinstance(data.get("backends"), list):
        raise RuntimeError("response missing backends array")
    print(line.strip())
except Exception as exc:
    print(f"telemetry.status query failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
}

start_ptrace_target() {
    local -a env_cmd=(env)
    if selected_has ldpreload; then
        env_cmd+=("CAS_PRELOAD_SOCKET=$PRELOAD_SOCK" "LD_PRELOAD=$PRELOAD_LIB")
    fi

    "${env_cmd[@]}" bash -c '
        set -euo pipefail
        mnt="$1"
        go="$2"
        done_file="$3"
        stop_file="$4"
        while [[ ! -e "$go" ]]; do
            sleep 0.05
        done
        exec 3< "$mnt/read.txt"
        IFS= read -r _ <&3 || true
        exec 3<&-
        printf "write\n" > "$mnt/write.txt"
        printf "append\n" >> "$mnt/write.txt"
        : > "$mnt/truncated.txt"
        [[ -e "$mnt/write.txt" ]] || true
        touch "$done_file"
        while [[ ! -e "$stop_file" ]]; do
            sleep 0.1
        done
    ' _ "$MNT" "$TARGET_GO" "$TARGET_DONE" "$TARGET_STOP" &
    TARGET_PID="$!"
}

run_direct_workload() {
    local -a env_cmd=(env)
    if selected_has ldpreload; then
        env_cmd+=("CAS_PRELOAD_SOCKET=$PRELOAD_SOCK" "LD_PRELOAD=$PRELOAD_LIB")
    fi

    "${env_cmd[@]}" bash -c '
        set -euo pipefail
        mnt="$1"
        exec 3< "$mnt/read.txt"
        IFS= read -r _ <&3 || true
        exec 3<&-
        printf "write\n" > "$mnt/write.txt"
        printf "append\n" >> "$mnt/write.txt"
        : > "$mnt/truncated.txt"
        [[ -e "$mnt/write.txt" ]] || true
    ' _ "$MNT"
}

daemon_args=(
    --source "$SRC"
    --mountpoint "$MNT"
    --store "$STORE"
    --control-sock "$CONTROL_SOCK"
    --telemetry="$BACKENDS_CSV"
)

if selected_has ldpreload; then
    daemon_args+=("--telemetry-ldpreload-socket=$PRELOAD_SOCK")
fi

if selected_has ptrace; then
    start_ptrace_target
    daemon_args+=("--telemetry-ptrace-pids=$TARGET_PID")
fi

"$BIN" "${daemon_args[@]}" -f -s &
DAEMON_PID="$!"

for _ in $(seq 1 80); do
    if [[ -S "$CONTROL_SOCK" ]] && mountpoint -q "$MNT"; then
        break
    fi
    if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
        wait "$DAEMON_PID" 2>/dev/null || true
        fail "daemon exited before mount became ready"
    fi
    sleep 0.1
done

if ! mountpoint -q "$MNT"; then
    fail "mount did not become ready"
fi

if selected_has ptrace; then
    touch "$TARGET_GO"
    for _ in $(seq 1 80); do
        [[ -e "$TARGET_DONE" ]] && break
        sleep 0.1
    done
    [[ -e "$TARGET_DONE" ]] || fail "ptrace target process did not complete activity"
else
    run_direct_workload
fi

events_ready() {
    shopt -s nullglob
    local telemetry=("$STORE"/telemetry/*.ndjson)
    [[ "${#telemetry[@]}" -gt 0 ]] || return 1

    local backend
    for backend in "${SELECTED[@]}"; do
        grep -h -q "\"backend\":\"$backend\"" "${telemetry[@]}" || return 1
    done
    return 0
}

for _ in $(seq 1 30); do
    events_ready && break
    sleep 0.1
done

shopt -s nullglob
telemetry=("$STORE"/telemetry/*.ndjson)
if [[ "${#telemetry[@]}" -eq 0 ]]; then
    fail "no telemetry files found"
fi

event_count="$(cat "${telemetry[@]}" | wc -l | tr -d ' ')"
if [[ "$event_count" -le 0 ]]; then
    fail "telemetry files are empty"
fi

for backend in "${SELECTED[@]}"; do
    grep -h -q "\"backend\":\"$backend\"" "${telemetry[@]}" \
        || fail "no $backend events"
done

if ! STATUS="$(query_telemetry_status)"; then
    fail "telemetry.status query failed"
fi
for backend in "${SELECTED[@]}"; do
    grep -q "\"name\":\"$backend\"" <<<"$STATUS" \
        || fail "telemetry.status missing $backend: $STATUS"
done
echo "PASS test_cas_multi_backend: telemetry.status reports $BACKENDS_CSV"

echo "PASS test_cas_multi_backend: $BACKENDS_CSV produced $event_count events"
