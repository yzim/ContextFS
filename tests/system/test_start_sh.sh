#!/bin/bash
set -euo pipefail

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$REPO/start.sh"
BIN="${BIN:-$REPO/build/agentvfs}"

WORK="$(mktemp -d)"
export HOME="$WORK/home"
export XDG_RUNTIME_DIR="$WORK/xdg"
export PATH="$REPO/build:$PATH"
mkdir -p "$HOME" "$XDG_RUNTIME_DIR"

cleanup() {
    for base in "$XDG_RUNTIME_DIR/agentvfs" "$WORK/custom-root" "$WORK/env-root"; do
        [[ -d "$base" ]] || continue
        for mnt in "$base"/*/mount; do
            [[ -d "$mnt" ]] || continue
            mountpoint -q "$mnt" && fusermount3 -u "$mnt" 2>/dev/null || true
        done
    done
    rm -rf "$WORK"
}
trap cleanup EXIT

# --- Test: no argument ---
set +e
"$SCRIPT" >"$WORK/no-arg.out" 2>"$WORK/no-arg.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: missing arg should exit non-zero"; exit 1; }
grep -qi "usage" "$WORK/no-arg.err" || { echo "FAIL: missing usage hint"; cat "$WORK/no-arg.err"; exit 1; }

# --- Test: non-existent path ---
set +e
"$SCRIPT" "$WORK/nonesuch" >"$WORK/missing.out" 2>"$WORK/missing.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: missing path should exit non-zero"; exit 1; }

# --- Test: path is a file, not a directory ---
touch "$WORK/afile"
set +e
"$SCRIPT" "$WORK/afile" >"$WORK/file.out" 2>"$WORK/file.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: file (not dir) should exit non-zero"; exit 1; }

# --- Test: agentvfs binary not on PATH ---
mkdir -p "$WORK/proj"
PATH="/usr/bin:/bin" "$SCRIPT" "$WORK/proj" >"$WORK/nobin.out" 2>"$WORK/nobin.err" && {
    echo "FAIL: missing agentvfs should exit non-zero"; exit 1;
} || true
grep -q "agentvfs not found" "$WORK/nobin.err" || {
    echo "FAIL: missing agentvfs hint not on stderr"; cat "$WORK/nobin.err"; exit 1;
}

# --- Test: invalid basename (contains a space) ---
mkdir -p "$WORK/has space"
set +e
"$SCRIPT" "$WORK/has space" >"$WORK/badname.out" 2>"$WORK/badname.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: invalid name should exit non-zero"; exit 1; }
grep -q "invalid workspace name" "$WORK/badname.err" || {
    echo "FAIL: missing invalid-name message"; cat "$WORK/badname.err"; exit 1;
}

# --- Test: fresh path triggers init, registers workspace ---
mkdir -p "$WORK/freshproj"
echo "alpha" > "$WORK/freshproj/file.txt"
"$SCRIPT" "$WORK/freshproj" >"$WORK/init1.out" 2>"$WORK/init1.err"
[[ -d "$XDG_RUNTIME_DIR/agentvfs/freshproj/source" ]] || {
    echo "FAIL: freshproj workspace dir not created"
    ls -la "$XDG_RUNTIME_DIR/agentvfs" 2>/dev/null || true
    exit 1
}
[[ -f "$XDG_RUNTIME_DIR/agentvfs/freshproj/source/file.txt" ]] || {
    echo "FAIL: source not seeded from --from"
    exit 1
}

# --- Test: re-run skips init (workspace dir already exists, no double-seed) ---
# If init re-ran, it would refuse with "already exists" and exit non-zero;
# start.sh would propagate the failure. A clean re-run proves init was skipped.
"$SCRIPT" "$WORK/freshproj" >"$WORK/init2.out" 2>"$WORK/init2.err"

# --- Test: stdout contains mount= and a next-step hint ---
OUT="$(cat "$WORK/init1.out")"
MNT="$(echo "$OUT" | sed -n 's/^mount=//p')"
[[ -n "$MNT" ]] || { echo "FAIL: no mount= line"; echo "$OUT"; exit 1; }
[[ -d "$MNT" ]] || { echo "FAIL: mount path '$MNT' is not a directory"; exit 1; }
mountpoint -q "$MNT" || { echo "FAIL: '$MNT' is not an active mountpoint"; exit 1; }
grep -q '^next: cd .* && claude' "$WORK/init1.out" || {
    echo "FAIL: missing 'next: cd ... && claude' hint"
    cat "$WORK/init1.out"
    exit 1
}

# Idempotent re-run reports the same mount path.
OUT2="$(cat "$WORK/init2.out")"
MNT2="$(echo "$OUT2" | sed -n 's/^mount=//p')"
[[ "$MNT" == "$MNT2" ]] || { echo "FAIL: re-run mount changed: '$MNT' vs '$MNT2'"; exit 1; }

# --- Test: Claude Code skill installed ---
DEST="$HOME/.claude/skills/agentvfs-workspace/SKILL.md"
[[ -f "$DEST" ]] || { echo "FAIL: SKILL.md not installed at $DEST"; exit 1; }
diff -q "$REPO/docs/skills/agentvfs-workspace.md" "$DEST" >/dev/null || {
    echo "FAIL: installed SKILL.md content differs from repo source"
    diff "$REPO/docs/skills/agentvfs-workspace.md" "$DEST"
    exit 1
}

# --- Test: re-run keeps the file (still equal to source) ---
diff -q "$REPO/docs/skills/agentvfs-workspace.md" "$DEST" >/dev/null || {
    echo "FAIL: SKILL.md drift after second run"; exit 1;
}

# --- Test: ~/.codex/AGENTS.md created with marker block when missing ---
AGENTS="$HOME/.codex/AGENTS.md"
[[ -f "$AGENTS" ]] || { echo "FAIL: AGENTS.md not created at $AGENTS"; exit 1; }
grep -q '^<!-- agentvfs:begin -->$' "$AGENTS" || { echo "FAIL: missing begin marker"; cat "$AGENTS"; exit 1; }
grep -q '^<!-- agentvfs:end -->$' "$AGENTS" || { echo "FAIL: missing end marker"; cat "$AGENTS"; exit 1; }
SKILL_FIRST_LINE="$(sed -n '1p' "$REPO/docs/skills/agentvfs-workspace.md")"
awk '/<!-- agentvfs:begin -->/,/<!-- agentvfs:end -->/' "$AGENTS" \
  | grep -qF -- "$SKILL_FIRST_LINE" || {
    echo "FAIL: managed block does not contain skill content"; cat "$AGENTS"; exit 1;
}

# --- Test: pre-existing AGENTS.md without markers — append, preserve user content ---
rm -f "$AGENTS"
mkdir -p "$HOME/.codex"
cat > "$AGENTS" <<'EOF'
# my codex notes
do not lose this line
EOF
"$SCRIPT" "$WORK/freshproj" >/dev/null 2>"$WORK/cdx-append.err"
grep -qF "do not lose this line" "$AGENTS" || { echo "FAIL: user content lost"; cat "$AGENTS"; exit 1; }
grep -q '^<!-- agentvfs:begin -->$' "$AGENTS" || { echo "FAIL: begin marker not appended"; cat "$AGENTS"; exit 1; }

# --- Test: pre-existing AGENTS.md WITH markers — replace block, preserve user content ---
cat > "$AGENTS" <<'EOF'
# my codex notes
preserved before block

<!-- agentvfs:begin -->
STALE CONTENT TO BE REPLACED
<!-- agentvfs:end -->

preserved after block
EOF
"$SCRIPT" "$WORK/freshproj" >/dev/null 2>"$WORK/cdx-replace.err"
grep -qF "preserved before block" "$AGENTS" || { echo "FAIL: pre-block content lost"; cat "$AGENTS"; exit 1; }
grep -qF "preserved after block" "$AGENTS" || { echo "FAIL: post-block content lost"; cat "$AGENTS"; exit 1; }
grep -qF "STALE CONTENT TO BE REPLACED" "$AGENTS" && { echo "FAIL: stale block not replaced"; cat "$AGENTS"; exit 1; } || true
awk '/<!-- agentvfs:begin -->/,/<!-- agentvfs:end -->/' "$AGENTS" \
  | grep -qF -- "$SKILL_FIRST_LINE" || {
    echo "FAIL: replaced block does not contain skill content"; cat "$AGENTS"; exit 1;
}
BLOCK_COUNT="$(grep -c '^<!-- agentvfs:begin -->$' "$AGENTS")"
[[ "$BLOCK_COUNT" == "1" ]] || { echo "FAIL: expected 1 begin marker, got $BLOCK_COUNT"; exit 1; }

# --- Test: --root relocates workspace state away from XDG_RUNTIME_DIR ---
CUSTOM_ROOT="$WORK/custom-root"
mkdir -p "$WORK/rootproj"
echo "beta" > "$WORK/rootproj/file.txt"
"$SCRIPT" --root "$CUSTOM_ROOT" "$WORK/rootproj" >"$WORK/root1.out" 2>"$WORK/root1.err"
[[ -d "$CUSTOM_ROOT/rootproj/source" ]] || {
    echo "FAIL: --root did not create workspace under custom root"
    ls -la "$CUSTOM_ROOT" 2>/dev/null || true
    exit 1
}
[[ ! -d "$XDG_RUNTIME_DIR/agentvfs/rootproj" ]] || {
    echo "FAIL: --root workspace also leaked into XDG_RUNTIME_DIR"
    exit 1
}
ROOT_MNT="$(sed -n 's/^mount=//p' "$WORK/root1.out")"
[[ "$ROOT_MNT" == "$CUSTOM_ROOT/rootproj/mount" ]] || {
    echo "FAIL: mount path under --root unexpected: $ROOT_MNT"
    cat "$WORK/root1.out"
    exit 1
}
mountpoint -q "$ROOT_MNT" || { echo "FAIL: '$ROOT_MNT' is not an active mountpoint"; exit 1; }
grep -q "^root=$CUSTOM_ROOT$" "$WORK/root1.out" || {
    echo "FAIL: expected 'root=' line in output"; cat "$WORK/root1.out"; exit 1;
}
fusermount3 -u "$ROOT_MNT" 2>/dev/null || true

# --- Test: AGENTVFS_WORKSPACE_ROOT env var has same effect as --root ---
ENV_ROOT="$WORK/env-root"
mkdir -p "$WORK/envproj"
AGENTVFS_WORKSPACE_ROOT="$ENV_ROOT" \
    "$SCRIPT" "$WORK/envproj" >"$WORK/env1.out" 2>"$WORK/env1.err"
[[ -d "$ENV_ROOT/envproj/source" ]] || {
    echo "FAIL: AGENTVFS_WORKSPACE_ROOT did not create workspace under env root"
    exit 1
}
ENV_MNT="$(sed -n 's/^mount=//p' "$WORK/env1.out")"
[[ "$ENV_MNT" == "$ENV_ROOT/envproj/mount" ]] || {
    echo "FAIL: env-var mount path unexpected: $ENV_MNT"; exit 1;
}
fusermount3 -u "$ENV_MNT" 2>/dev/null || true

# --- Test: --root requires an argument ---
set +e
"$SCRIPT" --root >"$WORK/root-missing.out" 2>"$WORK/root-missing.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: --root without value should exit non-zero"; exit 1; }
grep -q "requires <dir>" "$WORK/root-missing.err" || {
    echo "FAIL: missing '--root requires <dir>' message"; cat "$WORK/root-missing.err"; exit 1;
}

# --- Test: unknown option rejected ---
set +e
"$SCRIPT" --bogus "$WORK/freshproj" >"$WORK/bad-opt.out" 2>"$WORK/bad-opt.err"
RC=$?
set -e
[[ "$RC" -ne 0 ]] || { echo "FAIL: unknown option should exit non-zero"; exit 1; }
grep -q "unknown option" "$WORK/bad-opt.err" || {
    echo "FAIL: missing 'unknown option' message"; cat "$WORK/bad-opt.err"; exit 1;
}

echo "PASS start.sh"
