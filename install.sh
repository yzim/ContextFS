#!/bin/sh
# install.sh — installs the agentvfs prebuilt to ~/.local/bin (default).
# Usage: curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh
set -eu

REPO="thustorage/ContextFS"
URL_BASE="https://github.com/$REPO/releases/download"
VERSION="${AGENTVFS_INSTALL_VERSION:-}"
PREFIX="${AGENTVFS_INSTALL_PREFIX:-$HOME/.local/bin}"

usage() {
    cat <<'EOF'
Usage: install.sh [--version <tag>] [--prefix <dir>] [--help]

Installs the agentvfs prebuilt binary, agentvfs-ctl, and agentvfs-quickstart
to PREFIX. Default PREFIX is ~/.local/bin (no sudo).

Environment:
  AGENTVFS_INSTALL_VERSION  pin a release tag (e.g. v0.1.0)
  AGENTVFS_INSTALL_PREFIX   override install destination
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version)   VERSION="$2"; shift 2 ;;
        --version=*) VERSION="${1#--version=}"; shift ;;
        --prefix)    PREFIX="$2"; shift 2 ;;
        --prefix=*)  PREFIX="${1#--prefix=}"; shift ;;
        --help|-h)   usage; exit 0 ;;
        *) echo "install.sh: unknown argument '$1'" >&2; usage >&2; exit 2 ;;
    esac
done

# Detect host.
os="$(uname -s | tr '[:upper:]' '[:lower:]')"
arch="$(uname -m)"
case "$os" in
    linux|darwin) : ;;
    *) echo "install.sh: unsupported OS '$os' — build from source" >&2; exit 1 ;;
esac
case "$arch" in
    x86_64|amd64) arch=x86_64 ;;
    arm64|aarch64) [ "$os" = darwin ] && arch=arm64 || {
        echo "install.sh: aarch64 Linux prebuilt not yet published — build from source" >&2
        exit 1; } ;;
    *) echo "install.sh: unsupported arch '$arch' — build from source" >&2; exit 1 ;;
esac
HOST_TUPLE="$os-$arch"

# Resolve version.
if [ -z "$VERSION" ]; then
    api="https://api.github.com/repos/$REPO/releases/latest"
    if command -v curl >/dev/null; then
        VERSION="$(curl -fsSL "$api" | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
    elif command -v wget >/dev/null; then
        VERSION="$(wget -qO- "$api" | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
    else
        echo "install.sh: need curl or wget on PATH" >&2; exit 1
    fi
    [ -n "$VERSION" ] || { echo "install.sh: could not resolve latest release; pin with --version" >&2; exit 1; }
fi

ARCHIVE="agentvfs-$VERSION-$HOST_TUPLE.tar.gz"
CHECKSUMS="agentvfs-$VERSION-checksums.txt"

# Download and verify.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

fetch() {
    if command -v curl >/dev/null; then
        curl -fsSL --retry 3 --retry-delay 2 -o "$2" "$1" \
            || { echo "install.sh: download failed: $1" >&2; exit 1; }
    else
        wget -q --tries=3 -O "$2" "$1" \
            || { echo "install.sh: download failed: $1" >&2; exit 1; }
    fi
}

fetch "$URL_BASE/$VERSION/$ARCHIVE"   "$WORK/$ARCHIVE"
fetch "$URL_BASE/$VERSION/$CHECKSUMS" "$WORK/$CHECKSUMS"

expected="$(awk -v f="$ARCHIVE" '{ sub(/^\*/, "", $2); if ($2 == f) print $1 }' "$WORK/$CHECKSUMS")"
[ -n "$expected" ] || { echo "install.sh: $ARCHIVE not in checksums file" >&2; exit 1; }

if command -v sha256sum >/dev/null; then
    actual="$(sha256sum "$WORK/$ARCHIVE" | awk '{print $1}')"
else
    actual="$(shasum -a 256 "$WORK/$ARCHIVE" | awk '{print $1}')"
fi
if [ "$expected" != "$actual" ]; then
    echo "install.sh: checksum mismatch for $ARCHIVE" >&2
    echo "  expected: $expected" >&2
    echo "  actual:   $actual" >&2
    exit 1
fi

# Extract and install.
# Archive is expected to be flat (no nested dir); see release.yml packaging.
tar -xzf "$WORK/$ARCHIVE" -C "$WORK"
if [ ! -f "$WORK/agentvfs" ]; then
    echo "install.sh: archive missing 'agentvfs' binary — please report at https://github.com/thustorage/ContextFS/issues" >&2
    exit 1
fi
mkdir -p "$PREFIX"
for f in agentvfs agentvfs-ctl agentvfs-quickstart; do
    [ -f "$WORK/$f" ] && install -m 0755 "$WORK/$f" "$PREFIX/$f"
done

# Final message.
echo
echo "Installed agentvfs $VERSION to $PREFIX"
echo
case ":$PATH:" in
    *":$PREFIX:"*)
        echo "next: agentvfs-quickstart /path/to/project"
        ;;
    *)
        echo "Note: $PREFIX is not in your PATH. Add it:"
        echo "  export PATH=\"$PREFIX:\$PATH\""
        echo "  echo 'export PATH=\"$PREFIX:\$PATH\"' >> ~/.bashrc   # or ~/.zshrc"
        echo
        echo "Then: agentvfs-quickstart /path/to/project"
        ;;
esac
