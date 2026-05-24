<p align="center">
  <img src="https://github.com/thustorage/ContextFS/raw/main/docs/agentvfs-logo.svg" alt="AgentVFS" width="400">
</p>

<p align="center"><strong>Checkpoint &amp; Rollback · Per-Agent Branch Views · Pluggable Telemetry</strong></p>

<p align="center">A checkpointable, branchable, and content-addressed FUSE filesystem for AI agents.</p>

## What you get

| Feature | Description |
|---------|-------------|
| ⏪&nbsp;**Checkpoint&nbsp;&amp;&nbsp;Rollback** | Snapshot the working tree and roll back to any prior checkpoint |
| 🌿&nbsp;**Per&#8209;Agent&nbsp;Branches** | N agents over one source tree, routed by cgroup v2; three-way merge or surfaced conflicts |
| 🔗&nbsp;**Content&#8209;Addressed&nbsp;Store** | blake3-hashed objects deduplicate across checkpoints and branches — near-zero-cost snapshots |
| 🛰️&nbsp;**Pluggable&nbsp;Telemetry** | NDJSON audit via eBPF / fanotify / ptrace / `LD_PRELOAD`; Wasm or Lua to filter and verdict |
| 🖥️&nbsp;**Cross&#8209;Platform** | libfuse3 (Linux), fuse-t (macOS), WinFsp (Windows) |
| 🤖&nbsp;**Agent&#8209;CLI&nbsp;Skills** | `agentvfs-quickstart` mounts a project and installs a Skill for Claude Code / Codex to checkpoint and roll back |

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
# Linux (openEuler)
dnf install -y cmake clang fuse3-devel bpftool libbpf-devel
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
# Windows — requires WinFsp 2.0+ from https://winfsp.dev,
# and MSVC v141 (Visual Studio 2017 15.3) or newer for /arch:AVX512
# codegen on the BLAKE3 SIMD path (current VS releases all qualify).
cmake -B build -DAGENTVFS_EBPF=OFF -DAGENTVFS_WINFSP=ON
cmake --build build --config Release -j
.\build\Release\agentvfs.exe --source C:\some\dir --mountpoint Z:
.\build\Release\agentvfs-ctl.exe --sock \\.\pipe\agentvfs-<hash> checkpoint baseline
```

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE).
