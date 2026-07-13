"""Offline unit tests for the AgentVFS control wrapper.

All tests monkeypatch ``src.agentvfs.subprocess.run`` so no real
``agentvfs-ctl``, daemon, FUSE mount, or cgroup is required.
"""

from pathlib import Path
from types import SimpleNamespace

import pytest

from src.agentvfs import (AgentVFS, CtlError, VerbRecord, daemon_argv,
                          stop_daemon, wait_daemon_ready)


@pytest.fixture
def fake_run(monkeypatch):
    """One mutable subprocess.run result shared across all calls in a test.

    Monkeypatches ``src.agentvfs.subprocess.run`` so both ``_ctl`` and
    ``store_bytes`` see the same namespace. Tests mutate ``returncode`` /
    ``stdout`` / ``stderr`` between calls to script distinct invocations.
    """
    ns = SimpleNamespace(returncode=0, stdout="", stderr="")

    def _run(*args, **kwargs):
        return ns

    # ``src.agentvfs.subprocess`` is the stdlib subprocess module imported
    # into the wrapper; patching its ``run`` attribute is what the brief
    # means by "monkeypatches src.agentvfs.subprocess.run".
    from src import agentvfs
    monkeypatch.setattr(agentvfs.subprocess, "run", _run)
    return ns


def test_branch_commands_and_json_list(fake_run, tmp_path):
    fake_run.stdout = '{"branches":[{"name":"main"},{"name":"spec-2-0"}]}'
    avfs = AgentVFS("/tmp/control.sock", tmp_path / "objects", Path("ctl"))
    assert [b["name"] for b in avfs.branch_list()] == ["main", "spec-2-0"]
    avfs.branch_create("spec-2-0", step=2)
    assert avfs.verbs[-1].verb == "branch_create"


def test_control_error_records_failure(fake_run, tmp_path):
    fake_run.returncode = 1
    fake_run.stderr = "no such branch"
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    with pytest.raises(CtlError, match="no such branch"):
        avfs.branch_delete("missing", step=3)
    assert avfs.verbs[-1].status == "error"


def test_verb_record_carries_latency_and_step(fake_run, tmp_path):
    """A successful control call appends a VerbRecord with sane fields."""
    fake_run.stdout = "ok"
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    avfs.branch_create("spec-3-0", step=3)
    rec = avfs.verbs[-1]
    assert isinstance(rec, VerbRecord)
    assert rec.verb == "branch_create"
    assert rec.step == 3
    assert rec.status == "ok"
    assert isinstance(rec.latency_us, int)
    assert rec.latency_us >= 0


def test_session_register_tracks_active_sessions(fake_run, tmp_path):
    """Registration adds the cgroup path; unregistration removes it. Only a
    successful ctl mutates the set (the _ctl raise guards the add/discard)."""
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    avfs.session_register("/cg/cand-1", 1, "spec-1", step=4)
    avfs.session_register("/cg/cand-2", 2, "spec-2", step=4)
    assert avfs.active_sessions == {"/cg/cand-1", "/cg/cand-2"}
    avfs.session_unregister("/cg/cand-1", step=4)
    assert avfs.active_sessions == {"/cg/cand-2"}
    assert [(record.verb, record.step) for record in avfs.verbs] == [
        ("session_register", 4),
        ("session_register", 4),
        ("session_unregister", 4),
    ]


def test_session_register_not_added_when_ctl_fails(fake_run, tmp_path):
    """A failed register must not pollute active_sessions."""
    fake_run.returncode = 1
    fake_run.stderr = "branch not found"
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    with pytest.raises(CtlError):
        avfs.session_register("/cg/cand-9", 9, "nope")
    assert avfs.active_sessions == set()


def test_branch_list_accepts_bare_list_envelope(fake_run, tmp_path):
    """branch_list tolerates both {branches:[...]} and a bare list payload."""
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    fake_run.stdout = '[{"name":"main"},{"name":"spec-1-0"}]'
    assert [b["name"] for b in avfs.branch_list()] == ["main", "spec-1-0"]


def test_status_parses_json(fake_run, tmp_path):
    fake_run.stdout = '{"ok": true, "sessions": 2}'
    avfs = AgentVFS("sock", tmp_path / "objects", Path("ctl"))
    assert avfs.status() == {"ok": True, "sessions": 2}


# --- daemon lifecycle helpers (offline contract for the Task 7 fixture) -----


def test_daemon_argv_shape():
    """daemon_argv mirrors the speculative-ml-research foreground contract."""
    argv = daemon_argv("/p/agentvfs", "/src", "/mnt", "/store", "/sock")
    assert argv[0] == "/p/agentvfs"
    assert "--source" in argv and "/src" in argv
    assert "--mountpoint" in argv and "/mnt" in argv
    assert "--store" in argv and "/store" in argv
    assert "--control-sock" in argv and "/sock" in argv
    assert argv[-1] == "-f"


def test_wait_daemon_ready_requires_socket_and_sentinel(tmp_path):
    """Ready iff the control socket is reachable AND the seeded sentinel
    exists. Uses one try so the test is instantaneous when both are present."""
    sock = tmp_path / "ctl.sock"
    sock.touch()
    sentinel = tmp_path / ".seeded"
    sentinel.touch()
    # is_socket() is False for a plain file, so readiness is False even though
    # the sentinel exists -- proving both conditions are required.
    assert wait_daemon_ready(str(sock), sentinel, tries=1, interval=0) is False


def test_stop_daemon_partial_startup_returns_none(tmp_path):
    """proc is None (never started) and mount path not actually mounted =>
    full success, returns None, never raises."""
    assert stop_daemon(None, tmp_path / "never-mounted") is None


def test_stop_daemon_terminates_running_proc(tmp_path):
    """A still-running daemon is SIGTERM'd and reaped; unmounted path skips
    fusermount3; the teardown returns None (full success)."""
    events = []
    proc = SimpleNamespace(
        pid=1234,
        poll=lambda: None,                       # still running
        terminate=lambda: events.append("term"),
        wait=lambda timeout=None: events.append("wait") or 0,  # reaps clean
        kill=lambda: events.append("kill"),
    )
    mount = tmp_path / "mnt"
    mount.mkdir()  # plain dir => os.path.ismount False => fusermount skipped
    assert stop_daemon(proc, mount) is None
    assert events == ["term", "wait"]
    assert "kill" not in events


def test_stop_daemon_unmounts_when_mounted(monkeypatch, fake_run):
    """When the path IS mounted, fusermount3 -u is issued; success => None."""
    monkeypatch.setattr("src.agentvfs.os.path.ismount", lambda p: True)
    fake_run.returncode = 0
    assert stop_daemon(None, "/mnt/x") is None


def test_stop_daemon_returns_diag_on_unmount_failure(monkeypatch, fake_run):
    """A failed fusermount3 yields exactly one diagnostic string (never an
    exception), so callers can log incomplete cleanup without trapping."""
    monkeypatch.setattr("src.agentvfs.os.path.ismount", lambda p: True)
    fake_run.returncode = 1
    fake_run.stderr = "device busy"
    diag = stop_daemon(None, "/mnt/x")
    assert isinstance(diag, str)
    assert "fusermount3" in diag
    assert "device busy" in diag
