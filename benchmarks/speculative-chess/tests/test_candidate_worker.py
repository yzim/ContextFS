"""Offline unit tests for the candidate worker, attempt lifecycle, and cleanup.

Every test uses local fake process/cgroup/AgentVFS objects plus the supplied
:class:`~tests.fakes.ScriptedActor`; no real daemon, FUSE mount, cgroup,
network, or root is required. The two ``make_attempt`` helpers build a
launched-shape :class:`CandidateAttempt` from those fakes so cleanup ordering
is exercised exactly as the real lifecycle would observe it.
"""

import json
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from src.candidate_worker import (CandidateAttempt, CandidateFactory,
                                  CandidateResult, CandidateWorkError,
                                  CleanupError, PreparedResponse, run_candidate,
                                  main)
from src.cgroup import CgroupUnavailable
from src.agentvfs import CtlError
from src.game import GameState, StateError
from src.llm import LLMOutputError, MoveResult
from tests.fakes import ScriptedActor


# --- run_candidate: state mutation, prepared response, validation ------------


def test_run_candidate_writes_predicted_state_and_prepared_response(tmp_path):
    GameState.initial().save(tmp_path / "game.json")
    actor = ScriptedActor([MoveResult("e7e5", 0.2, 7, 2)])
    result = run_candidate(tmp_path, "e2e4", actor)
    state = GameState.load(tmp_path / "game.json")
    prepared = PreparedResponse.load(tmp_path / "prepared_response.json")
    assert state.moves == ("e2e4",)
    assert prepared.predicted_move == "e2e4"
    assert prepared.prepared_move == "e7e5"
    assert prepared.position_fen == state.fen
    assert result.tokens_in == 7


def test_run_candidate_result_carries_opposing_actor_metadata(tmp_path):
    """The CandidateResult mirrors the opposing Actor's reply move and token
    counts, and its position_fen is the post-prediction FEN (not pre-)."""
    GameState.initial().save(tmp_path / "game.json")
    actor = ScriptedActor([MoveResult("e7e5", 0.5, 11, 3)])
    result = run_candidate(tmp_path, "e2e4", actor)
    state = GameState.load(tmp_path / "game.json")
    assert result.predicted_move == "e2e4"
    assert result.prepared_move == "e7e5"
    assert result.position_fen == state.fen
    assert (result.tokens_in, result.tokens_out) == (11, 3)
    assert result.latency_s == pytest.approx(0.5)


def test_run_candidate_rejects_illegal_prepared_move_and_writes_nothing(tmp_path):
    """An illegal opposing reply raises StateError before any file is written,
    mirroring GameState.apply legality. d2d4 is a white-pawn move, illegal for
    black-to-move after e2e4."""
    GameState.initial().save(tmp_path / "game.json")
    actor = ScriptedActor([MoveResult("d2d4", 0.1, 1, 1)])
    with pytest.raises(StateError):
        run_candidate(tmp_path, "e2e4", actor)
    # Nothing committed on failure: game.json unchanged, no prepared response.
    assert not (tmp_path / "prepared_response.json").exists()
    assert GameState.load(tmp_path / "game.json").moves == ()


def test_run_candidate_propagates_illegal_prediction(tmp_path):
    """An illegal predicted_move surfaces GameState.apply's StateError."""
    GameState.initial().save(tmp_path / "game.json")
    actor = ScriptedActor([MoveResult("e7e5", 0.1, 1, 1)])
    with pytest.raises(StateError):
        run_candidate(tmp_path, "e7e5", actor)  # black move illegal for white


# --- PreparedResponse load / validate ---------------------------------------


def test_prepared_response_load_rejects_corrupt_file(tmp_path):
    """Corrupt JSON surfaces as StateError, never a bare JSONDecodeError."""
    path = tmp_path / "prepared_response.json"
    path.write_text("{not json")
    with pytest.raises(StateError):
        PreparedResponse.load(path)


def test_prepared_response_load_rejects_wrong_schema(tmp_path):
    path = tmp_path / "prepared_response.json"
    path.write_text(json.dumps(
        {"schema": 99, "predicted_move": "e2e4", "prepared_move": "e7e5",
         "position_fen": "x"}))
    with pytest.raises(StateError, match="schema"):
        PreparedResponse.load(path)


def test_prepared_response_save_round_trips_atomically(tmp_path):
    prepared = PreparedResponse(1, "e2e4", "e7e5", "fen")
    target = tmp_path / "sub" / "prepared_response.json"
    prepared.save(target)
    assert PreparedResponse.load(target) == prepared


def test_prepared_response_validate_against_rejects_position_mismatch():
    prepared = PreparedResponse(1, "e2e4", "g1f3", "bogus-fen")
    with pytest.raises(StateError, match="mismatch"):
        prepared.validate_against(GameState.initial())


def test_prepared_response_validate_against_rejects_illegal_move():
    """At the start position e7e5 is a black-pawn move, illegal for white."""
    state = GameState.initial()
    prepared = PreparedResponse(1, "e2e4", "e7e5", state.fen)
    with pytest.raises(StateError, match="illegal"):
        prepared.validate_against(state)


def test_prepared_response_validate_against_accepts_legal_reply():
    state = GameState.initial().apply("e2e4")  # black to move
    prepared = PreparedResponse(1, "e2e4", "e7e5", state.fen)
    prepared.validate_against(state)  # must not raise


# --- cleanup: exact order, idempotency, retry semantics ---------------------
# The two helpers below build a launched-shape attempt from small fake process,
# cgroup, and AgentVFS objects. Their only behavior is appending the four
# cleanup event names (kill / unregister / destroy / delete) in call order and,
# for the second helper, returning False from the first destroy() call.


class _FakeCgroup:
    """Records kill/destroy events and an optional scripted destroy sequence."""

    def __init__(self, events=None, path="/tmp/absent-cg-chess-spec",
                 destroy_returns=None):
        self.path = path
        self.children = []
        self._events = events
        self._destroy_returns = list(destroy_returns or [])
        self._destroy_calls = 0

    def kill_all(self):
        if self._events is not None:
            self._events.append("kill")

    def destroy(self):
        self._destroy_calls += 1
        if self._events is not None:
            self._events.append("destroy")
        if self._destroy_calls <= len(self._destroy_returns):
            return self._destroy_returns[self._destroy_calls - 1]
        return True


class _FakeAgentVFS:
    """Records unregister/delete events; tracks created/active sessions."""

    def __init__(self, events=None):
        self._events = events
        self.active_sessions = set()
        self.branches = []
        self.deleted_branches = []

    def branch_create(self, name, step=-1):
        self.branches.append(name)

    def branch_delete(self, name, step=-1):
        if self._events is not None:
            self._events.append("delete")
        self.deleted_branches.append(name)
        if name in self.branches:
            self.branches.remove(name)

    def session_register(self, cgroup_path, session_id, branch, step=-1):
        self.active_sessions.add(cgroup_path)

    def session_unregister(self, cgroup_path, step=-1):
        if self._events is not None:
            self._events.append("unregister")
        self.active_sessions.discard(cgroup_path)

    def branch_list(self):
        return [{"name": n} for n in self.branches]

    def status(self):
        return {"ok": True}


def make_attempt(events, predicted_move="e2e4"):
    """A launched-shape CandidateAttempt backed by recording fakes."""
    cg = _FakeCgroup(events=events)
    avfs = _FakeAgentVFS(events=events)
    attempt = CandidateAttempt(
        avfs=avfs, cgroup=cg, branch="chess-spec-0-0", session_id=10_000,
        mount=Path("/tmp/mnt"), worker_argv=["python", "worker.py"],
        worker_env={}, results_dir=Path("/tmp/res"),
        predicted_move=predicted_move)
    attempt.process = SimpleNamespace(pid=1111, poll=lambda: 0,
                                      wait=lambda timeout=None: 0)
    attempt.process_live = True
    attempt.session_registered = True
    attempt.cgroup_created = True
    attempt.branch_created = True
    return attempt


def make_attempt_with_first_cgroup_remove_failure():
    """Like make_attempt, but the cgroup's first destroy() returns False."""
    events: list[str] = []
    cg = _FakeCgroup(events=events, destroy_returns=[False])
    avfs = _FakeAgentVFS(events=events)
    attempt = CandidateAttempt(
        avfs=avfs, cgroup=cg, branch="chess-spec-0-0", session_id=10_000,
        mount=Path("/tmp/mnt"), worker_argv=["python", "worker.py"],
        worker_env={}, results_dir=Path("/tmp/res"), predicted_move="e2e4")
    attempt.process = SimpleNamespace(pid=2222, poll=lambda: 0,
                                      wait=lambda timeout=None: 0)
    attempt.process_live = True
    attempt.session_registered = True
    attempt.cgroup_created = True
    attempt.branch_created = True
    return attempt


def test_attempt_cleanup_has_fixed_order_and_is_idempotent():
    events = []
    attempt = make_attempt(events)
    assert attempt.cleanup() is True
    assert events == ["kill", "unregister", "destroy", "delete"]
    assert attempt.cleanup() is True
    assert events == ["kill", "unregister", "destroy", "delete"]


def test_attempt_cleanup_retries_only_unfinished_resources():
    attempt = make_attempt_with_first_cgroup_remove_failure()
    assert attempt.cleanup() is False
    assert attempt.cgroup_created
    assert attempt.branch_created
    assert attempt.cleanup() is True
    assert not attempt.cgroup_created
    assert not attempt.branch_created


class _ReapSpyProcess:
    """A fake Popen that records wait() calls -- the reap the parent must do
    to clear a zombie left by SIGKILL. poll()/communicate() are present to
    mirror the real Popen shape but are unused here."""

    def __init__(self):
        self.wait_calls = []
        self.returncode = 0

    def wait(self, timeout=None):
        self.wait_calls.append(timeout)
        return 0

    def poll(self):
        return self.returncode

    def communicate(self, timeout=None):
        return (b"", b"")


def test_attempt_cleanup_reaps_launched_process_without_collect():
    """Regression (I1): on the hit path, non-matching siblings are cleaned via
    require_cleanup WITHOUT ever being collected, so cleanup() -- not collect()
    -- must reap the launched process. An unreaped zombie stays listed in
    cgroup.procs, so CgroupSession.destroy()'s rmdir fails (EBUSY) and the
    bounded drain loop in kill_all cannot clear it (only the parent reaping
    can). This fails under the old code (no wait() call) and passes after the
    bounded reap is added to the cleanup kill step."""
    attempt = make_attempt(events=[])  # process_live=True, cgroup gone
    spy = _ReapSpyProcess()
    attempt.process = spy  # launched but NOT collected
    assert attempt.process_live
    assert attempt.cleanup() is True  # cgroup reports gone -> all clear
    assert spy.wait_calls, "cleanup() must reap the launched process"
    assert not attempt.process_live


# --- CandidateAttempt.collect: subprocess result parsing --------------------


class _FakeProcess:
    """A stand-in for a launched Popen with canned communicate() output."""

    def __init__(self, returncode=0, stdout=b"", stderr=b""):
        self.returncode = returncode
        self._stdout = stdout
        self._stderr = stderr

    def communicate(self, timeout=None):
        return (self._stdout, self._stderr)

    def poll(self):
        return self.returncode


def _result_record(**overrides):
    base = {"predicted_move": "e2e4", "prepared_move": "e7e5",
            "position_fen": "rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w",
            "latency_s": 0.2, "tokens_in": 7, "tokens_out": 2}
    base.update(overrides)
    return base


def test_attempt_collect_returns_candidate_result_from_single_record():
    attempt = make_attempt(events=[])
    record = json.dumps(_result_record()) + "\n"
    attempt.process = _FakeProcess(returncode=0, stdout=record.encode())
    attempt.process_live = True
    result = attempt.collect(timeout_s=5)
    assert isinstance(result, CandidateResult)
    assert result.predicted_move == "e2e4"
    assert result.prepared_move == "e7e5"
    assert result.tokens_in == 7
    # collect reaped the process; kill step will be skipped on cleanup.
    assert attempt.process_live is False


def test_attempt_collect_rejects_nonzero_exit():
    attempt = make_attempt(events=[])
    attempt.process = _FakeProcess(returncode=1, stdout=b"")
    attempt.process_live = True
    with pytest.raises(CandidateWorkError, match="exit"):
        attempt.collect(timeout_s=5)


def test_attempt_collect_rejects_missing_stdout_record():
    attempt = make_attempt(events=[])
    attempt.process = _FakeProcess(returncode=0, stdout=b"")
    attempt.process_live = True
    with pytest.raises(CandidateWorkError, match="1 stdout record"):
        attempt.collect(timeout_s=5)


def test_attempt_collect_rejects_multiple_stdout_records():
    attempt = make_attempt(events=[])
    record = json.dumps(_result_record()) + "\n"
    attempt.process = _FakeProcess(returncode=0, stdout=(record + record).encode())
    attempt.process_live = True
    with pytest.raises(CandidateWorkError, match="1 stdout record"):
        attempt.collect(timeout_s=5)


def test_attempt_collect_rejects_invalid_json():
    attempt = make_attempt(events=[])
    attempt.process = _FakeProcess(returncode=0, stdout=b"not-json\n")
    attempt.process_live = True
    with pytest.raises(CandidateWorkError, match="JSON"):
        attempt.collect(timeout_s=5)


def test_attempt_collect_rejects_prediction_mismatch():
    attempt = make_attempt(events=[], predicted_move="e2e4")
    record = json.dumps(_result_record(predicted_move="d2d4")) + "\n"
    attempt.process = _FakeProcess(returncode=0, stdout=record.encode())
    attempt.process_live = True
    with pytest.raises(CandidateWorkError, match="mismatch"):
        attempt.collect(timeout_s=5)


# --- CandidateFactory.create: naming, ids, partial cleanup ------------------


def _make_cgroup_factory(destroy_returns=None):
    """A cgroup factory producing _FakeCgroup instances (default destroy True)."""
    def factory(name):
        return _FakeCgroup(path=f"/tmp/cg-{name}",
                           destroy_returns=destroy_returns)
    return factory


def test_candidate_factory_create_uses_canonical_names_and_ids():
    avfs = _FakeAgentVFS()
    factory = CandidateFactory(
        avfs=avfs, mount=Path("/mnt"), results_dir=Path("/tmp/res"),
        cgroup_factory=_make_cgroup_factory(),
        worker_argv=["python", "worker.py", "--model", "m"],
        worker_env={"OPENAI_API_KEY": "sk-x"})
    attempt = factory.create(step=2, index=3, predicted_move="e2e4")
    assert attempt.branch == "chess-spec-2-3"
    assert attempt.session_id == 10_000 + 2 * 64 + 3
    assert "chess-spec-2-3" in avfs.branches
    assert f"/tmp/cg-chess-cand-2-3" in attempt.cgroup.path
    # Unlaunched: no process, but branch/cgroup/session are owned.
    assert attempt.process_live is False
    assert attempt.process is None
    assert attempt.branch_created
    assert attempt.cgroup_created
    assert attempt.session_registered
    # The session was registered with the daemon.
    assert attempt.cgroup.path in avfs.active_sessions


def test_candidate_factory_create_cleans_partial_state_on_cgroup_failure():
    """If cgroup construction fails after branch_create, the branch is cleaned
    and CandidateWorkError is raised (resources do not leak)."""
    avfs = _FakeAgentVFS()

    def failing_factory(name):
        raise CgroupUnavailable("no writable cgroup root")

    factory = CandidateFactory(
        avfs=avfs, mount=Path("/mnt"), results_dir=Path("/tmp/res"),
        cgroup_factory=failing_factory, worker_argv=[], worker_env={})
    with pytest.raises(CandidateWorkError):
        factory.create(step=0, index=0, predicted_move="e2e4")
    # branch_create ran, then partial cleanup deleted the branch.
    assert "chess-spec-0-0" in avfs.deleted_branches
    assert avfs.branches == []
    assert avfs.active_sessions == set()


def test_candidate_factory_create_propagates_ctl_error():
    """A CtlError from branch_create propagates as CtlError (not wrapped)."""
    avfs = _FakeAgentVFS()

    def raising_branch_create(name, step=-1):
        raise CtlError("daemon unreachable")

    avfs.branch_create = raising_branch_create
    factory = CandidateFactory(
        avfs=avfs, mount=Path("/mnt"), results_dir=Path("/tmp/res"),
        cgroup_factory=_make_cgroup_factory(), worker_argv=[], worker_env={})
    with pytest.raises(CtlError):
        factory.create(step=1, index=0, predicted_move="e2e4")


def test_candidate_factory_create_raises_cleanup_error_when_partial_leftover():
    """If both cgroup construction AND branch_delete fail, a CleanupError is
    raised chained from the original CandidateWorkError."""
    avfs = _FakeAgentVFS()

    def failing_branch_delete(name, step=-1):
        raise CtlError("cannot delete branch")

    avfs.branch_delete = failing_branch_delete

    def failing_factory(name):
        raise CgroupUnavailable("no cgroup")

    factory = CandidateFactory(
        avfs=avfs, mount=Path("/mnt"), results_dir=Path("/tmp/res"),
        cgroup_factory=failing_factory, worker_argv=[], worker_env={})
    with pytest.raises(CleanupError):
        factory.create(step=0, index=0, predicted_move="e2e4")


# --- worker CLI: secret hygiene and result protocol -------------------------


def test_main_emits_one_candidate_result_line(capsys, monkeypatch, tmp_path):
    fixed = CandidateResult("e2e4", "e7e5", "fen", 0.2, 7, 2)
    monkeypatch.setattr("src.candidate_worker.run_candidate",
                        lambda root, pm, actor: fixed)
    monkeypatch.setattr("openai.OpenAI", lambda **kw: None)
    GameState.initial().save(tmp_path / "game.json")
    rc = main(["--root", str(tmp_path), "--predicted-move", "e2e4",
               "--model", "actor-model", "--base-url-env", "BASE_URL",
               "--api-key-env", "API_KEY", "--timeout-s", "5", "--retries", "1"])
    assert rc == 0
    out = capsys.readouterr().out
    lines = [ln for ln in out.splitlines() if ln.strip()]
    assert len(lines) == 1
    decoded = json.loads(lines[0])
    assert decoded["prepared_move"] == "e7e5"
    assert decoded["tokens_in"] == 7


def test_main_writes_only_raw_excerpt_on_llm_error(capsys, monkeypatch, tmp_path):
    """On malformed opposing-Actor output, only raw_excerpt reaches stderr;
    never the exception object, endpoint, env value, or headers."""
    secret = "sk-SECRET-NEVER-LEAK"
    header_sentinel = "Bearer " + secret

    def fake_run(root, pm, actor):
        raise LLMOutputError(
            f"transport with {header_sentinel}", role="actor",
            raw_excerpt="MALFORMED MODEL OUTPUT")

    monkeypatch.setattr("src.candidate_worker.run_candidate", fake_run)
    monkeypatch.setattr("openai.OpenAI", lambda **kw: None)
    monkeypatch.setenv("BASE_URL", "https://endpoint.example")
    monkeypatch.setenv("API_KEY", secret)
    GameState.initial().save(tmp_path / "game.json")
    rc = main(["--root", str(tmp_path), "--predicted-move", "e2e4",
               "--model", "m", "--base-url-env", "BASE_URL",
               "--api-key-env", "API_KEY"])
    assert rc == 1
    captured = capsys.readouterr()
    # stdout is empty; stderr carries ONLY the bounded raw_excerpt.
    assert captured.out == ""
    assert captured.err == "MALFORMED MODEL OUTPUT"
    assert secret not in captured.err
    assert "endpoint.example" not in captured.err
    assert "Bearer" not in captured.err
