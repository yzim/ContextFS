# README and install UX redesign

**Date:** 2026-05-23
**Status:** Draft, awaiting user review
**Scope:** README + new install.sh + install.ps1 + GitHub Releases workflow. No source-tree changes outside that.

## Problem

Today's README and install flow read as a research artifact:

- Three OS-specific quickstarts with different cmake commands.
- `cmake -B build && cmake --build build -j && sudo cmake --install build` is the install path — minutes of build time, requires sudo, requires build deps installed first.
- `AGENTVFS_EBPF=ON` is the cmake default. Hosts without `bpftool` + kernel BTF fail at cmake time.
- The README leads with a feature table rather than a "what do I type" hero.

## Goals

1. README hero is two commands: install, then mount. No env-var prerequisite, no cmake, no sudo by default.
2. Linux, macOS, Windows all reachable from the hero. Windows is the lower-tier platform but doesn't look broken.
3. Existing source-build path remains for contributors — demoted, not removed.

## Non-goals

- Homebrew tap, apt repo, MSI bundle. Package-manager distribution comes later.
- Hosted landing page; use `raw.githubusercontent.com` URLs.
- Code signing, reproducible builds, SBOM, self-update, scripted uninstall, Docker image.
- aarch64 Linux artifact in v0.
- Auto-install of host FUSE runtime (libfuse3 / fuse-t / WinFsp). Document the command; don't run sudo invisibly.
- Runtime probes for glibc or FUSE. Document the requirement instead.
- Renaming `start.sh` in the repo. The release workflow copies it to `agentvfs-quickstart` when packaging.
- Changes to the `agentvfs` daemon, `agentvfs-ctl`, the workspace CLI, or the agentvfs-workspace skill.

## Audience

Primary: a curious dev who saw a tweet or HN post and wants to mount a project in under five minutes. Optimize the README hero for this user. Production users and researchers keep their existing entry points (`Driving the daemon directly`, `benchmarks/agent-sim/`, the build-from-source section), addressed below the fold.

## Installer contract

Two scripts, one contract: `install.sh` (POSIX sh, Linux + macOS) and `install.ps1` (PowerShell 5+, Windows).

1. **Detect host.** `uname` on POSIX; `RuntimeInformation` on PowerShell. Supported tuples: `linux-x86_64`, `darwin-arm64`, `darwin-x86_64`, `windows-x86_64`. Anything else exits 1 with a "build from source" pointer.
2. **Resolve version.** Default: latest GitHub Release for `thustorage/ContextFS` via the public Releases API. Override with `--version vX.Y.Z` or `AGENTVFS_INSTALL_VERSION`.
3. **Download** `agentvfs-<version>-<os>-<arch>.tar.gz` (or `.zip` on Windows) and `agentvfs-<version>-checksums.txt`.
4. **Verify SHA-256.** Refuse to continue on mismatch — no partial install.
5. **Extract and install** into `--prefix <dir>` (default `~/.local/bin`, or `%LOCALAPPDATA%\Programs\agentvfs` on Windows). Three files on PATH on Linux/macOS: `agentvfs`, `agentvfs-ctl`, `agentvfs-quickstart` (today's `start.sh` copied unchanged). Windows installs `agentvfs.exe` + `agentvfs-ctl.exe` only — `start.sh` is bash and `agentvfs workspace` is Linux-only, so a Windows quickstart wrapper would be meaningless.
6. **Print** install location, version, and the next command (`agentvfs-quickstart /path/to/project` on Linux/macOS; the two-line `agentvfs.exe --source ... --mountpoint Z:` + `agentvfs-ctl.exe ... checkpoint baseline` pair on Windows).

**Flags / env**: `--version <tag>` (`AGENTVFS_INSTALL_VERSION`), `--prefix <dir>` (`AGENTVFS_INSTALL_PREFIX`), `--help`. No other flags in v0.

## Release artifact format

```
agentvfs-v0.1.0-linux-x86_64.tar.gz
agentvfs-v0.1.0-darwin-arm64.tar.gz
agentvfs-v0.1.0-darwin-x86_64.tar.gz
agentvfs-v0.1.0-windows-x86_64.zip
agentvfs-v0.1.0-checksums.txt
```

Tag scheme: semver from day one. Archive contents are flat (no nested dir):

```
agentvfs            # daemon binary
agentvfs-ctl        # control client
agentvfs-quickstart # start.sh, renamed; Linux + macOS only
LICENSE
```

**Build flags per artifact**:

| Tuple | cmake flags |
|---|---|
| `linux-x86_64` | `-DAGENTVFS_EBPF=OFF` |
| `darwin-*` | `-DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON` |
| `windows-x86_64` | `-DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON` |

eBPF is off in every prebuilt because it needs `/sys/kernel/btf/vmlinux` at runtime. Power users build from source with `-DAGENTVFS_EBPF=ON`.

**Linux glibc floor**: builds run on `ubuntu-22.04` (glibc 2.35). Documented in the README. Older distros build from source; the dynamic linker will fail with a clear `GLIBC_2.35 not found` on first run if a user installs anyway, so no runtime probe is needed.

## CI workflow

New file `.github/workflows/release.yml`, triggered on `push: tags: ['v*']`. Existing `ci.yml` is untouched.

Five jobs:

1. `build-linux-x86_64` on `ubuntu-22.04` — cmake build, copy `start.sh` to `agentvfs-quickstart`, tar.
2. `build-darwin-arm64` on `macos-latest` — install fuse-t, cmake build, tar.
3. `build-darwin-x86_64` on `macos-13` — same.
4. `build-windows-x86_64` on `windows-latest` — install WinFsp via choco, cmake build, zip.
5. `publish` — downloads all artifacts, runs `sha256sum` to produce `checksums.txt`, `gh release create` with all five files attached.

## README restructure

### Above the fold

```
[logo]

Checkpointable, branchable FUSE workspace for AI agents.

## Install

# Linux / macOS
curl -fsSL https://raw.githubusercontent.com/thustorage/ContextFS/main/install.sh | sh

# Windows (PowerShell)
iwr -useb https://raw.githubusercontent.com/thustorage/ContextFS/main/install.ps1 | iex

## Mount a project

# Linux / macOS
agentvfs-quickstart /path/to/project

# Windows (PowerShell)
agentvfs.exe --source C:\some\dir --mountpoint Z:
agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

### Below the fold

1. **What you get** — existing feature table, unchanged.
2. **Driving the daemon directly** — existing `agentvfs workspace ...` block, unchanged.
3. **How it works** — existing ASCII diagram, unchanged.
4. **Platform support** — existing matrix with one row added: "Prebuilt installer" (✅ Linux x86_64 glibc ≥ 2.35 / ✅ macOS / ⚠️ Windows: no `agentvfs-quickstart`).
5. **Build from source** — today's per-OS cmake quickstarts, demoted, with `-DAGENTVFS_EBPF=ON` documented as the opt-in.
6. **License** — unchanged.

## Risks

1. **`curl … | sh` security pushback.** Mitigation: SHA-256 verification, HTTPS-only. Accepted.
2. **Linux glibc 2.35 floor.** Ubuntu 20.04 / RHEL 8 / Debian 11 users see `GLIBC_2.35 not found` at first run. Documented requirement; source-build remains the fallback. Accepted.
3. **FUSE runtime not auto-installed.** Users without libfuse3 / fuse-t / WinFsp see a mount-time error. README documents the install commands in the Platform support section. Accepted.
4. **Windows partial story.** Ships only `agentvfs.exe` + `agentvfs-ctl.exe`. Matches existing support matrix. Accepted.

## Success criteria

- README hero is two commands (install + run) with no prerequisites.
- All three OSes are reachable from the hero.
- Tagging `v0.1.0` produces a working release with no manual artifact uploads.
- Source-build users see no behavioral change: `./start.sh /path/to/project` still works.
