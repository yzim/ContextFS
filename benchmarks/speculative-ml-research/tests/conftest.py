import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

import pytest

from src.agentvfs import AgentVFS, daemon_argv, stop_daemon, wait_daemon_ready
from src.cgroup import CgroupSession, CgroupUnavailable

REPO = Path(__file__).resolve().parents[3]
AGENTVFS_BIN = REPO / "build" / "agentvfs"
CTL_BIN = REPO / "build" / "agentvfs-ctl"
RUN_BIN = REPO / "build" / "agentvfs-run"
CLIENT_LIB = REPO / "build" / "libagentvfs_runtime_client.so"

STUB_ARGV = [sys.executable, str(Path(__file__).parent / "stub_train.py")]


def require_coop():
    if not RUN_BIN.exists() or not CLIENT_LIB.exists():
        pytest.skip("agentvfs-run / runtime client lib missing "
                    "(cmake --build build -j)")


@dataclass
class DaemonHandle:
    mount: Path
    sock: str
    store: Path
    src: Path
    proc: subprocess.Popen


def make_avfs(daemon) -> AgentVFS:
    return AgentVFS(sock=daemon.sock, store_objects=daemon.store / "objects",
                    ctl_bin=CTL_BIN)


def cgroup_or_skip(name: str) -> CgroupSession:
    try:
        return CgroupSession(name)
    except CgroupUnavailable as e:
        pytest.skip(f"cgroup v2 unavailable: {e}")


def _missing_prereqs() -> str | None:
    if not AGENTVFS_BIN.exists() or not CTL_BIN.exists():
        return "build/agentvfs binaries missing (cmake -B build && cmake --build build -j)"
    if shutil.which("fusermount3") is None:
        return "fusermount3 not on PATH"
    return None


@pytest.fixture
def daemon(tmp_path):
    reason = _missing_prereqs()
    if reason:
        pytest.skip(reason)
    src = tmp_path / "src"
    mount = tmp_path / "mnt"
    store = tmp_path / "store"
    sock = str(tmp_path / "control.sock")
    src.mkdir()
    mount.mkdir()
    (src / "base.txt").write_text("base\n")
    proc = subprocess.Popen(
        daemon_argv(AGENTVFS_BIN, src, mount, store, sock) + ["-s"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT,
    )
    if not wait_daemon_ready(sock, mount / "base.txt"):
        proc.kill()
        pytest.skip("agentvfs daemon failed to start (FUSE unavailable?)")
    yield DaemonHandle(mount=mount, sock=sock, store=store, src=src, proc=proc)
    stop_daemon(proc, mount)
