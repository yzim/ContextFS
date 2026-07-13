"""Offline unit tests for the self-enrolling cgroup v2 session.

These exercise the launch contract (child writes its own pid into
cgroup.procs before exec) and the kill-before-remove cleanup ordering
without touching a real cgroup.
"""

import os
import subprocess
from types import SimpleNamespace

import pytest

from src.cgroup import CgroupSession, CgroupUnavailable, _ENROLL


def test_cgroup_child_self_enrolls_before_exec(monkeypatch, tmp_path):
    calls = []
    process = SimpleNamespace(args=None)

    def fake_popen(argv, **kwargs):
        calls.append((argv, kwargs))
        process.args = argv
        return process

    monkeypatch.setattr(subprocess, "Popen", fake_popen)
    session = object.__new__(CgroupSession)
    session.path = str(tmp_path / "cg")
    session.children = []
    session.run(["python", "worker.py"], cwd=tmp_path)
    argv = calls[0][0]
    assert "cgroup.procs" in argv[2]
    assert argv[-2:] == ["python", "worker.py"]


def test_enroll_constant_writes_self_into_procs():
    """The launch contract: bash positional $0 is the cgroup path; $$ is the
    child pid that lands in cgroup.procs before exec of the real argv."""
    assert "cgroup.procs" in _ENROLL
    assert "$0" in _ENROLL
    assert 'exec "$@"' in _ENROLL


def test_run_appends_child_and_starts_new_session(monkeypatch, tmp_path):
    """run() records the spawned process and uses a new session so its pgid
    is independently killable via os.killpg."""
    seen = {}
    proc = SimpleNamespace(pid=4242)

    def fake_popen(argv, **kwargs):
        seen["argv"] = argv
        seen["kwargs"] = kwargs
        return proc

    monkeypatch.setattr(subprocess, "Popen", fake_popen)
    session = object.__new__(CgroupSession)
    session.path = str(tmp_path / "cg")
    session.children = []
    returned = session.run(["echo", "hi"], cwd=tmp_path)
    assert returned is proc
    assert session.children == [proc]
    assert seen["kwargs"]["start_new_session"] is True


def test_destroy_kills_before_remove(monkeypatch, tmp_path):
    """destroy() must call kill_all BEFORE attempting os.rmdir, then remove."""
    order = []
    cg = tmp_path / "cg"
    cg.mkdir()
    session = object.__new__(CgroupSession)
    session.path = str(cg)
    session.children = []

    real_rmdir = os.rmdir

    def spy_kill_all():
        order.append("kill")

    def spy_rmdir(path):
        order.append("rmdir")
        return real_rmdir(path)

    monkeypatch.setattr(session, "kill_all", spy_kill_all)
    monkeypatch.setattr("os.rmdir", spy_rmdir)
    assert session.destroy() is True
    assert order == ["kill", "rmdir"]
    assert not cg.exists()


def test_destroy_returns_false_when_directory_remains(tmp_path):
    """A leftover directory (kill_all could not drain it) is a cleanup
    FAILURE signaled by False, not a warning."""
    cg = tmp_path / "cg"
    cg.mkdir()
    (cg / "stuck-pid-file").write_text("1")  # non-empty => rmdir fails
    session = object.__new__(CgroupSession)
    session.path = str(cg)
    session.children = []
    session.kill_all = lambda: None  # no-op; leave cgroup populated
    assert session.destroy() is False
    assert cg.exists()


def test_destroy_absent_path_is_idempotent_true(tmp_path):
    """Repeated destroy() on a path that never existed (or was already
    removed) returns True."""
    session = object.__new__(CgroupSession)
    session.path = str(tmp_path / "never-created")
    session.children = []
    session.kill_all = lambda: None
    assert session.destroy() is True
    assert session.destroy() is True


def test_cgroup_unavailable_when_no_writable_root(monkeypatch):
    """With no candidate roots, CgroupSession raises the skippable
    CgroupUnavailable signal rather than touching the filesystem."""
    monkeypatch.setattr("src.cgroup._candidates", lambda name: [])
    with pytest.raises(CgroupUnavailable):
        CgroupSession("worker")
