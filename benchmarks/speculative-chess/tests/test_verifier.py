"""Offline unit tests for the routed-cgroup Verifier.

A :class:`_RoutedCgroup` fake serves canned file contents via ``run_text`` so
the verifier observes "the routed branch" without a real cgroup or daemon. A
:class:`_FakeAgentVFS` provides ``branch_list`` / ``status`` and never reaches
``agentvfs-ctl``. No real mount, FUSE, cgroup, network, or root is required.
"""

import json
from pathlib import Path
from types import SimpleNamespace

import pytest

from src.candidate_worker import CandidateResult, PreparedResponse
from src.game import GameState, StateError
from src.verifier import VerificationError, Verifier


class _RoutedCgroup:
    """A fake cgroup that returns canned file contents for ``cat`` via
    ``run_text``; models what the routed branch makes visible."""

    def __init__(self, files=None, path="/tmp/absent-cg"):
        self.path = path
        self.files = dict(files or {})
        self.children = []

    def run_text(self, argv, cwd, timeout=60, input_text=None):
        if argv and argv[0] == "cat":
            # cat concatenates the named files in order; each stored as a
            # single compact JSON line (mirroring the real writers).
            return "".join(self.files.get(name, "") for name in argv[1:])
        return ""


class _FakeAgentVFS:
    def __init__(self, branches=None, status_ok=True):
        self._branches = list(branches or [])
        self._status_ok = status_ok

    def branch_list(self):
        return [{"name": n} for n in self._branches]

    def status(self):
        if not self._status_ok:
            raise RuntimeError("daemon unreachable")
        return {"ok": True}


def _write_main(tmp_path, state):
    """Place state as main's game.json under the mount and return its bytes."""
    path = tmp_path / "game.json"
    state.save(path)
    return path.read_bytes()


def _prepared_json(prepared):
    return json.dumps(prepared.to_dict())


def _attempt(cgroup, *, predicted_move="e2e4", branch="chess-spec-0-0"):
    return SimpleNamespace(cgroup=cgroup, branch=branch,
                           predicted_move=predicted_move, process_live=False)


# --- snapshot_main + check_main_unchanged -----------------------------------


def test_snapshot_main_records_bytes_and_state(tmp_path):
    state = GameState.initial()
    main_bytes = _write_main(tmp_path, state)
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    recorded = verifier.snapshot_main()
    assert recorded == state
    assert verifier._main_bytes == main_bytes


def test_check_main_unchanged_passes_when_identical(tmp_path):
    _write_main(tmp_path, GameState.initial())
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    verifier.check_main_unchanged()  # must not raise
    assert verifier.failures == 0


def test_check_main_unchanged_fails_when_mutated(tmp_path):
    _write_main(tmp_path, GameState.initial())
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    # Simulate a stray main mutation during the speculation window.
    GameState.initial().apply("e2e4").save(tmp_path / "game.json")
    with pytest.raises(VerificationError, match="main-mutated"):
        verifier.check_main_unchanged()
    assert verifier.failures == 1
    report = (tmp_path / "ver" / "verify-fail-main-mutated.txt").read_text()
    assert report.startswith("POSTCONDITION FAILED: main-mutated")


def test_check_main_unchanged_fails_without_snapshot(tmp_path):
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    with pytest.raises(VerificationError, match="snapshot"):
        verifier.check_main_unchanged()


# --- check_candidate_base: byte-equality before launch ----------------------


def test_check_candidate_base_passes_on_byte_equality(tmp_path):
    main_bytes = _write_main(tmp_path, GameState.initial())
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    cg = _RoutedCgroup(files={"game.json": main_bytes.decode()})
    verifier.check_candidate_base(_attempt(cg))
    assert verifier.failures == 0


def test_check_candidate_base_fails_on_byte_mismatch(tmp_path):
    _write_main(tmp_path, GameState.initial())
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    # Candidate branch sees a stale/different game.json.
    other = GameState.initial().apply("d2d4")
    cg = _RoutedCgroup(files={"game.json": json.dumps(other.to_dict()) + "\n"})
    with pytest.raises(VerificationError, match="candidate-base"):
        verifier.check_candidate_base(_attempt(cg))
    assert (tmp_path / "ver" / "verify-fail-candidate-base.txt").exists()


# --- check_candidate: routed game.json + prepared_response.json -------------


def test_check_candidate_passes_on_consistent_branch_state(tmp_path):
    main_state = GameState.initial()
    _write_main(tmp_path, main_state)
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    predicted = "e2e4"
    cand_state = main_state.apply(predicted)
    prepared = PreparedResponse(1, predicted, "e7e5", cand_state.fen)
    cg = _RoutedCgroup(files={
        "game.json": json.dumps(cand_state.to_dict()) + "\n",
        "prepared_response.json": _prepared_json(prepared) + "\n"})
    result = CandidateResult(predicted, "e7e5", cand_state.fen, 0.2, 7, 2)
    verifier.check_candidate(_attempt(cg), result)
    assert verifier.failures == 0


def test_check_candidate_fails_when_branch_state_not_predicted(tmp_path):
    main_state = GameState.initial()
    _write_main(tmp_path, main_state)
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    # The candidate wrote a state that is NOT main + e2e4.
    wrong = main_state.apply("d2d4")
    prepared = PreparedResponse(1, "e2e4", "d7d5", wrong.fen)
    cg = _RoutedCgroup(files={
        "game.json": json.dumps(wrong.to_dict()) + "\n",
        "prepared_response.json": _prepared_json(prepared) + "\n"})
    result = CandidateResult("e2e4", "d7d5", wrong.fen, 0.1, 1, 1)
    with pytest.raises(VerificationError, match="candidate-state"):
        verifier.check_candidate(_attempt(cg), result)


def test_check_candidate_fails_when_prepared_response_illegal(tmp_path):
    main_state = GameState.initial()
    _write_main(tmp_path, main_state)
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    cand_state = main_state.apply("e2e4")  # black to move
    # d2d4 is a white-pawn move: illegal for black.
    prepared = PreparedResponse(1, "e2e4", "d2d4", cand_state.fen)
    cg = _RoutedCgroup(files={
        "game.json": json.dumps(cand_state.to_dict()) + "\n",
        "prepared_response.json": _prepared_json(prepared) + "\n"})
    result = CandidateResult("e2e4", "d2d4", cand_state.fen, 0.1, 1, 1)
    with pytest.raises(VerificationError, match="candidate-prepared"):
        verifier.check_candidate(_attempt(cg), result)


# --- check_hit_merge --------------------------------------------------------


def test_check_hit_merge_passes_when_main_matches_branch(tmp_path):
    main_state = GameState.initial()
    _write_main(tmp_path, main_state)
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    merged = main_state.apply("e2e4")
    merged_bytes = json.dumps(merged.to_dict()) + "\n"
    # After the merge, main == the merged branch content.
    (tmp_path / "game.json").write_text(merged_bytes)
    cg = _RoutedCgroup(files={"game.json": merged_bytes})
    verifier.check_hit_merge(_attempt(cg))
    assert verifier.failures == 0


def test_check_hit_merge_fails_on_mismatch(tmp_path):
    _write_main(tmp_path, GameState.initial())
    verifier = Verifier(_FakeAgentVFS(), tmp_path, tmp_path / "ver")
    verifier.snapshot_main()
    other = json.dumps(GameState.initial().apply("d2d4").to_dict()) + "\n"
    cg = _RoutedCgroup(files={"game.json": other})
    with pytest.raises(VerificationError, match="hit-merge"):
        verifier.check_hit_merge(_attempt(cg))


# --- check_cleanup ----------------------------------------------------------


def test_check_cleanup_passes_when_all_gone(tmp_path):
    verifier = Verifier(_FakeAgentVFS(branches=["main"]), tmp_path,
                        tmp_path / "ver")
    gone = _attempt(_RoutedCgroup(path="/tmp/absent-cg"))
    verifier.check_cleanup([gone], branch_names=["chess-spec-0-0"])
    assert verifier.failures == 0


def test_check_cleanup_fails_on_leftover_branch(tmp_path):
    verifier = Verifier(
        _FakeAgentVFS(branches=["main", "chess-spec-0-0"]), tmp_path,
        tmp_path / "ver")
    gone = _attempt(_RoutedCgroup(path="/tmp/absent-cg"))
    with pytest.raises(VerificationError, match="cleanup-branches"):
        verifier.check_cleanup([gone], branch_names=["chess-spec-0-0"])


def test_check_cleanup_fails_on_live_process(tmp_path):
    verifier = Verifier(_FakeAgentVFS(branches=["main"]), tmp_path,
                        tmp_path / "ver")
    live = _attempt(_RoutedCgroup(path="/tmp/absent-cg"))
    live.process_live = True
    with pytest.raises(VerificationError, match="cleanup-process"):
        verifier.check_cleanup([live], branch_names=[])


def test_check_cleanup_fails_on_remaining_cgroup_path(tmp_path, monkeypatch):
    verifier = Verifier(_FakeAgentVFS(branches=["main"]), tmp_path,
                        tmp_path / "ver")
    remnant = _attempt(_RoutedCgroup(path="/tmp/leftover-cg"))
    monkeypatch.setattr("pathlib.Path.exists", lambda self: str(self) == "/tmp/leftover-cg")
    with pytest.raises(VerificationError, match="cleanup-cgroup"):
        verifier.check_cleanup([remnant], branch_names=[])


# --- _fail diagnostics: file header + daemon reachability -------------------


def test_fail_appends_status_and_branch_list_when_daemon_reachable(tmp_path):
    avfs = _FakeAgentVFS(branches=["main", "chess-spec-0-0"], status_ok=True)
    verifier = Verifier(avfs, tmp_path, tmp_path / "ver")
    with pytest.raises(VerificationError):
        verifier._fail("demo", ["a line"])
    body = (tmp_path / "ver" / "verify-fail-demo.txt").read_text()
    assert body.startswith("POSTCONDITION FAILED: demo")
    assert "a line" in body
    assert "status:" in body
    assert "branches:" in body
    assert "chess-spec-0-0" in body


def test_fail_omits_diagnostics_when_daemon_unreachable(tmp_path):
    avfs = _FakeAgentVFS(status_ok=False)
    verifier = Verifier(avfs, tmp_path, tmp_path / "ver")
    with pytest.raises(VerificationError):
        verifier._fail("demo", ["a line"])
    body = (tmp_path / "ver" / "verify-fail-demo.txt").read_text()
    assert body.startswith("POSTCONDITION FAILED: demo")
    # No status/branches lines leak in when the daemon is unreachable.
    assert "status:" not in body
    assert "branches:" not in body
