#!/bin/bash
set -euo pipefail

LABEL="${1:?usage: run.sh <label> <agentvfs-bin> <bench-bin> [work-root]}"
AGENTVFS_BIN="$(realpath "${2:?missing agentvfs binary}")"
BENCH_BIN="$(realpath "${3:?missing benchmark binary}")"
WORK_ROOT="${4:-/tmp/agentvfs-fuse-io-$USER}"
DIRS="${FUSE_IO_DIRS:-100}"
FILES_PER_DIR="${FUSE_IO_FILES_PER_DIR:-100}"
LARGE_BYTES="${FUSE_IO_LARGE_BYTES:-1073741824}"
REPS="${FUSE_IO_REPS:-7}"
ITER_DIV="${FUSE_IO_ITER_DIV:-1}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ).$$"
OUT="$SCRIPT_DIR/results/$STAMP"
SRC="$WORK_ROOT/src"
MNT="$WORK_ROOT/mnt"
STORE="$WORK_ROOT/store"
SOCK="$WORK_ROOT/control.sock"
JSONL="$OUT/samples.jsonl"
DAEMON_PID=""
CGROUP_PATH="/sys/fs/cgroup/agentvfs-fuse-io-$$"

iters() { echo $(( $1 / ITER_DIV > 0 ? $1 / ITER_DIV : 1 )); }

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    if [[ -n "$DAEMON_PID" ]]; then
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rmdir "$CGROUP_PATH" 2>/dev/null || true
    rm -rf "$WORK_ROOT"
}
trap cleanup EXIT

rm -rf "$WORK_ROOT"
mkdir -p "$SRC/data" "$SRC/.bench-write" "$MNT" "$OUT"
python3 - "$SRC" "$DIRS" "$FILES_PER_DIR" "$LARGE_BYTES" <<'PY'
import pathlib, sys
root = pathlib.Path(sys.argv[1])
dirs, files_per_dir, large_bytes = (int(value) for value in sys.argv[2:5])
for directory in range(dirs):
    d = root / "data" / f"d{directory:03d}"
    d.mkdir(parents=True)
    for item in range(files_per_dir):
        index = directory * files_per_dir + item
        block = (f"file-{index:05d}\n".encode() * 512)[:4096]
        (d / f"f{index:05d}.dat").write_bytes(block.ljust(4096, b"x"))
with open(root / "large.bin", "wb") as stream:
    stream.truncate(large_bytes)
(root / ".gitignore").write_text(".bench-write/\nlarge.bin\n", encoding="ascii")
PY
git -C "$SRC" init -q
git -C "$SRC" config user.name benchmark
git -C "$SRC" config user.email benchmark@example.invalid
git -C "$SRC" add data .gitignore
git -C "$SRC" commit -qm fixture

"$AGENTVFS_BIN" --source "$SRC" --mountpoint "$MNT" --store "$STORE" \
    --control-sock "$SOCK" -f >"$OUT/daemon.log" 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 100); do
    [[ -S "$SOCK" ]] && mountpoint -q "$MNT" && break
    sleep 0.1
done
[[ -S "$SOCK" ]] && mountpoint -q "$MNT" || {
    echo "daemon failed to mount" >&2
    exit 1
}

append_json() {
    local line="$1" target="$2"
    python3 - "$line" "$LABEL" "$target" >>"$JSONL" <<'PY'
import json, sys
row = json.loads(sys.argv[1])
row.update(label=sys.argv[2], target=sys.argv[3])
print(json.dumps(row, sort_keys=True))
PY
}

append_case() {
    local target="$1" root="$2" case_name="$3" iterations="$4"
    local line
    line=$("$BENCH_BIN" "$case_name" "$root" "$iterations")
    append_json "$line" "$target"
}

append_git_status() {
    local target="$1" root="$2" line
    line=$(python3 - "$root" <<'PY'
import json, os, subprocess, sys, time
env = dict(os.environ, GIT_OPTIONAL_LOCKS="0")
start = time.monotonic_ns()
subprocess.run(["git", "-C", sys.argv[1], "status", "--porcelain",
                "--untracked-files=all"], check=True,
               stdout=subprocess.DEVNULL, env=env)
elapsed = time.monotonic_ns() - start
print(json.dumps({"case": "git-status", "elapsed_ns": elapsed,
                  "operations": 1, "ops_per_second": 1e9 / elapsed}))
PY
)
    append_json "$line" "$target"
}

warmup() {
    local root="$1"
    "$BENCH_BIN" stat-existing "$root" 100 >/dev/null
    "$BENCH_BIN" lookup-missing "$root" 100 >/dev/null
    "$BENCH_BIN" open-close "$root" 100 >/dev/null
    "$BENCH_BIN" tree-walk "$root" 1 >/dev/null
    "$BENCH_BIN" read-small "$root" 100 >/dev/null
    "$BENCH_BIN" seq-read "$root" 1 >/dev/null
    "$BENCH_BIN" random-read "$root" 100 >/dev/null
    "$BENCH_BIN" create-write-close "$root" 10 >/dev/null
    "$BENCH_BIN" create-unlink "$root" 10 >/dev/null
    GIT_OPTIONAL_LOCKS=0 git -C "$root" status --porcelain \
        --untracked-files=all >/dev/null
}
warmup "$SRC"
warmup "$MNT"
for repetition in $(seq 1 "$REPS"); do
    for target in plain fuse; do
        [[ "$target" == plain ]] && root="$SRC" || root="$MNT"
        append_case "$target" "$root" stat-existing "$(iters 100000)"
        append_case "$target" "$root" lookup-missing "$(iters 100000)"
        append_case "$target" "$root" open-close "$(iters 20000)"
        append_case "$target" "$root" tree-walk 1
        append_case "$target" "$root" read-small "$(iters 20000)"
        append_git_status "$target" "$root"
        append_case "$target" "$root" seq-read 1
        append_case "$target" "$root" random-read "$(iters 10000)"
        append_case "$target" "$root" create-write-close "$(iters 5000)"
        append_case "$target" "$root" create-unlink "$(iters 5000)"
    done
done

CGROUP_STATUS="SKIP: requires root and writable cgroup v2"
CTL_BIN="$(dirname "$AGENTVFS_BIN")/agentvfs-ctl"
if [[ $EUID -eq 0 ]] && mkdir "$CGROUP_PATH" 2>/dev/null; then
    "$CTL_BIN" --sock "$SOCK" branch create bench-routed >/dev/null
    "$CTL_BIN" --sock "$SOCK" session register --cgroup "$CGROUP_PATH" \
        --id 7001 --branch bench-routed >/dev/null
    for repetition in $(seq 1 "$REPS"); do
        (
            echo $BASHPID > "$CGROUP_PATH/cgroup.procs"
            append_case fuse-routed "$MNT" stat-existing "$(iters 100000)"
            append_case fuse-routed "$MNT" read-small "$(iters 20000)"
        )
    done
    "$CTL_BIN" --sock "$SOCK" session unregister --cgroup "$CGROUP_PATH" >/dev/null
    "$CTL_BIN" --sock "$SOCK" branch delete bench-routed >/dev/null
    rmdir "$CGROUP_PATH"
    CGROUP_STATUS="PASS"
fi

python3 - "$JSONL" "$OUT/results.json" "$LABEL" "$AGENTVFS_BIN" \
    "$CGROUP_STATUS" "$((DIRS * FILES_PER_DIR))" "$LARGE_BYTES" \
    "$REPS" "$ITER_DIV" <<'PY'
import json, pathlib, platform, subprocess, sys
rows = [json.loads(line) for line in pathlib.Path(sys.argv[1]).read_text().splitlines()]
for case in ("read-small", "seq-read", "random-read"):
    plain = {row["checksum"] for row in rows
             if row["target"] == "plain" and row["case"] == case}
    fuse = {row["checksum"] for row in rows
            if row["target"] == "fuse" and row["case"] == case}
    assert len(plain) == 1 and plain == fuse, (case, plain, fuse)
    routed = {row["checksum"] for row in rows
              if row["target"] == "fuse-routed" and row["case"] == case}
    assert not routed or routed == plain, (case, plain, routed)
cpuinfo = pathlib.Path("/proc/cpuinfo").read_text()
cpu = platform.machine()
for line in cpuinfo.splitlines():
    if line.startswith("model name"):
        cpu = line.split(":", 1)[1].strip()
        break
metadata = {
    "label": sys.argv[3],
    "agentvfs_binary": sys.argv[4],
    "git_commit": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    "kernel": platform.release(),
    "libfuse": subprocess.check_output(["pkg-config", "--modversion", "fuse3"], text=True).strip(),
    "cpu": cpu,
    "fixture_files": int(sys.argv[6]),
    "fixture_small_file_bytes": 4096,
    "fixture_large_file_bytes": int(sys.argv[7]),
    "repetitions": int(sys.argv[8]),
    "iteration_divisor": int(sys.argv[9]),
    "cgroup_routed_guardrail": sys.argv[5],
}
pathlib.Path(sys.argv[2]).write_text(
    json.dumps({"metadata": metadata, "samples": rows}, indent=2) + "\n")
PY
python3 "$SCRIPT_DIR/report.py" summarize "$OUT/results.json" \
    --output "$OUT/report.md"
printf '%s\n' "$OUT"
