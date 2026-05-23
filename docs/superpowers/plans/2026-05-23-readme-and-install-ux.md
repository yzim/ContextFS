# README and Install UX Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship `install.sh` + `install.ps1` (one-line installers that fetch GitHub Releases prebuilts), a `release.yml` workflow that publishes those artifacts on tag push, and a restructured README whose hero is two commands.

**Architecture:** Both installers detect host, download `agentvfs-<version>-<tuple>.{tar.gz,zip}` + `checksums.txt`, verify SHA-256, extract to `~/.local/bin` (or `%LOCALAPPDATA%\Programs\agentvfs`). `release.yml` builds four platform tuples on tag push and creates a release with checksums. README hero is install + mount; existing cmake quickstarts are demoted to a "Build from source" section. `start.sh` is unchanged in the repo; the release workflow copies it to `agentvfs-quickstart` when packaging.

**Tech Stack:** POSIX `sh`, PowerShell 5+, GitHub Actions.

**Spec:** `docs/superpowers/specs/2026-05-23-readme-and-install-ux-design.md`

**Testing approach:** No in-repo automated tests for the installers. Smoke-test manually against the real `v0.1.0` release artifacts. release.yml is exercised by tagging; if it breaks, delete the tag and re-cut.

---

## File structure

**New files:**
- `install.sh` — POSIX sh, root of repo.
- `install.ps1` — PowerShell, root of repo.
- `.github/workflows/release.yml` — tag-triggered build + publish.

**Modified:**
- `README.md` — restructured per spec.

**Unchanged but referenced:**
- `start.sh` — copied to `agentvfs-quickstart` at packaging time.

---

## Task 1: install.sh

**Files:** Create `install.sh`.

- [ ] **Step 1.1: Write install.sh**

Create `install.sh` at the repo root:

```sh
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

expected="$(awk -v f="$ARCHIVE" '$2 == f { print $1 }' "$WORK/$CHECKSUMS")"
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
tar -xzf "$WORK/$ARCHIVE" -C "$WORK"
mkdir -p "$PREFIX"
for f in agentvfs agentvfs-ctl agentvfs-quickstart; do
    [ -f "$WORK/$f" ] && install -m 0755 "$WORK/$f" "$PREFIX/$f"
done

# Final message.
echo
echo "Installed agentvfs $VERSION to $PREFIX"
echo
echo "next: agentvfs-quickstart /path/to/project"
case ":$PATH:" in
    *":$PREFIX:"*) : ;;
    *) echo "  (add $PREFIX to PATH if it isn't already)" ;;
esac
```

Mark it executable: `chmod +x install.sh`.

- [ ] **Step 1.2: Sanity check the script parses**

```
sh -n install.sh && echo "syntax ok"
./install.sh --help
```

Expected: `syntax ok`, then the usage block.

- [ ] **Step 1.3: Commit**

```bash
git add install.sh
git commit -m "$(cat <<'EOF'
feat(install): one-line installer for Linux + macOS

install.sh detects host, resolves latest release (or honors
--version / AGENTVFS_INSTALL_VERSION), downloads the prebuilt
archive + checksums.txt, verifies SHA-256, and installs agentvfs,
agentvfs-ctl, and agentvfs-quickstart to ~/.local/bin (default)
or --prefix.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: install.ps1

**Files:** Create `install.ps1`.

- [ ] **Step 2.1: Write install.ps1**

Create `install.ps1` at the repo root:

```powershell
# install.ps1 — installs the agentvfs prebuilt to %LOCALAPPDATA%\Programs\agentvfs.
# Usage: iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex

[CmdletBinding()]
param(
    [string]$Version = $env:AGENTVFS_INSTALL_VERSION,
    [string]$Prefix  = $env:AGENTVFS_INSTALL_PREFIX,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
$Repo = 'thustorage/ContextFS'
$UrlBase = "https://github.com/$Repo/releases/download"

if ($Help) {
    @'
Usage: install.ps1 [-Version <tag>] [-Prefix <dir>] [-Help]

Installs the agentvfs prebuilt to Prefix.
Default Prefix is %LOCALAPPDATA%\Programs\agentvfs (no admin).

Environment:
  AGENTVFS_INSTALL_VERSION   pin a release tag (e.g. v0.1.0)
  AGENTVFS_INSTALL_PREFIX    override install destination
'@ | Write-Output
    return
}

$arch = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture
if ($arch -ne 'X64') {
    Write-Error "install.ps1: unsupported arch '$arch' — only windows-x86_64 has a prebuilt. Build from source."
    exit 1
}
$Tuple = 'windows-x86_64'

if (-not $Version) {
    try {
        $rel = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/latest"
        $Version = $rel.tag_name
    } catch {
        Write-Error "install.ps1: could not resolve latest release; pin with -Version vX.Y.Z"
        exit 1
    }
}

if (-not $Prefix) { $Prefix = Join-Path $env:LOCALAPPDATA 'Programs\agentvfs' }

$archive    = "agentvfs-$Version-$Tuple.zip"
$checksums  = "agentvfs-$Version-checksums.txt"
$work = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "agentvfs-install-$(Get-Random)") -Force

try {
    Invoke-WebRequest -UseBasicParsing -Uri "$UrlBase/$Version/$archive"   -OutFile (Join-Path $work $archive)
    Invoke-WebRequest -UseBasicParsing -Uri "$UrlBase/$Version/$checksums" -OutFile (Join-Path $work $checksums)

    $expected = (Get-Content (Join-Path $work $checksums) | Where-Object { $_ -match "  $archive$" }) -replace '\s.*',''
    if (-not $expected) { throw "install.ps1: $archive not in checksums file" }
    $actual = (Get-FileHash -Algorithm SHA256 (Join-Path $work $archive)).Hash.ToLower()
    if ($expected.ToLower() -ne $actual) {
        throw "install.ps1: checksum mismatch for $archive`n  expected: $expected`n  actual:   $actual"
    }

    Expand-Archive -Path (Join-Path $work $archive) -DestinationPath $work -Force
    New-Item -ItemType Directory -Path $Prefix -Force | Out-Null
    foreach ($f in 'agentvfs.exe','agentvfs-ctl.exe') {
        Copy-Item -Path (Join-Path $work $f) -Destination (Join-Path $Prefix $f) -Force
    }

    Write-Output ""
    Write-Output "Installed agentvfs $Version to $Prefix"
    Write-Output ""
    Write-Output "next:"
    Write-Output "  agentvfs.exe --source C:\some\dir --mountpoint Z:"
    Write-Output "  agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline"

    $userPath = [Environment]::GetEnvironmentVariable('Path','User')
    if ($userPath -notlike "*$Prefix*") {
        Write-Output ""
        Write-Output "  (add $Prefix to your PATH)"
    }
} finally {
    Remove-Item -Recurse -Force $work
}
```

- [ ] **Step 2.2: Commit**

```bash
git add install.ps1
git commit -m "$(cat <<'EOF'
feat(install): one-line installer for Windows

install.ps1 mirrors install.sh: detect arch (x86_64 only), resolve
latest release (or honor -Version / AGENTVFS_INSTALL_VERSION),
download .zip + checksums.txt, verify SHA-256, extract agentvfs.exe
and agentvfs-ctl.exe to %LOCALAPPDATA%\Programs\agentvfs by default.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Release workflow

**Files:** Create `.github/workflows/release.yml`.

- [ ] **Step 3.1: Write release.yml**

Create `.github/workflows/release.yml`:

```yaml
name: Release

on:
  push:
    tags: ['v*']

permissions:
  contents: write

jobs:
  build-linux-x86_64:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: Install deps
        run: sudo apt-get update && sudo apt-get install -y libfuse3-dev fuse3
      - name: Build
        run: |
          cmake -B build -DAGENTVFS_EBPF=OFF -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl LICENSE "$stage/"
          cp start.sh "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-linux-x86_64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: linux-x86_64
          path: agentvfs-*-linux-x86_64.tar.gz

  build-darwin-arm64:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v4
      - run: brew install --cask macos-fuse-t/cask/fuse-t
      - name: Build
        run: |
          cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl LICENSE "$stage/"
          cp start.sh "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-darwin-arm64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: darwin-arm64
          path: agentvfs-*-darwin-arm64.tar.gz

  build-darwin-x86_64:
    runs-on: macos-13
    steps:
      - uses: actions/checkout@v4
      - run: brew install --cask macos-fuse-t/cask/fuse-t
      - name: Build
        run: |
          cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
      - name: Package
        run: |
          version="${GITHUB_REF_NAME}"
          stage="$(mktemp -d)"
          cp build/agentvfs build/agentvfs-ctl LICENSE "$stage/"
          cp start.sh "$stage/agentvfs-quickstart"
          chmod +x "$stage/agentvfs-quickstart"
          tar -C "$stage" -czf "agentvfs-${version}-darwin-x86_64.tar.gz" \
              agentvfs agentvfs-ctl agentvfs-quickstart LICENSE
      - uses: actions/upload-artifact@v4
        with:
          name: darwin-x86_64
          path: agentvfs-*-darwin-x86_64.tar.gz

  build-windows-x86_64:
    runs-on: windows-latest
    steps:
      - uses: actions/checkout@v4
      - run: choco install winfsp -y --no-progress
        shell: powershell
      - name: Build
        run: |
          cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON -DCMAKE_BUILD_TYPE=Release
          cmake --build build --config Release -j
      - name: Package
        shell: powershell
        run: |
          $version = "$env:GITHUB_REF_NAME"
          $stage = New-Item -ItemType Directory -Path (Join-Path $env:TEMP "stage-$(Get-Random)") -Force
          Copy-Item build\Release\agentvfs.exe     $stage
          Copy-Item build\Release\agentvfs-ctl.exe $stage
          Copy-Item LICENSE                        $stage
          Compress-Archive -Path "$stage\*" -DestinationPath "agentvfs-$version-windows-x86_64.zip"
      - uses: actions/upload-artifact@v4
        with:
          name: windows-x86_64
          path: agentvfs-*-windows-x86_64.zip

  publish:
    needs: [build-linux-x86_64, build-darwin-arm64, build-darwin-x86_64, build-windows-x86_64]
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: actions/download-artifact@v4
        with: { merge-multiple: true }
      - name: Generate checksums
        run: |
          version="${GITHUB_REF_NAME}"
          sha256sum agentvfs-${version}-linux-x86_64.tar.gz \
                    agentvfs-${version}-darwin-arm64.tar.gz \
                    agentvfs-${version}-darwin-x86_64.tar.gz \
                    agentvfs-${version}-windows-x86_64.zip \
              > "agentvfs-${version}-checksums.txt"
      - name: Create release
        env:
          GH_TOKEN: ${{ secrets.GITHUB_TOKEN }}
        run: |
          version="${GITHUB_REF_NAME}"
          gh release create "$version" \
              --title "$version" \
              --generate-notes \
              "agentvfs-${version}-linux-x86_64.tar.gz" \
              "agentvfs-${version}-darwin-arm64.tar.gz" \
              "agentvfs-${version}-darwin-x86_64.tar.gz" \
              "agentvfs-${version}-windows-x86_64.zip" \
              "agentvfs-${version}-checksums.txt"
```

- [ ] **Step 3.2: Validate YAML parses**

```
python3 -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml')); print('ok')"
```

Expected: `ok`.

- [ ] **Step 3.3: Commit**

```bash
git add .github/workflows/release.yml
git commit -m "$(cat <<'EOF'
feat(release): GitHub Actions release workflow

Tag-triggered (v*) workflow. Four parallel build jobs produce
artifacts for linux-x86_64 (ubuntu-22.04, glibc 2.35 floor),
darwin-arm64, darwin-x86_64, windows-x86_64. Publish job
aggregates, generates SHA-256 checksums.txt, and creates the
GitHub Release via gh CLI.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: README rewrite and v0.1.0 cut

**Files:** Modify `README.md`. Then push the `v0.1.0` tag.

- [ ] **Step 4.1: Rewrite README.md**

Replace the entire file with:

```markdown
<p align="center">
  <img src="https://github.com/thustorage/ContextFS/raw/main/docs/agentvfs-logo.svg" alt="AgentVFS" width="400">
</p>

<p align="center"><strong>Checkpointable, branchable FUSE workspace for AI agents.</strong></p>

## Install

```bash
# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh

# Windows (PowerShell)
iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex
```

The Linux prebuilt needs glibc ≥ 2.35 (Debian 12, Ubuntu 22.04+, RHEL 9+). The daemon needs a FUSE runtime: `sudo apt install libfuse3-3` (Linux), `brew install --cask macos-fuse-t/cask/fuse-t` (macOS), or [WinFsp 2.0+](https://winfsp.dev) (Windows). Older distros or anything else: see **Build from source** below.

## Mount a project

```bash
# Linux / macOS
agentvfs-quickstart /path/to/project
# prints `mount=<path>` and a `cd <path> && claude` (or `codex`) hint
```

```powershell
# Windows
agentvfs.exe --source C:\some\dir --mountpoint Z:
agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

## What you get

| Feature | Description |
|---------|-------------|
| ⏪&nbsp;**Checkpoint&nbsp;&amp;&nbsp;Rollback** | Snapshot the working tree and roll back to any prior checkpoint |
| 🌿&nbsp;**Per&#8209;Agent&nbsp;Branches** | N agents over one source tree, routed by cgroup v2; three-way merge or surfaced conflicts |
| 🔗&nbsp;**Content&#8209;Addressed&nbsp;Store** | blake3-hashed objects deduplicate across checkpoints and branches — near-zero-cost snapshots |
| 🛰️&nbsp;**Pluggable&nbsp;Telemetry** | NDJSON audit via eBPF / fanotify / ptrace / `LD_PRELOAD`; Wasm or Lua to filter and verdict |
| 🖥️&nbsp;**Cross&#8209;Platform** | libfuse3 (Linux), fuse-t (macOS), WinFsp (Windows) |
| 🤖&nbsp;**Agent&#8209;CLI&nbsp;Skills** | `agentvfs-quickstart` mounts a project and installs a Skill for Claude Code / Codex to checkpoint and roll back |

## Driving the daemon directly

```bash
agentvfs workspace start my-task --from /path/to/project
agentvfs workspace checkpoint my-task before-refactor
# ... agent makes changes ...
agentvfs workspace rollback my-task before-refactor
agentvfs workspace stop my-task
```

## How it works

```
agent process
   │  read/write
   ▼
FUSE / WinFsp / fuse-t  ──►  WorkingTree (in-memory, COW)  ──►  ObjectStore (blake3 CAS)
                                                            ▲
                                              checkpoint /  │  rollback
                                                            │
                                              control socket│ ─► agentvfs-ctl
                                                            │
                                              optional eBPF │ ─► NDJSON audit log
```

## Platform support

| Feature | Linux | macOS | Windows |
|---|:---:|:---:|:---:|
| Prebuilt installer | ✅ x86_64 (glibc ≥ 2.35) | ✅ arm64 + x86_64 | ⚠️ no `agentvfs-quickstart` |
| Checkpoint &amp; Rollback | ✅ | ✅ | ✅ |
| Content-Addressed Store | ✅ | ✅ | ✅ |
| Per-Agent Branches | ✅ | Coming soon | Coming soon |
| Pluggable Telemetry | ✅ | Coming soon | Coming soon |
| `agentvfs workspace` CLI | ✅ | Coming soon | Coming soon |

## Build from source

Use this path for Linux distros below glibc 2.35, to enable eBPF telemetry, or to contribute.

```bash
# Linux (Debian/Ubuntu)
sudo apt install build-essential cmake libfuse3-dev pkg-config \
                 clang libbpf-dev linux-tools-generic
cmake -B build && cmake --build build -j
sudo cmake --install build
./start.sh /path/to/project
# eBPF telemetry is on by default; -DAGENTVFS_EBPF=OFF to skip.
```

```bash
# macOS
brew install --cask macos-fuse-t/cask/fuse-t
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON
cmake --build build -j
./build/agentvfs --source ~/some-dir --mountpoint /tmp/agentvfs-mnt &
./build/agentvfs-ctl --sock /tmp/agentvfs-<hash>.sock checkpoint baseline
```

```powershell
# Windows — requires WinFsp 2.0+ from https://winfsp.dev
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
cmake --build build --config Release -j
.\build\Release\agentvfs.exe --source C:\some\dir --mountpoint Z:
.\build\Release\agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

Pin a release: `AGENTVFS_INSTALL_VERSION=v0.1.0 curl -fsSL .../install.sh | sh`. Benchmark harness: `benchmarks/agent-sim/run.sh` (see `benchmarks/agent-sim/README.md`).

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
```

- [ ] **Step 4.2: Commit the README and push to main**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
docs: restructure README around install.sh + install.ps1

Hero is two commands: install, then mount. Per-OS cmake quickstarts
demoted to a 'Build from source' section. Platform support matrix
gets a row for prebuilt-installer coverage.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
git push origin main
```

- [ ] **Step 4.3: Cut v0.1.0**

**Ask the user before running** — this is the first public release and any mistake means a deleted tag.

```bash
git tag v0.1.0
git push origin v0.1.0
gh run watch
```

Expected: four build jobs succeed, publish job creates the release.

- [ ] **Step 4.4: Smoke-test the installed binary**

On a clean Linux host (VM, container, or fresh user account):

```
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh
~/.local/bin/agentvfs --help
```

Expected: install completes; `--help` prints the daemon usage. If anything fails:

```
git push --delete origin v0.1.0
git tag -d v0.1.0
# fix, re-tag, re-push
```

- [ ] **Step 4.5: Smoke-test the hero verbatim**

```
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh
agentvfs-quickstart /tmp/some-project
```

Expected: prints `mount=<path>` and the `cd <path> && claude` hint.

---

## Self-review notes

- **Spec coverage**: installer contract (Tasks 1+2), artifact format + CI (Task 3), README restructure + quickstart rename + v0.1.0 cut (Task 4). All risks in the spec are accepted (no runtime probes); the smoke tests in Task 4 are the regression net.
- **Placeholders**: none.
- **Type consistency**: `AGENTVFS_INSTALL_VERSION` / `AGENTVFS_INSTALL_PREFIX`, `--version` / `--prefix` / `--help`, the four artifact tuples, and the `agentvfs-<version>-<tuple>.{tar.gz,zip}` + `checksums.txt` filenames are consistent across all four tasks.
- **Out of scope**: `start.sh` unchanged in the repo; daemon and `agentvfs workspace` CLI untouched.
