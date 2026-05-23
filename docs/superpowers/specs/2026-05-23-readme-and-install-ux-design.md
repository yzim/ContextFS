# README and install UX redesign

**Date:** 2026-05-23
**Status:** Draft, awaiting user review
**Scope:** README + new install.sh + install.ps1 + GitHub Releases workflow. No source-tree changes outside that.

## Problem

Today's README and install flow read as a research artifact rather than a product:

- Three OS-specific quickstarts with different commands (apt + cmake on Linux, brew + cmake on macOS, choco/manual + cmake on Windows).
- `cmake -B build && cmake --build build -j && sudo cmake --install build` is the install path — minutes of build time, requires sudo, requires the user to first install build deps.
- `AGENTVFS_EBPF=ON` is the cmake default. On a host without `bpftool` and `/sys/kernel/btf/vmlinux` the build fails with a cmake-time error, and the README's mitigation (`-DAGENTVFS_EBPF=OFF`) is footnoted rather than the default for casual users.
- `start.sh` requires `AGENTVFS_WORKSPACE_ROOT` to be exported by the user before running.
- The README leads with a feature table rather than a "what do I type" hero. Curious-dev visitors (the dominant audience) don't convert.

## Goals and non-goals

**Goals**:

1. Time-to-mount for a curious dev seeing the repo for the first time: under 30 seconds on a host that already has the FUSE runtime, under 90 seconds on a host that doesn't.
2. The README hero is two commands: install, then run. No env var prerequisites, no cmake, no sudo by default.
3. Linux, macOS, and Windows all reachable from the hero. Windows is the lower-tier platform (matches the existing support matrix) but doesn't look broken.
4. Existing source-build path remains fully supported for contributors and power users — demoted, not removed.

**Non-goals** (deferred or out of scope):

- Homebrew tap, apt repo, MSI bundle. One-line installer first; package-manager-native distribution later.
- Hosted landing page (e.g. `agentvfs.dev`). Use `raw.githubusercontent.com` URLs.
- Code signing (minisign / cosign), reproducible builds, SBOM.
- Self-update command (`agentvfs-quickstart --upgrade`). Reinstall via `curl … | sh` is the upgrade path.
- Scripted uninstall. Documented as `rm ~/.local/bin/agentvfs*`.
- Docker image distribution. FUSE-in-Docker is non-trivial and the curious-dev audience doesn't need it.
- aarch64 Linux artifact in v0. Added in a follow-up release.
- Renaming `start.sh` in the repo. The file stays where it is; install.sh copies it onto PATH under a new name (see § Quickstart rename).
- Changes to the `agentvfs` daemon, `agentvfs-ctl`, the `agentvfs workspace` CLI, the control-socket protocol, or `docs/skills/agentvfs-workspace.md`.
- Telemetry-backend enablement in prebuilts beyond what's stated below.
- Changes to `benchmarks/agent-sim/` and `demo/agentvfs-quickstart.sh`.

## Audience

Primary: a curious dev who saw a tweet or HN post, wants to install in under five minutes, mount a project, run an agent, and decide if it's worth a second look. Optimize the README hero for this user.

Secondary, both fully supported but addressed lower in the README: production users integrating AgentVFS into agent pipelines, and researchers reproducing benchmarks. Both keep their existing entry points (`Driving the daemon directly`, `benchmarks/agent-sim/run.sh`, the build-from-source section).

## Installer contract

Two scripts, one contract: `install.sh` (POSIX `sh`, Linux + macOS) and `install.ps1` (PowerShell 5+, Windows).

### Behavior

1. **Detect host.** `uname -s` / `uname -m` on POSIX; `$IsWindows` / `[System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture` on PowerShell. Supported tuples: `linux-x86_64`, `darwin-arm64`, `darwin-x86_64`, `windows-x86_64`. Anything else exits 1 with a "build from source" pointer to the README's source-build section.

2. **Resolve version.** Default: latest GitHub Release for `thustorage/ContextFS` via the public Releases API (no auth). Override with `--version vX.Y.Z` or `AGENTVFS_INSTALL_VERSION=vX.Y.Z`.

3. **Probe glibc on Linux** before download. Read `ldd --version` and refuse if glibc < 2.35 with a one-line "your glibc is too old; build from source" message and a link to the README's source-build section. This is the prebuilt's compatibility floor.

4. **Download** the artifact for the resolved tuple plus the checksums file:
   - `agentvfs-<version>-<os>-<arch>.tar.gz` (or `.zip` on Windows)
   - `agentvfs-<version>-checksums.txt`

5. **Verify checksum.** SHA-256 of the artifact against the value in the checksums file. Refuse to continue on mismatch.

6. **Probe FUSE runtime host dep** but never install it.
   - Linux: check for `libfuse3.so.3` via `ldconfig -p libfuse3.so.3` and the common `/usr/lib/x86_64-linux-gnu/libfuse3.so.3` / `/usr/lib64/libfuse3.so.3` paths.
   - macOS: check for `libfuse-t.dylib` under `/opt/homebrew/lib/` and `/usr/local/lib/`.
   - Windows: check for the WinFsp registry key `HKLM:\SOFTWARE\WinFsp` and `winfsp-x64.dll` under the WinFsp install dir.
   On miss, print the exact one-liner the user should run (`sudo apt install libfuse3-3`, `brew install --cask macos-fuse-t/cask/fuse-t`, or the WinFsp installer URL) and continue with a warning — the binary installs fine, it just won't mount until the runtime is present. The final success message reprints the warning so the user sees both commands together.

7. **Extract and install** into `--prefix <dir>` (default `~/.local/bin`; `--system` switches to `/usr/local/bin` and prompts for sudo). Three files land on PATH on Linux/macOS:
   - `agentvfs`
   - `agentvfs-ctl`
   - `agentvfs-quickstart` (today's `start.sh` copied unchanged)

   Windows installs only `agentvfs.exe` and `agentvfs-ctl.exe`. No `agentvfs-quickstart` on Windows (see § Quickstart rename).

8. **Final message.** A single block: install location, version, the next command, and the FUSE-host warning lines if any. The "next command" on Linux/macOS is `agentvfs-quickstart /path/to/project`; on Windows it's the two-line `agentvfs.exe --source ... --mountpoint Z:` + `agentvfs-ctl.exe ... checkpoint baseline` pair from the README hero.

### Flags and environment

- `--version <tag>` / `AGENTVFS_INSTALL_VERSION` — pin a release.
- `--prefix <dir>` / `AGENTVFS_INSTALL_PREFIX` — install destination.
- `--system` — install to `/usr/local/bin` with sudo (default is per-user `~/.local/bin`).
- `--yes` — skip the "about to install to X" interactive prompt; for CI use.
- `--dry-run` — print what would be done and exit 0 without touching disk.
- `--help`.

### Non-goals for the installer

- Installing libfuse3 / fuse-t / WinFsp on behalf of the user (silent-sudo smell, breaks the "show me what you'll do" principle).
- Self-update. Reinstall is `curl … | sh` again.
- Uninstall. `rm ~/.local/bin/agentvfs*` is the documented path.

## Release artifact format

### Naming

```
agentvfs-v0.1.0-linux-x86_64.tar.gz
agentvfs-v0.1.0-darwin-arm64.tar.gz
agentvfs-v0.1.0-darwin-x86_64.tar.gz
agentvfs-v0.1.0-windows-x86_64.zip
agentvfs-v0.1.0-checksums.txt
```

`checksums.txt` format is `<sha256>  <filename>` per line, compatible with `sha256sum -c`.

Tag scheme is **semver from day one**: `v0.1.0`, `v0.2.0`, etc.

### Archive contents (flat, no nested directory)

```
agentvfs            # the daemon binary
agentvfs-ctl        # the control client
agentvfs-quickstart # start.sh, renamed; Linux + macOS only
LICENSE
```

### Build flags per artifact

| Tuple | cmake flags |
|---|---|
| `linux-x86_64` | `-DAGENTVFS_EBPF=OFF` |
| `darwin-arm64`, `darwin-x86_64` | `-DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON` |
| `windows-x86_64` | `-DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON` |

eBPF is off in every prebuilt. Rationale: eBPF needs `/sys/kernel/btf/vmlinux` at runtime; baking it in would make the prebuilt fail or degrade on common machines (containers, older kernels, non-Debian distros). Power users who want eBPF still build from source. The "Build from source" README section documents `-DAGENTVFS_EBPF=ON` as the opt-in for eBPF telemetry.

### Glibc floor

Linux artifacts are built on `ubuntu-22.04` (glibc 2.35). Documented in the README's "Supported platforms" table. Older distros fall back to source-build; install.sh refuses with a clear message rather than letting the dynamic linker fail confusingly at first run.

## CI workflow

New file `.github/workflows/release.yml`, triggered on `push: tags: ['v*']`. Existing `.github/workflows/ci.yml` is untouched.

Jobs:

1. **build-linux-x86_64** (`runs-on: ubuntu-22.04`) — `cmake -B build -DAGENTVFS_EBPF=OFF && cmake --build build -j`. Copies `start.sh` to `agentvfs-quickstart`, packages `agentvfs`, `agentvfs-ctl`, `agentvfs-quickstart`, `LICENSE` into a flat tarball. Uploads as a workflow artifact.
2. **build-darwin-arm64** (`runs-on: macos-latest`) — `brew install --cask macos-fuse-t/cask/fuse-t`, then `cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_FUSE_T=ON && cmake --build build -j`. Same packaging.
3. **build-darwin-x86_64** (`runs-on: macos-13`, the last Intel runner) — same as above.
4. **build-windows-x86_64** (`runs-on: windows-latest`) — `choco install winfsp -y`, then `cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON && cmake --build build --config Release -j`. Packages `agentvfs.exe`, `agentvfs-ctl.exe`, `LICENSE` into a flat zip. No `agentvfs-quickstart` on Windows.
5. **publish** (depends on all four builds, `runs-on: ubuntu-latest`) — downloads all artifacts, runs `sha256sum` to produce `agentvfs-<version>-checksums.txt`, creates the GitHub Release via `gh release create`, attaches all five files.

No code-signing, no reproducible-build flags. Documented as future work.

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
# prints `mount=<path>` and a `cd <path> && claude` (or `codex`) hint

# Windows (PowerShell)
agentvfs.exe --source C:\some\dir --mountpoint Z:
agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

No feature table, no platform matrix, no diagram above the fold. The Windows "Mount a project" snippet is the same content as today's README Windows section — promoted into the hero so Windows users have a working next-step.

### Below the fold, in this order

1. **What you get** — the existing feature table (Checkpoint & Rollback, Per-Agent Branches, Content-Addressed Store, Pluggable Telemetry, Cross-Platform, Agent-CLI Skills). Content unchanged.
2. **Driving the daemon directly** — the existing `agentvfs workspace start/checkpoint/rollback/stop` block. Unchanged.
3. **How it works** — the existing ASCII diagram. Unchanged.
4. **Platform support** — the existing support matrix, with one row added: "Prebuilt binaries available (`install.sh`)" — ✅ Linux x86_64, ✅ macOS, ⚠️ Windows (no `agentvfs-quickstart`), and a note that the Linux prebuilt requires glibc ≥ 2.35.
5. **Build from source** — the current per-OS quickstart blocks (apt + cmake on Linux, fuse-t + cmake on macOS, WinFsp + cmake on Windows), reframed as the contributor / power-user path. Adds:
   - "Enable eBPF telemetry: `cmake -B build -DAGENTVFS_EBPF=ON …`" on the Linux block.
   - Pointer to `benchmarks/agent-sim/` for researchers reproducing results.
   - The `--system` / `--prefix` / `AGENTVFS_INSTALL_VERSION` documentation for install.sh.
6. **License** — unchanged.

## Quickstart rename

`start.sh` stays in the repo, unchanged. The release workflow copies it to `agentvfs-quickstart` (executable bit preserved) when packaging. install.sh extracts it to the install prefix.

Source-build users continue to run `./start.sh /path/to/project` from a checkout. Prebuilt-install users run `agentvfs-quickstart /path/to/project`. Both invoke the same script content.

Windows install.ps1 does not install `agentvfs-quickstart`. Rationale: `start.sh` is bash and relies on `agentvfs workspace`, which is Linux-only per the support matrix. A Windows quickstart wrapper would be meaningless until those features land. The README's Windows install block points at the existing `agentvfs-ctl … checkpoint baseline` example (already in the README today) as the equivalent demonstration.

The renamed file requires `bash`, not just `sh`. install.sh probes for `bash` on PATH on Linux and warns if absent (rare but possible on Alpine/BusyBox setups). Documented as a known limitation.

## Risks and accepted tradeoffs

1. **`curl … | sh` security pushback.** Some users object on principle. Mitigation: `--dry-run` prints the plan without touching disk; checksums are verified over HTTPS; the README documents `curl … > install.sh && less install.sh && sh install.sh` as the audit-then-run alternative. Accepted.

2. **Glibc 2.35 floor.** Ubuntu 20.04, RHEL 8, Debian 11 are excluded from the prebuilt path. install.sh refuses with a clear message, so the failure mode is informative rather than a confusing dynamic-linker error. Source-build remains available. Accepted.

3. **FUSE runtime not auto-installed.** install.sh prints the next command but doesn't run it. First-time users may still try to mount, see a "no fuse" error, and bounce. Mitigation: the final success message reprints the host-dep install command so the user sees both lines together. Accepted.

4. **GitHub Releases API rate limit.** Unauthenticated calls are limited to 60/hr per IP. install.sh makes one API call per run. Realistic at current usage; if traffic grows, switch to the `/releases/latest/download/<filename>` URL pattern which doesn't require an API call. Documented as a future-work mitigation.

5. **eBPF telemetry absent from prebuilts.** Anyone who installed via install.sh sees no eBPF backend. The "Build from source" README section documents the opt-in. Accepted.

6. **Windows partial story.** Windows ships `agentvfs.exe` + `agentvfs-ctl.exe` only. First-impression on Windows is worse than Linux/macOS, but matches the existing platform-support matrix's "Coming soon" cells. Not a regression. Accepted.

7. **`agentvfs-quickstart` requires bash.** Will not run on hosts without bash. install.sh warns on Linux if bash isn't present. Documented limitation. Accepted.

## Success criteria

- A first-time visitor can mount a project in under 30 seconds on a host with the FUSE runtime present, under 90 seconds on a host without.
- The README hero is two commands (install + run), no env-var prerequisites, no sudo by default.
- All three OSes are reachable from the hero (Linux/macOS via install.sh, Windows via install.ps1).
- Tagging a release (`git tag v0.1.0 && git push --tags`) produces published artifacts and a working install path with no manual steps.
- Source-build users see no behavioral change: `./start.sh /path/to/project` still works exactly as today.
