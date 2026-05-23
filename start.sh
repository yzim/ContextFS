#!/bin/bash
set -euo pipefail

usage() {
    cat >&2 <<'EOF'
usage: start.sh [--root <dir>] <path-to-project>

  --root <dir>   place workspace state (store, mount, socket) under <dir>
                 instead of $XDG_RUNTIME_DIR/agentvfs (or /tmp/agentvfs-$UID).
                 Subsequent 'agentvfs workspace ...' commands need the same
                 --root or AGENTVFS_WORKSPACE_ROOT=<dir> to find the workspace.

Environment:
  AGENTVFS_WORKSPACE_ROOT  default value for --root (overridden by the flag)
EOF
}

ROOT=""
SRC_PATH=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --root)
            if [[ $# -lt 2 ]]; then
                echo "start.sh: --root requires <dir>" >&2
                exit 2
            fi
            ROOT="$2"
            shift 2
            ;;
        --root=*)
            ROOT="${1#--root=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        --)
            shift
            break
            ;;
        -*)
            echo "start.sh: unknown option '$1'" >&2
            usage
            exit 2
            ;;
        *)
            if [[ -n "$SRC_PATH" ]]; then
                echo "start.sh: unexpected extra argument '$1'" >&2
                usage
                exit 2
            fi
            SRC_PATH="$1"
            shift
            ;;
    esac
done

if [[ -z "$SRC_PATH" ]]; then
    usage
    exit 2
fi

# Flag wins over env var; env var wins over the workspace_cli.cpp default.
if [[ -z "$ROOT" && -n "${AGENTVFS_WORKSPACE_ROOT:-}" ]]; then
    ROOT="$AGENTVFS_WORKSPACE_ROOT"
fi

if [[ ! -e "$SRC_PATH" ]]; then
    echo "start.sh: '$SRC_PATH' does not exist" >&2
    exit 1
fi

if [[ ! -d "$SRC_PATH" ]]; then
    echo "start.sh: '$SRC_PATH' is not a directory" >&2
    exit 1
fi

ABS_PATH="$(realpath "$SRC_PATH")"

if ! command -v agentvfs >/dev/null 2>&1; then
    echo "start.sh: agentvfs not found — run cmake --build && sudo cmake --install build first" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# In a repo checkout the skill source lives under docs/skills/. In a
# packaged install (release.yml + install.sh) it's bundled flat next to
# the renamed agentvfs-quickstart script.
SKILL_SRC=""
for candidate in \
    "$SCRIPT_DIR/docs/skills/agentvfs-workspace.md" \
    "$SCRIPT_DIR/agentvfs-workspace.md"; do
    if [[ -f "$candidate" ]]; then
        SKILL_SRC="$candidate"
        break
    fi
done

if [[ -z "$SKILL_SRC" ]]; then
    echo "start.sh: skill source not found." >&2
    echo "  searched: $SCRIPT_DIR/docs/skills/agentvfs-workspace.md" >&2
    echo "            $SCRIPT_DIR/agentvfs-workspace.md" >&2
    echo "  If you installed via install.sh, this is a packaging bug — please" >&2
    echo "  report at https://github.com/thustorage/ContextFS/issues." >&2
    echo "  If you're running from a repo checkout, make sure" >&2
    echo "  docs/skills/agentvfs-workspace.md exists." >&2
    exit 1
fi

NAME="$(basename "$ABS_PATH")"

# Validate name against the workspace_cli.cpp:46 charset (alnum + - _ .).
if [[ ! "$NAME" =~ ^[A-Za-z0-9._-]+$ ]]; then
    echo "start.sh: invalid workspace name '$NAME' (must match [A-Za-z0-9._-]+) — rename the directory or use 'agentvfs workspace init' directly" >&2
    exit 1
fi

# Replicate workspace_cli.cpp:default_workspace_root() so we can probe for an
# existing workspace directory. `agentvfs workspace list` only reports
# workspaces that have been started (it filters on session.json), so it can't
# tell us whether init has already run.
ROOT_FLAGS=()
if [[ -n "$ROOT" ]]; then
    mkdir -p "$ROOT"
    ROOT="$(realpath "$ROOT")"
    WORKSPACE_ROOT="$ROOT"
    ROOT_FLAGS=(--root "$ROOT")
elif [[ -n "${XDG_RUNTIME_DIR:-}" ]]; then
    WORKSPACE_ROOT="$XDG_RUNTIME_DIR/agentvfs"
else
    WORKSPACE_ROOT="/tmp/agentvfs-$(id -u)"
fi

if [[ ! -d "$WORKSPACE_ROOT/$NAME" ]]; then
    agentvfs workspace init "$NAME" --from "$ABS_PATH" \
        ${ROOT_FLAGS[@]+"${ROOT_FLAGS[@]}"} >/dev/null
fi

START_OUT="$(agentvfs workspace start "$NAME" \
    ${ROOT_FLAGS[@]+"${ROOT_FLAGS[@]}"})"
MOUNT_PATH="$(echo "$START_OUT" | sed -n 's/^mount=//p' | head -n 1)"

if [[ -z "$MOUNT_PATH" ]]; then
    echo "start.sh: 'agentvfs workspace start' did not report a mount path" >&2
    echo "$START_OUT" >&2
    exit 1
fi

CLAUDE_DIR="$HOME/.claude/skills/agentvfs-workspace"
mkdir -p "$CLAUDE_DIR"
cp "$SKILL_SRC" "$CLAUDE_DIR/SKILL.md"

CODEX_DIR="$HOME/.codex"
AGENTS_FILE="$CODEX_DIR/AGENTS.md"
BEGIN_MARK='<!-- agentvfs:begin -->'
END_MARK='<!-- agentvfs:end -->'

mkdir -p "$CODEX_DIR"

build_block() {
    printf '%s\n' "$BEGIN_MARK"
    cat "$SKILL_SRC"
    printf '%s\n' "$END_MARK"
}

if [[ ! -f "$AGENTS_FILE" ]]; then
    build_block > "$AGENTS_FILE"
elif grep -qF "$BEGIN_MARK" "$AGENTS_FILE" && grep -qF "$END_MARK" "$AGENTS_FILE"; then
    NEW_BLOCK="$(build_block)"
    TMP="$(mktemp)"
    awk -v block="$NEW_BLOCK" -v b="$BEGIN_MARK" -v e="$END_MARK" '
        BEGIN { in_block = 0 }
        $0 == b { print block; in_block = 1; next }
        $0 == e { in_block = 0; next }
        !in_block { print }
    ' "$AGENTS_FILE" > "$TMP"
    mv "$TMP" "$AGENTS_FILE"
else
    {
        printf '\n'
        build_block
    } >> "$AGENTS_FILE"
fi

echo "mount=$MOUNT_PATH"
if [[ -n "$ROOT" ]]; then
    echo "root=$ROOT"
    echo "next: cd $MOUNT_PATH && claude   # or: codex   (workspace cmds need --root $ROOT)"
else
    echo "next: cd $MOUNT_PATH && claude   # or: codex"
fi

exit 0
