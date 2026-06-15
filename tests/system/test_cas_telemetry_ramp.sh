#!/bin/bash
# tests/cas/test_cas_telemetry_ramp.sh — cumulative multi-backend telemetry ramp.
#
# For each K in 1..7, spawn agentvfs with the first K backends from the canonical
# order (sources before processors), run 5 checkpoints + reverse rollback, verify
# state and telemetry, tear down. Skips iterations whose required backend is not
# usable on this host (capability or runtime missing).
#
# NOTE: intentionally NOT using `set -e`. The script makes dozens of assertion
# calls; with `set -e` any non-zero return aborts the iteration, forcing `set +e`
# blocks around every check. `set -uo pipefail` gives us undefined-variable and
# pipeline-failure protection while letting `pass`/`fail` count outcomes freely.
set -uo pipefail

BIN="${BIN:-$(pwd)/build/agentvfs}"
PRELOAD_LIB="${PRELOAD_LIB:-$(pwd)/build/libcas_preload.so}"
ROOT="${ROOT:-/tmp/agentvfs-ramp}"
RAMP_LIMIT="${RAMP_LIMIT:-7}"   # stop after iteration N (dev/CI smoke)
RAMP_NEGATIVE="${RAMP_NEGATIVE:-0}"

PASS_TOTAL=0
FAIL_TOTAL=0
TESTS_RUN=0
TESTS_SKIPPED=0
declare -a SKIP_REASONS=()

ITER_PASS=0
ITER_FAIL=0

DAEMON_PID=""
WORKLOAD_PID=""
ITER_ROOT=""
ITER_MNT=""

cleanup_global() {
    teardown_iter 2>/dev/null || true
    rm -rf "$ROOT" 2>/dev/null || true
}
trap cleanup_global EXIT

pass() { echo "  PASS: $1"; PASS_TOTAL=$((PASS_TOTAL + 1)); ITER_PASS=$((ITER_PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL_TOTAL=$((FAIL_TOTAL + 1)); ITER_FAIL=$((ITER_FAIL + 1)); }
info() { echo "  INFO: $1"; }
skip() { echo "  SKIP: $1"; }

# Body of the script will be appended by later tasks.

# control SOCKPATH "command args" — sends one line, returns one response line.
control() {
    local sock="$1" cmd="$2"
    printf '%s\n' "$cmd" | nc -U -w 2 "$sock"
}

# wait_ready SOCKPATH MNTPATH TIMEOUT_S — polls until both socket and mount appear.
# Returns 0 on ready, 1 on timeout.
wait_ready() {
    local sock="$1" mnt="$2" timeout_s="${3:-10}"
    local deadline=$(( SECONDS + timeout_s ))
    while (( SECONDS < deadline )); do
        if [[ -S "$sock" ]] && mountpoint -q "$mnt"; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

backend_compiled() {
    local backend="$1" backend_line
    backend_line="$("$BIN" --help 2>&1 | grep -E '^[[:space:]]*Backends:' | head -n1 || true)"
    grep -Eq "(^|[[:space:],])${backend}([[:space:],]|$)" <<<"$backend_line"
}

has_cap() {
    local bit="$1" cap_eff
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

# Returns 0 if the named backend is usable on this host (compiled AND runtime preconditions met),
# 1 otherwise. Sets REASON on failure.
backend_usable() {
    local b="$1"
    REASON=""
    if ! backend_compiled "$b"; then
        REASON="$b not compiled in $BIN"
        return 1
    fi
    case "$b" in
        ebpf)
            # CAP_BPF (bit 39) OR root.
            if [[ "$(id -u)" -ne 0 ]] && ! has_cap 39; then
                REASON="ebpf requires root or CAP_BPF"
                return 1
            fi
            ;;
        fanotify)
            if [[ "$(id -u)" -ne 0 ]] || ! has_cap 21; then
                REASON="fanotify requires root with CAP_SYS_ADMIN"
                return 1
            fi
            ;;
        ptrace)
            if ! ptrace_runtime_usable; then
                REASON="ptrace requires x86_64 and ptrace attach permission"
                return 1
            fi
            ;;
        ldpreload)
            if [[ ! -f "$PRELOAD_LIB" ]]; then
                REASON="ldpreload missing $PRELOAD_LIB"
                return 1
            fi
            ;;
        bpftime|wasm|lua)
            # Stubs: compile presence is sufficient. They will report "start failed"
            # but the daemon stays up; check_health() accepts that.
            ;;
        *)
            REASON="unknown backend $b"
            return 1
            ;;
    esac
    return 0
}

# Canonical ramp order: sources first, processors after.
RAMP_ORDER=(ebpf fanotify ptrace ldpreload bpftime wasm lua)

# build_ramp populates RAMP_SUBSETS as 7 comma-separated lists, one per iteration.
# Each iteration K (1-indexed) is the first K elements of RAMP_ORDER joined by commas.
# Skips are decided per-iteration in main(), not here.
build_ramp() {
    RAMP_SUBSETS=()
    local i k entry subset
    for k in 1 2 3 4 5 6 7; do
        subset=""
        for ((i=0; i<k; i++)); do
            entry="${RAMP_ORDER[$i]}"
            if [[ -z "$subset" ]]; then
                subset="$entry"
            else
                subset="$subset,$entry"
            fi
        done
        RAMP_SUBSETS+=("$subset")
    done
}

# prepare_fixtures ROOT — create fixture files needed by stub backends.
prepare_fixtures() {
    local root="$1"
    # Trivial Lua pass-through script. Stub backend will reject it (start fails),
    # but the file must exist so the path is parseable.
    cat > "$root/policy.lua" <<'LUA'
-- Trivial pass-through; stub backend does not execute it.
function on_event(ev) return "allow" end
LUA
    : > "$root/empty.wasm"
    : > "$root/bpftime-probes"
}

# backend_args BACKEND ROOT WORKLOAD_PID — emits zero or more `--flag=value` tokens
# on stdout, one per line, for the named backend.
backend_args() {
    local b="$1" root="$2" wpid="$3"
    case "$b" in
        ebpf|fanotify) ;;
        ptrace)     printf '%s\n' "--telemetry-ptrace-pids=$wpid" ;;
        ldpreload)  printf '%s\n' "--telemetry-ldpreload-socket=$root/preload.sock" ;;
        bpftime)    printf '%s\n' "--telemetry-bpftime-probes=$root/bpftime-probes" ;;
        wasm)       printf '%s\n' "--telemetry-wasm-module=$root/empty.wasm" ;;
        lua)        printf '%s\n' "--telemetry-lua-script=$root/policy.lua" ;;
    esac
}

# setup_iter K — fresh ROOT directory tree for iteration K. Sets ITER_*ROOT vars.
setup_iter() {
    local k="$1"
    ITER_ROOT="$ROOT/iter-$k"
    ITER_SRC="$ITER_ROOT/src"
    ITER_MNT="$ITER_ROOT/mnt"
    ITER_STORE="$ITER_ROOT/store"
    ITER_SOCK="$ITER_ROOT/control.sock"
    ITER_LOG="$ITER_ROOT/iter-$k.log"

    rm -rf "$ITER_ROOT"
    mkdir -p "$ITER_SRC" "$ITER_MNT"
    echo "v0" > "$ITER_SRC/a.txt"
    echo "base" > "$ITER_SRC/stable.txt"

    prepare_fixtures "$ITER_ROOT"
}

# start_workload_writer — start a long-lived idle subshell whose pid we use for ptrace.
# The subshell does nothing but sleep; the test mutates files directly from the parent
# shell. The writer pid only needs to exist so ptrace has a target to attach to.
start_workload_writer() {
    ( exec sleep 600 ) &
    WORKLOAD_PID=$!
}

# start_daemon SUBSET — launch agentvfs with the requested backends + extras.
# Uses ITER_* paths and WORKLOAD_PID. Sets DAEMON_PID.
start_daemon() {
    local subset="$1"
    local -a cmd=(
        "$BIN"
        --source "$ITER_SRC"
        --mountpoint "$ITER_MNT"
        --store "$ITER_STORE"
        --control-sock "$ITER_SOCK"
        -f
        "--telemetry=$subset"
    )
    local b
    local -a names
    IFS=',' read -r -a names <<<"$subset"
    for b in "${names[@]}"; do
        while IFS= read -r flag; do
            [[ -n "$flag" ]] && cmd+=("$flag")
        done < <(backend_args "$b" "$ITER_ROOT" "$WORKLOAD_PID")
    done
    "${cmd[@]}" >"$ITER_LOG" 2>&1 &
    DAEMON_PID=$!
}

# expected_state BACKEND — echoes "started=true status=started" for source backends,
# "started=false status=start failed" for stub backends.
expected_state() {
    case "$1" in
        ebpf|fanotify|ptrace|ldpreload) echo 'started=true status=started' ;;
        bpftime|wasm|lua)               echo 'started=false status=start failed' ;;
        *)                              echo 'started=true status=started' ;;
    esac
}

# extract_backend_block JSON BACKEND — echoes the JSON object for that backend or empty.
# Tolerates unknown ordering of fields. Uses sed/grep only — no jq dependency.
extract_backend_block() {
    local json="$1" b="$2"
    grep -oE '\{[^{}]*"name":"'"$b"'"[^{}]*\}' <<<"$json" | head -n1
}

check_health() {
    local subset="$1" json b started status expected exp_started exp_status
    local -a names
    json="$(control "$ITER_SOCK" "telemetry.status" || true)"
    if [[ -z "$json" ]] || ! grep -q '"backends"' <<<"$json"; then
        fail "telemetry.status returned no backends payload"
        return
    fi
    IFS=',' read -r -a names <<<"$subset"
    for b in "${names[@]}"; do
        local block; block="$(extract_backend_block "$json" "$b")"
        if [[ -z "$block" ]]; then
            fail "telemetry.status: backend $b not present"
            continue
        fi
        started="$(grep -oE '"started":(true|false)' <<<"$block" | head -n1 | cut -d: -f2)"
        status="$(grep -oE '"status":"[^"]*"' <<<"$block" | head -n1 | sed -E 's/"status":"(.*)"/\1/')"
        expected="$(expected_state "$b")"
        exp_started="${expected#started=}"; exp_started="${exp_started%% *}"
        exp_status="${expected#*status=}"
        if [[ "$started" == "$exp_started" && "$status" == "$exp_status" ]]; then
            pass "telemetry.status: $b started=$started status=$status"
        else
            fail "telemetry.status: $b expected started=$exp_started status='$exp_status', got started=$started status='$status'"
        fi
    done
}

# do_workload — performs 5 mutations + checkpoints; populates COMMITS[1..5].
# Reads ITER_MNT, ITER_SOCK. Reports per-checkpoint pass/fail.
do_workload() {
    COMMITS=()
    local i resp
    for i in 1 2 3 4 5; do
        echo "v$i" > "$ITER_MNT/a.txt"
        echo "file-$i-content" > "$ITER_MNT/file_$i.txt"
        resp="$(control "$ITER_SOCK" "checkpoint cp$i")"
        if grep -q '"ok":true' <<<"$resp"; then
            local hash
            hash="$(sed -E 's/.*"commit":"([^"]+)".*/\1/' <<<"$resp")"
            COMMITS[$i]="$hash"
            pass "checkpoint cp$i (${hash:0:12}…)"
        else
            COMMITS[$i]=""
            fail "checkpoint cp$i resp=$resp"
        fi
    done
}

# check_distinct — asserts: 5 non-empty hashes, all distinct, refs/main equals cp5 hash.
check_distinct() {
    local i missing=0
    for i in 1 2 3 4 5; do
        [[ -z "${COMMITS[$i]:-}" ]] && missing=1
    done
    if (( missing )); then
        fail "checkpoint hashes incomplete: ${COMMITS[*]:-}"
        return
    fi

    local distinct
    distinct="$(printf '%s\n' "${COMMITS[1]}" "${COMMITS[2]}" "${COMMITS[3]}" "${COMMITS[4]}" "${COMMITS[5]}" | sort -u | wc -l)"
    if [[ "$distinct" == "5" ]]; then
        pass "5 checkpoint hashes distinct"
    else
        fail "checkpoint hashes not distinct (unique=$distinct)"
    fi

    local refs_file="$ITER_STORE/refs/main"
    if [[ -r "$refs_file" ]]; then
        local refs_main; refs_main="$(cat "$refs_file")"
        if [[ "$refs_main" == "${COMMITS[5]}" ]]; then
            pass "refs/main == cp5 hash"
        else
            fail "refs/main=$refs_main but cp5=${COMMITS[5]}"
        fi
    else
        fail "refs/main not readable at $refs_file"
    fi
}

# check_post_workload_state — assert files have expected content after 5 cps.
check_post_workload_state() {
    local val
    val="$(cat "$ITER_MNT/a.txt" 2>/dev/null || true)"
    [[ "$val" == "v5" ]] && pass "a.txt == v5" || fail "a.txt expected v5 got '$val'"
    val="$(cat "$ITER_MNT/stable.txt" 2>/dev/null || true)"
    [[ "$val" == "base" ]] && pass "stable.txt == base" || fail "stable.txt expected base got '$val'"
    local i
    for i in 1 2 3 4 5; do
        val="$(cat "$ITER_MNT/file_$i.txt" 2>/dev/null || true)"
        if [[ "$val" == "file-$i-content" ]]; then
            pass "file_$i.txt present"
        else
            fail "file_$i.txt expected 'file-$i-content' got '$val'"
        fi
    done
}

# rollback_to LABEL EXPECTED_A_VAL EXPECTED_PRESENT EXPECTED_ABSENT
# EXPECTED_PRESENT and EXPECTED_ABSENT are space-separated lists of file basenames.
rollback_to() {
    local label="$1" exp_a="$2" present="$3" absent="$4"
    local resp
    resp="$(control "$ITER_SOCK" "rollback $label")"
    if grep -q '"ok":true' <<<"$resp"; then
        pass "rollback $label accepted"
    else
        fail "rollback $label resp=$resp"
        return
    fi

    local val; val="$(cat "$ITER_MNT/a.txt" 2>/dev/null || true)"
    [[ "$val" == "$exp_a" ]] && pass "rollback $label: a.txt == $exp_a" \
                              || fail "rollback $label: a.txt expected $exp_a got '$val'"

    local f
    for f in $present; do
        if [[ -e "$ITER_MNT/$f" ]]; then
            pass "rollback $label: $f present"
        else
            fail "rollback $label: $f missing"
        fi
    done
    for f in $absent; do
        if [[ ! -e "$ITER_MNT/$f" ]]; then
            pass "rollback $label: $f absent"
        else
            fail "rollback $label: $f unexpectedly present"
        fi
    done
}

rollback_reverse() {
    rollback_to cp4 v4 "file_1.txt file_2.txt file_3.txt file_4.txt" "file_5.txt"
    rollback_to cp3 v3 "file_1.txt file_2.txt file_3.txt"            "file_4.txt file_5.txt"
    rollback_to cp2 v2 "file_1.txt file_2.txt"                       "file_3.txt file_4.txt file_5.txt"
    rollback_to cp1 v1 "file_1.txt"                                  "file_2.txt file_3.txt file_4.txt file_5.txt"
}

# check_telemetry SUBSET — assert the session NDJSON exists; count "backend":"<b>"
# lines per requested backend. All counts are reported as INFO: event emission
# requires per-backend setup that this test does not model (ebpf needs
# session.register + cgroup membership, ldpreload needs LD_PRELOAD on the
# workload, ptrace needs a writer that actually does file ops). The presence of
# the NDJSON file itself is the only hard requirement; per-backend event
# coverage lives in test_cas_backend_compare.sh.
check_telemetry() {
    local subset="$1"
    shopt -s nullglob
    local files=("$ITER_STORE"/telemetry/*.ndjson)
    shopt -u nullglob
    if (( ${#files[@]} == 0 )); then
        fail "no NDJSON files under $ITER_STORE/telemetry/"
        return
    fi
    pass "telemetry: NDJSON file exists (${#files[@]})"

    local b count
    local -a names
    IFS=',' read -r -a names <<<"$subset"
    for b in "${names[@]}"; do
        count="$(grep -h "\"backend\":\"$b\"" "${files[@]}" 2>/dev/null | wc -l)"
        info "telemetry: $b emitted $count event(s)"
    done
}

check_negative() {
    [[ "$RAMP_NEGATIVE" != "1" ]] && return
    local resp
    resp="$(control "$ITER_SOCK" "rollback nonexistent_cp")"
    if grep -q '"ok":false' <<<"$resp"; then
        pass "negative rollback rejected"
    else
        fail "negative rollback unexpectedly accepted: $resp"
    fi
}

summary() {
    echo
    echo "=== Summary ==="
    echo "  Tests run:     $TESTS_RUN"
    echo "  Tests skipped: $TESTS_SKIPPED"
    if (( TESTS_SKIPPED > 0 )); then
        local r
        for r in "${SKIP_REASONS[@]}"; do
            echo "    - $r"
        done
    fi
    echo "  Total PASS:    $PASS_TOTAL"
    echo "  Total FAIL:    $FAIL_TOTAL"
}

# stop_iter — unmount, kill daemon and workload writer, but keep ITER_ROOT files.
# Used to freeze the telemetry NDJSON before check_telemetry reads it; otherwise
# fanotify (which uses FAN_ACCESS_PERM permission events) keeps appending while
# we read, racing grep.
stop_iter() {
    [[ -n "${ITER_MNT:-}" ]] && fusermount3 -u "$ITER_MNT" 2>/dev/null || true
    if [[ -n "${DAEMON_PID:-}" ]] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -TERM "$DAEMON_PID" 2>/dev/null || true
        local i
        for i in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -KILL "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    if [[ -n "${WORKLOAD_PID:-}" ]] && kill -0 "$WORKLOAD_PID" 2>/dev/null; then
        kill -TERM "$WORKLOAD_PID" 2>/dev/null || true
        wait "$WORKLOAD_PID" 2>/dev/null || true
    fi
    DAEMON_PID=""
    WORKLOAD_PID=""
}

# teardown_iter — stop_iter + rm -rf ITER_ROOT. Idempotent.
teardown_iter() {
    stop_iter
    [[ -n "${ITER_ROOT:-}" ]] && rm -rf "$ITER_ROOT"
    ITER_ROOT=""
    ITER_MNT=""
}

preflight() {
    if [[ ! -x "$BIN" ]]; then
        echo "preflight FAIL: $BIN not executable"
        return 2
    fi
    if ! command -v fusermount3 >/dev/null; then
        echo "preflight FAIL: fusermount3 not found"
        return 2
    fi
    if ! command -v nc >/dev/null; then
        echo "preflight FAIL: nc (netcat) not found"
        return 2
    fi

    # Best-effort: clear a stale mount from a prior crashed run.
    fusermount3 -u "$ROOT/mnt" 2>/dev/null || true

    local backend_line
    backend_line="$("$BIN" --help 2>&1 | grep -E '^[[:space:]]*Backends:' | head -n1 || true)"
    echo "preflight: BIN=$BIN"
    echo "preflight: $backend_line"

    local b
    for b in ebpf fanotify ptrace ldpreload bpftime wasm lua; do
        if backend_usable "$b"; then
            echo "preflight: $b usable"
        else
            echo "preflight: $b unusable ($REASON)"
        fi
    done
    return 0
}

main() {
    preflight || exit $?
    (( RAMP_LIMIT == 0 )) && { echo "RAMP_LIMIT=0: preflight only, exiting."; exit 0; }

    build_ramp
    if [[ "${RAMP_DEBUG:-0}" == "1" ]]; then
        local k subset b
        local -a names
        for k in 1 2 3 4 5 6 7; do
            (( k > RAMP_LIMIT )) && break
            subset="${RAMP_SUBSETS[$((k-1))]}"
            echo "iter $k: --telemetry=$subset"
            IFS=',' read -r -a names <<<"$subset"
            for b in "${names[@]}"; do
                while IFS= read -r line; do
                    [[ -n "$line" ]] && echo "  extra: $line"
                done < <(backend_args "$b" "/tmp/agentvfs-ramp" "12345")
            done
        done
        exit 0
    fi

    mkdir -p "$ROOT"

    local k subset
    for k in 1 2 3 4 5 6 7; do
        (( k > RAMP_LIMIT )) && break
        subset="${RAMP_SUBSETS[$((k-1))]}"
        echo
        echo "=== Test $k/7: $subset ==="

        # Skip-gate: if any backend in this subset is unusable, skip the iteration.
        local b unusable=""
        local -a names
        IFS=',' read -r -a names <<<"$subset"
        for b in "${names[@]}"; do
            if ! backend_usable "$b"; then
                unusable="$REASON"
                break
            fi
        done
        if [[ -n "$unusable" ]]; then
            skip "$unusable"
            echo "  → Test $k: SKIPPED"
            TESTS_SKIPPED=$((TESTS_SKIPPED + 1))
            SKIP_REASONS+=("test $k: $unusable")
            continue
        fi

        ITER_PASS=0
        ITER_FAIL=0
        setup_iter "$k"
        start_workload_writer
        start_daemon "$subset"

        if wait_ready "$ITER_SOCK" "$ITER_MNT" 10; then
            pass "daemon ready (sock+mount)"
        else
            fail "daemon failed to become ready"
            tail -n 50 "$ITER_LOG" 2>/dev/null | sed 's/^/    /'
            teardown_iter
            TESTS_RUN=$((TESTS_RUN + 1))
            echo "  → Test $k: $ITER_PASS PASS / $ITER_FAIL FAIL"
            continue
        fi

        check_health "$subset"
        do_workload
        check_distinct
        check_post_workload_state
        rollback_reverse
        check_negative

        # Stop the daemon BEFORE reading the NDJSON. Source backends (notably
        # fanotify, which uses FAN_*_PERM permission events) keep appending to
        # the file as long as the daemon is alive; grep races them otherwise.
        stop_iter
        check_telemetry "$subset"

        local iter_failed=$ITER_FAIL
        if (( iter_failed > 0 )); then
            echo "  --- last 50 lines of $ITER_LOG ---"
            tail -n 50 "$ITER_LOG" 2>/dev/null | sed 's/^/    /'
            echo "  --- end log ---"
        fi

        teardown_iter
        TESTS_RUN=$((TESTS_RUN + 1))
        echo "  → Test $k: $ITER_PASS PASS / $iter_failed FAIL"
    done

    summary
    if (( FAIL_TOTAL > 0 )); then
        exit 1
    fi
    exit 0
}

main "$@"
