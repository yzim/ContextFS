"""Shared pytest fixtures and the AgentVFS prerequisite checker for the
speculative-chess benchmark integration tests.

The ``daemon`` fixture mirrors the daemon-lifecycle boundary established in
:mod:`src.agentvfs` (foreground launch via :func:`daemon_argv`, the readiness
condition of a reachable control socket plus a seeded mount sentinel, and
``fusermount3`` teardown via :func:`stop_daemon`) so the integration test
exercises the same protocol the CLI entrypoint uses.

``missing_agentvfs_prerequisite`` is the gate: it skips only for genuinely
missing platform prerequisites (binaries, ``fusermount3``, ``/dev/fuse`` access,
cgroup delegation). Once those checks pass, daemon-start, JSON preflight,
routing, cleanup, and unmount failures are test failures with diagnostics,
never skips.
"""

import os
import shutil
import subprocess
from dataclasses import dataclass
from pathlib import Path

import pytest

from src.agentvfs import daemon_argv, stop_daemon, wait_daemon_ready
from src.candidate_worker import CleanupError
from src.cgroup import CgroupSession, CgroupUnavailable
from src.game import GameState

# Binaries are resolved from the config.yml convention (``../../build/agentvfs``
# relative to benchmarks/speculative-chess/), i.e. the repository-root build
# directory. Kept as module constants so the fixture and the prerequisite check
# agree with load_config's path resolution.
_HERE = Path(__file__).resolve().parent          # benchmarks/speculative-chess/tests
_PROJECT = _HERE.parent                            # benchmarks/speculative-chess
AGENTVFS_BIN = (_PROJECT / "../../build/agentvfs").resolve()
CTL_BIN = (_PROJECT / "../../build/agentvfs-ctl").resolve()


@dataclass
class DaemonHandle:
    """A running daemon: the FUSE mount (runner's root), the control socket,
    the CAS store, the backing source directory, and the daemon process."""
    mount: Path
    sock: str
    store: Path
    source: Path
    proc: subprocess.Popen


def missing_agentvfs_prerequisite(agentvfs_bin=None, ctl_bin=None) -> str | None:
    """Return a human skip-reason for the first missing AgentVFS prerequisite,
    or None when every prerequisite is satisfied.

    Checked in order: the built binaries (resolved from the config's path
    convention), ``fusermount3`` on PATH, ``/dev/fuse`` present, read/write
    access to ``/dev/fuse``, then cgroup delegation (a probe
    :class:`CgroupSession`, destroyed on success). The first failing check wins;
    once all pass, the caller proceeds to start the daemon for real.
    """
    agentvfs_bin = Path(agentvfs_bin or AGENTVFS_BIN)
    ctl_bin = Path(ctl_bin or CTL_BIN)

    if not agentvfs_bin.exists():
        return f"missing agentvfs binary: {agentvfs_bin}"
    if not ctl_bin.exists():
        return f"missing agentvfs-ctl binary: {ctl_bin}"
    if shutil.which("fusermount3") is None:
        return "fusermount3 not found on PATH"

    fuse = Path("/dev/fuse")
    if not fuse.exists():
        return "/dev/fuse does not exist"
    if not (os.access(str(fuse), os.R_OK) and os.access(str(fuse), os.W_OK)):
        return f"insufficient read/write access to {fuse}"

    # Cgroup delegation: reuse the same search CgroupSession uses in production.
    # In a delegation-capable environment this briefly creates + destroys a real
    # cgroup; in this non-root environment it raises CgroupUnavailable, which is
    # the documented skip reason (a platform prerequisite, not a test failure).
    try:
        session = CgroupSession("chess-preflight")
    except CgroupUnavailable as exc:
        return f"cgroup delegation unavailable: {exc}"
    session.destroy()
    return None


@pytest.fixture
def daemon(tmp_path):
    """Start a real AgentVFS daemon over a seeded game and yield a handle.

    Skips cleanly when a platform prerequisite is missing. Once prerequisites
    pass, a daemon-start failure or a body failure is a hard failure with
    diagnostics; teardown always runs and a cleanup failure is chained onto the
    original failure rather than masking it.
    """
    reason = missing_agentvfs_prerequisite()
    if reason:
        pytest.skip(reason)
    source, mount, store = (tmp_path / "src", tmp_path / "mnt",
                            tmp_path / "store")
    source.mkdir()
    mount.mkdir()
    GameState.initial().save(source / "game.json")
    sock = str(tmp_path / "control.sock")
    log = tmp_path / "daemon.log"
    stream = log.open("wb")
    proc = subprocess.Popen(
        daemon_argv(AGENTVFS_BIN, source, mount, store, sock),
        stdout=stream, stderr=subprocess.STDOUT)
    body_error = None
    try:
        if not wait_daemon_ready(sock, mount / "game.json"):
            stream.flush()
            pytest.fail(
                "agentvfs daemon failed after prerequisites passed:\n"
                + log.read_text(errors="replace")[-4096:])
        yield DaemonHandle(mount, sock, store, source, proc)
    except BaseException as exc:
        body_error = exc
    finally:
        cleanup_error = stop_daemon(proc, mount)
        stream.close()
        if os.path.ismount(str(mount)) and not cleanup_error:
            cleanup_error = "mount remains after stop_daemon"
        if body_error is not None:
            if cleanup_error:
                raise body_error from CleanupError(cleanup_error)
            raise body_error
        if cleanup_error:
            pytest.fail(cleanup_error)


@pytest.fixture
def cgroup_or_skip():
    """Yield a real :class:`CgroupSession`, skipping when delegation is
    unavailable. The session is destroyed on teardown."""
    try:
        session = CgroupSession("chess-test")
    except CgroupUnavailable as exc:
        pytest.skip(f"cgroup delegation unavailable: {exc}")
    try:
        yield session
    finally:
        session.destroy()
