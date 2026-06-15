#!/bin/bash
set -euo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"

CANDIDATES=(ebpf fanotify ptrace ldpreload bpftime)
OPS=(read write open close unlink rename truncate stat exec create)

if [[ ! -x "$BIN" ]]; then
    echo "SKIP test_cas_backend_compare: $BIN is not built"
    exit 0
fi

BIN_DIR="$(cd "$(dirname "$BIN")" && pwd)"
PRELOAD_LIB="${CAS_PRELOAD_LIB:-$BIN_DIR/libcas_preload.so}"

PYTHON_BIN=""
if command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="python3"
elif command -v python >/dev/null 2>&1 &&
     python -c 'import sys; raise SystemExit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
    PYTHON_BIN="python"
else
    echo "SKIP test_cas_backend_compare: Python 3 is required"
    exit 0
fi

HELP="$("$BIN" --help 2>&1 || true)"
BACKEND_LINE="$(grep -E "Backends:" <<<"$HELP" | head -n1 || true)"

backend_compiled() {
    local backend="$1"
    grep -Eq "(^|[[:space:],:])${backend}([[:space:],]|$)" <<<"$BACKEND_LINE"
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

strict_backend() {
    case "$1" in
        ebpf|fanotify|ptrace|ldpreload) return 0 ;;
        *) return 1 ;;
    esac
}

compiled=()
for backend in "${CANDIDATES[@]}"; do
    if backend_compiled "$backend"; then
        compiled+=("$backend")
    fi
done

if [[ "${#compiled[@]}" -eq 0 ]]; then
    echo "SKIP test_cas_backend_compare: no candidate telemetry backends compiled"
    exit 0
fi

runtime_skip_reason() {
    local backend="$1"
    case "$backend" in
        ldpreload)
            [[ -f "$PRELOAD_LIB" ]] || {
                echo "missing $PRELOAD_LIB"
                return 0
            }
            ;;
        fanotify)
            if [[ "$(id -u)" -ne 0 ]] || ! has_cap 21; then
                echo "requires root with CAP_SYS_ADMIN"
                return 0
            fi
            ;;
        ptrace)
            if ! ptrace_runtime_usable; then
                echo "requires x86_64 and ptrace attach permission"
                return 0
            fi
            ;;
        bpftime)
            if ! command -v bpftime >/dev/null 2>&1; then
                echo "bpftime runtime command is unavailable"
                return 0
            fi
            ;;
    esac
    return 1
}

fail() {
    echo "FAIL test_cas_backend_compare: $*"
    exit 1
}

TMP_PARENT="${TMPDIR:-/tmp}"
ROOT="$(mktemp -d "${TMP_PARENT%/}/agentvfs-backend-compare.XXXXXX")"
REPORT_DIR="$ROOT/reports"
mkdir -p "$REPORT_DIR"

ACTIVE_MNT=""
ACTIVE_RUN_DIR=""
ACTIVE_DAEMON_PID=""
ACTIVE_TARGET_PID=""
ACTIVE_TARGET_STOP=""
ACTIVE_CGROUP=""

unmount_mountpoint() {
    local mnt="$1"
    [[ -n "$mnt" ]] || return 0
    fusermount3 -u "$mnt" 2>/dev/null || \
        fusermount -u "$mnt" 2>/dev/null || \
        umount "$mnt" 2>/dev/null || true
}

cleanup_active() {
    set +e
    if [[ -n "$ACTIVE_TARGET_STOP" ]]; then
        touch "$ACTIVE_TARGET_STOP" 2>/dev/null || true
    fi
    if [[ -n "$ACTIVE_MNT" ]]; then
        unmount_mountpoint "$ACTIVE_MNT"
    fi
    if [[ -n "$ACTIVE_DAEMON_PID" ]]; then
        kill "$ACTIVE_DAEMON_PID" 2>/dev/null || true
        wait "$ACTIVE_DAEMON_PID" 2>/dev/null || true
    fi
    if [[ -n "$ACTIVE_TARGET_PID" ]]; then
        kill "$ACTIVE_TARGET_PID" 2>/dev/null || true
        wait "$ACTIVE_TARGET_PID" 2>/dev/null || true
    fi
    if [[ -n "$ACTIVE_MNT" ]]; then
        unmount_mountpoint "$ACTIVE_MNT"
    fi
    if [[ -n "$ACTIVE_CGROUP" ]]; then
        if ! rmdir "$ACTIVE_CGROUP" 2>/dev/null; then
            echo "WARN test_cas_backend_compare: leaving cgroup $ACTIVE_CGROUP" >&2
        fi
    fi
    if [[ -n "$ACTIVE_RUN_DIR" ]]; then
        if [[ -n "$ACTIVE_MNT" ]] && mountpoint -q "$ACTIVE_MNT"; then
            echo "WARN test_cas_backend_compare: leaving $ACTIVE_RUN_DIR because $ACTIVE_MNT is still mounted" >&2
        else
            rm -rf -- "$ACTIVE_RUN_DIR"
        fi
    fi
    ACTIVE_MNT=""
    ACTIVE_RUN_DIR=""
    ACTIVE_DAEMON_PID=""
    ACTIVE_TARGET_PID=""
    ACTIVE_TARGET_STOP=""
    ACTIVE_CGROUP=""
    set -e
}

control_request() {
    local sock="$1"
    local request="$2"

    "$PYTHON_BIN" - "$sock" "$request" <<'PY'
import socket
import sys

sock_path = sys.argv[1]
request = sys.argv[2]

try:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.settimeout(2.0)
        sock.connect(sock_path)
        sock.sendall(request.encode("utf-8") + b"\n")
        line = sock.makefile("r", encoding="utf-8").readline()
    if not line:
        raise RuntimeError("empty response")
    print(line.strip())
except Exception as exc:
    print(f"control request failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
}

json_bool_or_missing() {
    local json="$1"
    local field="$2"

    "$PYTHON_BIN" - "$json" "$field" <<'PY'
import json
import sys

try:
    data = json.loads(sys.argv[1])
except json.JSONDecodeError as exc:
    print(f"invalid JSON: {exc}", file=sys.stderr)
    sys.exit(1)

field = sys.argv[2]
if field not in data:
    print("missing")
    sys.exit(0)

value = data[field]
if not isinstance(value, bool):
    print(f"{field} is not boolean", file=sys.stderr)
    sys.exit(1)

print("true" if value else "false")
PY
}

create_ebpf_cgroup() {
    local root="/sys/fs/cgroup"
    if [[ ! -f "$root/cgroup.controllers" ]]; then
        echo "cgroup v2 root unavailable at $root"
        return 1
    fi

    local cg
    if ! cg="$(mktemp -d "$root/agentvfs-backend-compare.XXXXXX" 2>/dev/null)"; then
        echo "cannot create cgroup under $root"
        return 1
    fi
    echo "$cg"
}

prepare_ebpf_session() {
    local sock="$1"
    local status
    local available
    local cgroup_result
    local response
    local ok
    local register_available
    local session_id

    if ! status="$(control_request "$sock" "status")"; then
        fail "eBPF status query failed"
    fi
    if ! available="$(json_bool_or_missing "$status" "ebpf_available")"; then
        fail "invalid eBPF status response: $status"
    fi
    if [[ "$available" != "true" ]]; then
        echo "SKIP backend ebpf: ebpf_available=false"
        skipped+=("ebpf: ebpf_available=false")
        return 1
    fi

    if ! cgroup_result="$(create_ebpf_cgroup)"; then
        echo "SKIP backend ebpf: $cgroup_result"
        skipped+=("ebpf: $cgroup_result")
        return 1
    fi
    ACTIVE_CGROUP="$cgroup_result"

    session_id=$(( ($$ * 1000) + (${#tested[@]} + 1) ))
    if ! response="$(control_request "$sock" \
        "session.register {\"cgroup_path\":\"$ACTIVE_CGROUP\",\"session_id\":$session_id,\"telemetry_verbosity\":2}")"; then
        fail "eBPF session.register control request failed"
    fi
    if ! ok="$(json_bool_or_missing "$response" "ok")"; then
        fail "invalid eBPF session.register response: $response"
    fi
    if [[ "$ok" != "true" ]]; then
        fail "eBPF session.register failed after ebpf_available=true: $response"
    fi

    if ! register_available="$(json_bool_or_missing "$response" "ebpf_available")"; then
        fail "invalid eBPF session.register response: $response"
    fi
    if [[ "$register_available" == "false" ]]; then
        echo "SKIP backend ebpf: session.register reported ebpf_available=false"
        skipped+=("ebpf: session.register reported ebpf_available=false")
        return 1
    fi

    echo "backend ebpf: ebpf_available=true; registered $ACTIVE_CGROUP"
    return 0
}

cleanup_all() {
    cleanup_active
    if [[ -d "$ROOT" ]]; then
        if find "$ROOT" -type d -exec mountpoint -q {} \; -print -quit | grep -q .; then
            echo "WARN test_cas_backend_compare: leaving $ROOT because a mount is still active" >&2
        else
            rm -rf -- "$ROOT"
        fi
    fi
}
trap cleanup_all EXIT

write_workload_seed() {
    local src="$1"
    mkdir -p "$src"
    printf "seed\n" > "$src/read.txt"
    printf '#!/bin/sh\nexit 0\n' > "$src/exec.sh"
    chmod +x "$src/exec.sh"
}

run_python_workload() {
    local mnt="$1"
    local cgroup_path="${2:-}"
"$PYTHON_BIN" - "$mnt" "$cgroup_path" <<'PY'
import ctypes
import os
import sys

mnt = sys.argv[1]
cgroup_path = sys.argv[2]
read_path = os.path.join(mnt, "read.txt")
create_path = os.path.join(mnt, "created.txt")
rename_path = os.path.join(mnt, "renamed.txt")
exec_path = os.path.join(mnt, "exec.sh")

if cgroup_path:
    try:
        with open(os.path.join(cgroup_path, "cgroup.procs"), "w", encoding="ascii") as handle:
            handle.write(str(os.getpid()))
    except OSError as exc:
        print(f"cgroup move failed: {exc}", file=sys.stderr)
        raise SystemExit(77)

libc = ctypes.CDLL(None, use_errno=True)
libc.open.restype = ctypes.c_int
libc.read.restype = ctypes.c_ssize_t
libc.write.restype = ctypes.c_ssize_t
libc.close.restype = ctypes.c_int

def check_rc(rc, name, path):
    if rc == -1:
        err = ctypes.get_errno()
        raise OSError(err, f"{name} failed", path)
    return rc

fd = check_rc(libc.open(os.fsencode(read_path), os.O_RDONLY, 0),
              "open", read_path)
buf = ctypes.create_string_buffer(64)
try:
    check_rc(libc.read(fd, buf, len(buf)), "read", read_path)
finally:
    check_rc(libc.close(fd), "close", read_path)

fd = check_rc(libc.open(os.fsencode(create_path),
                        os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                        0o644), "open", create_path)
payload = b"write\n"
try:
    check_rc(libc.write(fd, payload, len(payload)), "write", create_path)
finally:
    check_rc(libc.close(fd), "close", create_path)

os.rename(create_path, rename_path)

if hasattr(libc, "stat"):
    stat_buf = ctypes.create_string_buffer(512)
    check_rc(libc.stat(os.fsencode(rename_path), ctypes.byref(stat_buf)),
             "stat", rename_path)
else:
    os.stat(rename_path)

if hasattr(libc, "truncate"):
    check_rc(libc.truncate(os.fsencode(rename_path), ctypes.c_long(1)),
             "truncate", rename_path)
else:
    os.truncate(rename_path, 1)

os.unlink(rename_path)
os.execv(exec_path, [exec_path])
PY
}

run_direct_workload() {
    local backend="$1"
    local mnt="$2"
    local preload_sock="$3"
    local cgroup_path="${4:-}"

    if [[ "$backend" == "ldpreload" ]]; then
        CAS_PRELOAD_SOCKET="$preload_sock" LD_PRELOAD="$PRELOAD_LIB" \
            run_python_workload "$mnt" "$cgroup_path"
    else
        run_python_workload "$mnt" "$cgroup_path"
    fi
}

start_ptrace_target() {
    local mnt="$1"
    local go_file="$2"
    local stop_file="$3"

"$PYTHON_BIN" - "$mnt" "$go_file" "$stop_file" <<'PY' &
import ctypes
import os
import sys
import time

mnt = sys.argv[1]
go_file = sys.argv[2]
stop_file = sys.argv[3]

while not os.path.exists(go_file):
    if os.path.exists(stop_file):
        raise SystemExit(0)
    time.sleep(0.05)

read_path = os.path.join(mnt, "read.txt")
create_path = os.path.join(mnt, "created.txt")
rename_path = os.path.join(mnt, "renamed.txt")
exec_path = os.path.join(mnt, "exec.sh")

libc = ctypes.CDLL(None, use_errno=True)
libc.open.restype = ctypes.c_int
libc.read.restype = ctypes.c_ssize_t
libc.write.restype = ctypes.c_ssize_t
libc.close.restype = ctypes.c_int

def check_rc(rc, name, path):
    if rc == -1:
        err = ctypes.get_errno()
        raise OSError(err, f"{name} failed", path)
    return rc

fd = check_rc(libc.open(os.fsencode(read_path), os.O_RDONLY, 0),
              "open", read_path)
buf = ctypes.create_string_buffer(64)
try:
    check_rc(libc.read(fd, buf, len(buf)), "read", read_path)
finally:
    check_rc(libc.close(fd), "close", read_path)

fd = check_rc(libc.open(os.fsencode(create_path),
                        os.O_WRONLY | os.O_CREAT | os.O_TRUNC,
                        0o644), "open", create_path)
payload = b"write\n"
try:
    check_rc(libc.write(fd, payload, len(payload)), "write", create_path)
finally:
    check_rc(libc.close(fd), "close", create_path)

os.rename(create_path, rename_path)

if hasattr(libc, "stat"):
    stat_buf = ctypes.create_string_buffer(512)
    check_rc(libc.stat(os.fsencode(rename_path), ctypes.byref(stat_buf)),
             "stat", rename_path)
else:
    os.stat(rename_path)

if hasattr(libc, "truncate"):
    check_rc(libc.truncate(os.fsencode(rename_path), ctypes.c_long(1)),
             "truncate", rename_path)
else:
    os.truncate(rename_path, 1)

os.unlink(rename_path)
os.execv(exec_path, [exec_path])
PY
    ACTIVE_TARGET_PID="$!"
}

declare -A EVENT_TOTAL
declare -A OP_COUNTS
tested=()
skipped=()

load_counts() {
    local backend="$1"
    local report="$2"
    local output
    if ! output="$("$PYTHON_BIN" - "$backend" "$report" "${OPS[@]}" <<'PY'
import json
import sys

backend = sys.argv[1]
path = sys.argv[2]
ops = sys.argv[3:]
counts = {op: 0 for op in ops}
total = 0

with open(path, "r", encoding="utf-8") as handle:
    for lineno, line in enumerate(handle, 1):
        line = line.strip()
        if not line:
            continue
        try:
            event = json.loads(line)
        except json.JSONDecodeError as exc:
            raise SystemExit(f"{path}:{lineno}: invalid JSON: {exc}")
        if event.get("backend") != backend:
            continue
        total += 1
        op = event.get("op")
        if op in counts:
            counts[op] += 1

print(total, *(counts[op] for op in ops))
PY
)"; then
        fail "$backend telemetry report could not be parsed"
    fi

    local stats=($output)
    EVENT_TOTAL["$backend"]="${stats[0]:-0}"
    local i
    for i in "${!OPS[@]}"; do
        OP_COUNTS["$backend:${OPS[$i]}"]="${stats[$((i + 1))]:-0}"
    done
}

print_backend_counts() {
    local backend="$1"
    echo "backend $backend: events=${EVENT_TOTAL[$backend]}"
    local op
    local line="  ops:"
    for op in "${OPS[@]}"; do
        line+=" $op=${OP_COUNTS[$backend:$op]}"
    done
    echo "$line"
}

run_backend() {
    local backend="$1"
    local run_dir="$ROOT/run-$backend"
    local src="$run_dir/src"
    local mnt="$run_dir/mnt"
    local store="$run_dir/store"
    local sock="$run_dir/control.sock"
    local preload_sock="$run_dir/cas_preload.sock"
    local go_file="$run_dir/target.go"
    local stop_file="$run_dir/target.stop"
    local daemon_log="$run_dir/daemon.log"
    local report="$REPORT_DIR/$backend.ndjson"

    mkdir -p "$run_dir" "$mnt"
    write_workload_seed "$src"

    ACTIVE_RUN_DIR="$run_dir"
    ACTIVE_MNT="$mnt"
    ACTIVE_TARGET_STOP="$stop_file"

    local -a daemon_args=(
        --source "$src"
        --mountpoint "$mnt"
        --store "$store"
        --control-sock "$sock"
        --telemetry="$backend"
    )

    case "$backend" in
        ldpreload)
            daemon_args+=("--telemetry-ldpreload-socket=$preload_sock")
            ;;
        ptrace)
            start_ptrace_target "$mnt" "$go_file" "$stop_file"
            daemon_args+=("--telemetry-ptrace-pids=$ACTIVE_TARGET_PID")
            ;;
        bpftime)
            daemon_args+=("--telemetry-bpftime-probes=$run_dir/bpftime-probes.json")
            ;;
    esac

    "$BIN" "${daemon_args[@]}" -f -s >"$daemon_log" 2>&1 &
    ACTIVE_DAEMON_PID="$!"

    local ready=0
    local i
    for i in $(seq 1 80); do
        if [[ -S "$sock" ]] && mountpoint -q "$mnt"; then
            ready=1
            break
        fi
        if ! kill -0 "$ACTIVE_DAEMON_PID" 2>/dev/null; then
            break
        fi
        sleep 0.1
    done

    if [[ "$ready" -ne 1 ]]; then
        local reason="daemon did not mount"
        if [[ -s "$daemon_log" ]]; then
            reason+=" ($(tail -n1 "$daemon_log"))"
        fi
        if strict_backend "$backend"; then
            fail "$backend startup failed: $reason"
        fi
        echo "SKIP backend $backend: $reason"
        skipped+=("$backend: $reason")
        cleanup_active
        return 0
    fi

    if [[ "$backend" == "ebpf" ]]; then
        if ! prepare_ebpf_session "$sock"; then
            cleanup_active
            return 0
        fi
    fi

    if [[ "$backend" == "ptrace" ]]; then
        touch "$go_file"
        if ! wait "$ACTIVE_TARGET_PID"; then
            fail "ptrace workload exited unsuccessfully"
        fi
        ACTIVE_TARGET_PID=""
    elif [[ "$backend" == "ebpf" ]]; then
        set +e
        run_direct_workload "$backend" "$mnt" "$preload_sock" "$ACTIVE_CGROUP"
        local workload_rc="$?"
        set -e
        if [[ "$workload_rc" -eq 77 ]]; then
            local reason="cannot move workload into cgroup $ACTIVE_CGROUP"
            echo "SKIP backend ebpf: $reason"
            skipped+=("ebpf: $reason")
            cleanup_active
            return 0
        fi
        if [[ "$workload_rc" -ne 0 ]]; then
            fail "eBPF workload exited with status $workload_rc"
        fi
    else
        run_direct_workload "$backend" "$mnt" "$preload_sock"
    fi

    shopt -s nullglob
    local -a telemetry=()
    local saw_backend=0
    for i in $(seq 1 50); do
        telemetry=("$store"/telemetry/*.ndjson)
        if [[ "${#telemetry[@]}" -gt 0 ]] &&
           grep -h -q "\"backend\":\"$backend\"" "${telemetry[@]}"; then
            saw_backend=1
            break
        fi
        sleep 0.1
    done

    telemetry=("$store"/telemetry/*.ndjson)
    if [[ "${#telemetry[@]}" -gt 0 ]]; then
        cat "${telemetry[@]}" > "$report"
    else
        : > "$report"
    fi

    load_counts "$backend" "$report"
    if [[ "${EVENT_TOTAL[$backend]}" -eq 0 || "$saw_backend" -ne 1 ]]; then
        local reason="no $backend events produced"
        if strict_backend "$backend"; then
            fail "$reason"
        fi
        echo "SKIP backend $backend: $reason"
        skipped+=("$backend: $reason")
        cleanup_active
        return 0
    fi

    tested+=("$backend")
    print_backend_counts "$backend"
    cleanup_active
}

for backend in "${compiled[@]}"; do
    if reason="$(runtime_skip_reason "$backend")"; then
        echo "SKIP backend $backend: $reason"
        skipped+=("$backend: $reason")
        continue
    fi
    run_backend "$backend"
done

if [[ "${#tested[@]}" -eq 0 ]]; then
    echo "SKIP test_cas_backend_compare: no usable compiled telemetry backends emitted events"
    if [[ "${#skipped[@]}" -gt 0 ]]; then
        printf "  skipped: %s\n" "${skipped[@]}"
    fi
    exit 0
fi

echo "coverage matrix:"
printf "%-10s" "op"
for backend in "${tested[@]}"; do
    printf " %10s" "$backend"
done
printf "\n"

for op in "${OPS[@]}"; do
    printf "%-10s" "$op"
    for backend in "${tested[@]}"; do
        printf " %10s" "${OP_COUNTS[$backend:$op]}"
    done
    printf "\n"
done

echo "PASS test_cas_backend_compare: tested ${tested[*]}"
