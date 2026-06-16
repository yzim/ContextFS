#!/bin/bash
# test_quickstart_demo_smoke.sh — Verifies demo/agentvfs-quickstart.sh runs
# end-to-end with all four beats (DEPLOY → MOUNT → SKILL → AGENT) without
# requiring VHS or producing a GIF. Suitable for CI.
#
# Isolates HOME so the test doesn't write to the developer's
# ~/.claude/skills/ directory (start.sh installs the skill into $HOME).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/demo/agentvfs-quickstart.sh"
export PATH="$REPO_ROOT/build:$PATH"

if [[ ! -x "$SCRIPT" ]]; then
    echo "FAIL: $SCRIPT not found or not executable" >&2
    exit 1
fi

TMP="$(mktemp -d -t quickstart-smoke.XXXXXX)"
FAKE_HOME="$TMP/home"
DEMO_ROOT="$TMP/state"
mkdir -p "$FAKE_HOME" "$DEMO_ROOT"

cleanup() {
    # Best-effort: stop any leftover workspace before nuking the dir.
    if command -v agentvfs >/dev/null 2>&1; then
        agentvfs workspace stop myproject --root "$DEMO_ROOT" \
            --no-checkpoint >/dev/null 2>&1 || true
    fi
    if [[ -d "$DEMO_ROOT" ]]; then
        for mnt in "$DEMO_ROOT"/*/mount; do
            [[ -d "$mnt" ]] || continue
            mountpoint -q "$mnt" && fusermount3 -u "$mnt" 2>/dev/null || true
        done
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

OUT="$TMP/output.log"

# DEMO_NO_TYPE=1 short-circuits typing and spinner sleeps; the script
# still prints every line so we can grep its structure.
HOME="$FAKE_HOME" \
DEMO_NO_TYPE=1 \
DEMO_ROOT="$DEMO_ROOT" \
DEMO_FIXTURE="$REPO_ROOT/demo/fixture/myproject" \
"$SCRIPT" > "$OUT" 2>&1 || {
    echo "FAIL: quickstart script exited non-zero" >&2
    cat "$OUT" >&2
    exit 1
}

assert_grep() {
    local pattern="$1" label="$2"
    if ! grep -qE "$pattern" "$OUT"; then
        echo "FAIL: $label not found in output" >&2
        echo "      pattern: $pattern" >&2
        echo "------ output ------" >&2
        cat "$OUT" >&2
        echo "--------------------" >&2
        exit 1
    fi
}

# Every beat prints "▸ <NAME>" via beat_divider. Strip ANSI before grepping
# so the colored output doesn't trip the patterns.
sed -i 's/\x1b\[[0-9;]*m//g' "$OUT"

assert_grep '▸ DEPLOY'                'DEPLOY beat divider'
assert_grep '▸ MOUNT'                 'MOUNT beat divider'
assert_grep '▸ SKILL'                 'SKILL beat divider'
assert_grep '▸ AGENT'                 'AGENT beat divider'
assert_grep '^mount='                 'deploy emitted mount= line'
assert_grep '\[ ready in'             'deploy chip rendered'
assert_grep 'real files, content-addressed' 'mount caption rendered'
assert_grep 'skill installed → '      'skill install confirmation'
assert_grep '^name: agentvfs-workspace' 'skill name line'
assert_grep 'Claude Code'             'agent panel header rendered'
assert_grep '⏺ Bash'                  'first tool call rendered'
assert_grep '⏺ Read'                  'second tool call rendered'
assert_grep 'State checkpointed'      'final reply rendered'

# The skill file should also be on disk (start.sh installed it under FAKE_HOME).
SKILL_FILE="$FAKE_HOME/.claude/skills/agentvfs-workspace/SKILL.md"
if [[ ! -f "$SKILL_FILE" ]]; then
    echo "FAIL: skill file not at $SKILL_FILE" >&2
    exit 1
fi

echo "PASS: all 4 beats rendered, skill installed under FAKE_HOME"
