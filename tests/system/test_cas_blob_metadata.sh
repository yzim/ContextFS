#!/bin/bash
set -euo pipefail

ROOT="${1:-/tmp/agentvfs-blob-metadata}"
MNT="$ROOT/mnt"
STORE="$ROOT/store"
SOCK="$ROOT/control.sock"
BIN="$(pwd)/build/agentvfs"

cleanup() {
    fusermount3 -u "$MNT" 2>/dev/null || true
    kill %1 2>/dev/null || true
    wait 2>/dev/null || true
    rm -rf "$ROOT"
}
trap cleanup EXIT

rm -rf "$ROOT"
mkdir -p "$MNT"

# Empty source: blob.txt is created via the mount, not bootstrapped.
"$BIN" --source "$ROOT" --mountpoint "$MNT" --store "$STORE" \
       --control-sock "$SOCK" -f -s &
FUSE_PID=$!

for _ in $(seq 1 50); do
    if [[ -S "$SOCK" ]] && mountpoint -q "$MNT"; then break; fi
    sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: control socket not ready"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: mount not ready"; exit 1; }

# --- ZERO_HASH blob entries must read as empty files, not EIO ---
# Force write_blob to fail by making the store tmp dir unwritable; cas_create
# then records a ZERO_HASH blob entry in the working tree. A read-capable open
# of that entry must serve EOF like the overlay path (getattr already reports
# size 0), not EIO from an invalid retained blob fd. Delete any already-stored
# empty blob object first: object_exists checks the filesystem, so without it
# write_blob must attempt a tmp write and fail.
python3 - "$STORE" <<'PY'
import pathlib, sys
objects = pathlib.Path(sys.argv[1]) / "objects"
empty = b"blob" + (0).to_bytes(8, "little")
for path in objects.glob("*/*"):
    if path.read_bytes() == empty:
        path.unlink()
PY
chmod 555 "$STORE/tmp"
: > "$MNT/zero.txt"
chmod 755 "$STORE/tmp"

size=$(stat -c %s "$MNT/zero.txt")
[[ "$size" == 0 ]] || { echo "FAIL: zero-hash entry size=$size"; exit 1; }
set +e
out=$(cat "$MNT/zero.txt" 2>"$ROOT/zero.err")
rc=$?
set -e
[[ "$rc" -eq 0 && -z "$out" ]] || {
    echo "FAIL: reading zero-hash entry rc=$rc out=${#out}B"
    cat "$ROOT/zero.err"
    exit 1
}

# Write a blob whose payload is "metadata-payload" (16 bytes). On disk it is
# laid out as b"blob" + size_le(16) + b"metadata-payload".
printf 'metadata-payload' > "$MNT/blob.txt"

# Locate that blob object and corrupt its header by truncating the file to
# just b"blob" (4 bytes), leaving no size field and no payload. A subsequent
# metadata read must surface EIO rather than silently reporting size 0.
python3 - "$STORE" <<'PY'
import pathlib, sys
store = pathlib.Path(sys.argv[1]) / "objects"
matches = []
for path in store.glob("*/*"):
    data = path.read_bytes()
    if data[:4] == b"blob" and data[12:] == b"metadata-payload":
        matches.append(path)
assert len(matches) == 1, matches
matches[0].chmod(0o644)
matches[0].write_bytes(b"blob")
PY

set +e
stat "$MNT/blob.txt" >"$ROOT/stat.out" 2>"$ROOT/stat.err"
rc=$?
set -e

[[ "$rc" -ne 0 ]] || { echo "FAIL: stat succeeded on corrupt blob"; exit 1; }
grep -qi 'Input/output error' "$ROOT/stat.err" || {
    echo "FAIL: stat did not report EIO; stderr:"; cat "$ROOT/stat.err"; exit 1;
}

echo "PASS test_cas_blob_metadata"
