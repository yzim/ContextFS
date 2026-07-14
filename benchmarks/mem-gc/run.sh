#!/bin/bash
# Usage: run.sh <after-agentvfs-bin> [before-agentvfs-bin]
#               [--g4-ratio R] [--allow-skip]
# Env: MEMGC_FILES (default 100000), MEMGC_DIRS (10000), MEMGC_KMAX (64)   -- S1
#      MEMGC_S2_FILES (20000), MEMGC_S2_DIRS (2000), MEMGC_CYCLES (100)    -- S2
#      MEMGC_S3_STEPS (40)                                                 -- S3 (>=24)
# Extra args after the two binaries are forwarded to report.py (e.g.
# --g4-ratio <R>, computed from benchmarks/fuse-io stat-existing after/before).
# Full runs are strict by default; --allow-skip is only for reduced smoke runs.
set -euo pipefail
AFTER_BIN="$(realpath "${1:?usage: run.sh <after-bin> [before-bin] [report options]}")"
shift
BEFORE_BIN=""
if [[ $# -gt 0 && "$1" != --* ]]; then
    BEFORE_BIN="$(realpath "$1")"
    shift
fi
REPORT_ARGS=("$@")
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ).$$"
OUT="$SCRIPT_DIR/results/$STAMP"
mkdir -p "$OUT"
FILES="${MEMGC_FILES:-100000}"; DIRS="${MEMGC_DIRS:-10000}"; KMAX="${MEMGC_KMAX:-64}"

run_s1() {  # $1=bin $2=label
    python3 "$SCRIPT_DIR/s1_fanout.py" "$1" "$2" "$OUT/s1-$2.csv" "$FILES" "$DIRS" "$KMAX"
}
run_s1 "$AFTER_BIN" after
[[ -n "$BEFORE_BIN" ]] && run_s1 "$BEFORE_BIN" before
# merge per-label CSVs into s1.csv (header once)
{ head -1 "$OUT/s1-after.csv"; tail -q -n+2 "$OUT"/s1-*.csv; } > "$OUT/s1.csv"

S2_FILES="${MEMGC_S2_FILES:-20000}"; S2_DIRS="${MEMGC_S2_DIRS:-2000}"; CYCLES="${MEMGC_CYCLES:-100}"
run_s2() {  # $1=bin $2=label
    python3 "$SCRIPT_DIR/s2_churn.py" "$1" "$2" "$OUT/s2-$2.csv" "$S2_FILES" "$S2_DIRS" "$CYCLES"
}
run_s2 "$AFTER_BIN" after
[[ -n "$BEFORE_BIN" ]] && run_s2 "$BEFORE_BIN" before
# merge per-label CSVs into s2.csv (header once)
{ head -1 "$OUT/s2-after.csv"; tail -q -n+2 "$OUT"/s2-*.csv; } > "$OUT/s2.csv"

# S3: store growth / orphan share / GC reclamation / retention (gate G3).
# gc.* commands exist only on the after build; before rows carry store_bytes
# growth only (s3_gc.py gates the gc assertions on label == "after").
S3_STEPS="${MEMGC_S3_STEPS:-40}"
run_s3() {  # $1=bin $2=label
    python3 "$SCRIPT_DIR/s3_gc.py" "$1" "$2" "$OUT" "$S3_STEPS"
}
run_s3 "$AFTER_BIN" after
[[ -n "$BEFORE_BIN" ]] && run_s3 "$BEFORE_BIN" before
# merge per-label CSVs into s3.csv (header once)
{ head -1 "$OUT/s3-after.csv"; tail -q -n+2 "$OUT"/s3-*.csv; } > "$OUT/s3.csv"

# Forward extra CLI args (e.g. --g4-ratio <R> computed from fuse-io) to report.
python3 "$SCRIPT_DIR/report.py" "$OUT" "${REPORT_ARGS[@]}"
echo "results: $OUT"
