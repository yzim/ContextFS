"""Offline tests for the sequential benchmark runner and metrics writers.

All tests are offline: ``ScriptedActor`` replays canned moves and a local
``FakeAgentVFS`` records checkpoint calls as objects with a ``.label``. No real
``agentvfs-ctl``, daemon, FUSE mount, cgroup, root, network, or model is
required.
"""

import json
from dataclasses import dataclass
from types import SimpleNamespace

import pytest

from src.game import GameState
from src.llm import MoveResult
from src.metrics import (RunConfig, RunOutcome, StepRecord, compute_totals,
                         write_results)
from src.runner_regular import run_regular
from src.verifier import Verifier
from tests.fakes import ScriptedActor

MOVES = ["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6"]


@dataclass
class _CheckpointCall:
    """One recorded checkpoint invocation. Mirrors the real
    ``AgentVFS.checkpoint(label, step=, branch=)`` argument shape so the test
    can assert on ``.label`` exactly the way the controller pinned it."""
    label: str
    step: int


class FakeAgentVFS:
    """Minimal offline stand-in for :class:`src.agentvfs.AgentVFS`.

    ``checkpoint`` mirrors the real signature
    ``(label, step=-1, branch=None)`` and appends a :class:`_CheckpointCall`.
    ``verbs`` stays an empty list because no real ctl is invoked, so the metrics
    layer's verb handling is exercised only by ``test_agentvfs_unit``.
    """

    def __init__(self):
        self.checkpoints: list[_CheckpointCall] = []
        self.verbs: list = []

    def checkpoint(self, label: str, step: int = -1,
                   branch: str | None = None) -> str:
        self.checkpoints.append(_CheckpointCall(label, step))
        return "ok"


def test_regular_commits_scripted_game_and_checkpoints(tmp_path):
    root = tmp_path / "work"
    root.mkdir()
    actor = ScriptedActor([MoveResult(move, 0.01) for move in MOVES])
    avfs = FakeAgentVFS()
    cfg = RunConfig(max_plies=6, results_dir=tmp_path / "results",
                    k=3, k_launch=2, candidate_timeout_s=5)
    outcome = run_regular(root, actor, cfg, avfs=avfs)
    assert outcome.committed_moves == tuple(MOVES)
    assert outcome.state.moves == tuple(MOVES)
    assert [call.label for call in avfs.checkpoints] == [
        "chess-initial", "chess-ply-0", "chess-ply-1", "chess-ply-2",
        "chess-ply-3", "chess-ply-4", "chess-ply-5"]


def test_regular_resumes_existing_trajectory_to_total_max_plies(tmp_path):
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().apply("e2e4").save(root / "game.json")
    cfg = RunConfig(max_plies=2, results_dir=tmp_path / "results",
                    k=1, k_launch=1, candidate_timeout_s=5)

    outcome = run_regular(root, ScriptedActor.moves(["e7e5"]), cfg)

    assert outcome.committed_moves == ("e2e4", "e7e5")
    assert outcome.state.moves == outcome.committed_moves
    assert [step.ply for step in outcome.steps] == [1]


def test_write_results_has_stable_empty_and_nonempty_schemas(tmp_path):
    write_results(tmp_path, [], [], compute_totals([], [], 0.0, {"mode": "regular"}),
                  GameState.initial())
    assert (tmp_path / "perstep.csv").read_text().startswith("ply,source,")
    assert json.loads((tmp_path / "lossless.json").read_text())["ok"] is True


def test_run_regular_no_avfs_writes_full_results(tmp_path):
    """Without an AgentVFS the runner commits nothing but still writes a full
    results directory: per-step rows, committed moves, a lossless replay that
    matches the final state, and the stable CSV/MD headers."""
    root = tmp_path / "work"
    root.mkdir()
    actor = ScriptedActor([MoveResult(move, 0.01) for move in MOVES])
    cfg = RunConfig(max_plies=6, results_dir=tmp_path / "results",
                    k=3, k_launch=2, candidate_timeout_s=5)
    outcome = run_regular(root, actor, cfg)
    assert outcome.state.moves == tuple(MOVES)
    assert isinstance(outcome, RunOutcome)
    assert len(outcome.steps) == 6
    assert all(step.source == "actor" for step in outcome.steps)
    assert outcome.steps[0].ply == 0
    assert outcome.steps[0].committed_move == "e2e4"
    assert outcome.steps[0].spec_latency_s == 0.0
    assert outcome.steps[0].predicted_hit is False
    assert outcome.steps[0].hit_idx is None
    results = tmp_path / "results"
    perstep = (results / "perstep.csv").read_text()
    assert perstep.startswith("ply,source,")
    # one header line + six ply rows
    assert len(perstep.strip().splitlines()) == 7
    committed = json.loads((results / "committed.json").read_text())
    assert committed["moves"] == MOVES
    assert committed["initial_fen"] == outcome.state.initial_fen
    lossless = json.loads((results / "lossless.json").read_text())
    assert lossless["ok"] is True
    assert lossless["moves_match"] is True
    assert lossless["final_fen"] == outcome.state.fen
    assert lossless["replay_fen"] == outcome.state.fen
    assert (results / "verbs.csv").read_text().startswith("verb,step,")
    assert (results / "totals.csv").read_text().startswith("key,value")
    summary = (results / "summary.md").read_text()
    assert "mode" in summary and "regular" in summary


def test_step_record_carries_all_regular_columns():
    """A regular-mode StepRecord sets every speculative column to its zero
    value so the CSV schema is identical across modes."""
    rec = StepRecord(
        ply=0, source="actor", actor_latency_s=0.01, spec_latency_s=0.0,
        predictions="", predicted_hit=False, prelaunched_hit=False,
        hit_idx=None, window_missed=False, candidate_failures=0,
        head_start_s=0.0, latency_saved_s=0.0, missed_window_wait_s=0.0,
        actor_tokens_in=5, actor_tokens_out=1, spec_tokens_in=0,
        spec_tokens_out=0, committed_move="e2e4")
    assert rec.committed_move == "e2e4"
    assert rec.spec_tokens_in == 0 and rec.spec_tokens_out == 0


def test_compute_totals_includes_cost_accuracy_and_failure_contract():
    rec = StepRecord(
        ply=0, source="spec", actor_latency_s=0.1, spec_latency_s=0.02,
        predictions="e2e4", predicted_hit=True, prelaunched_hit=True,
        hit_idx=0, window_missed=False, candidate_failures=1,
        head_start_s=0.08, latency_saved_s=0.05,
        missed_window_wait_s=0.0, actor_tokens_in=10, actor_tokens_out=2,
        spec_tokens_in=6, spec_tokens_out=1, committed_move="e2e4",
        candidate_tokens_in=8, candidate_tokens_out=2,
        candidate_token_usage_unknown=1)
    verbs = [SimpleNamespace(status="ok"), SimpleNamespace(status="error")]

    totals = compute_totals([rec], verbs, 1.0, {"mode": "spec"},
                            verification_failures=2)

    assert totals["prediction_accuracy"] == 1.0
    assert totals["useful_hits"] == 1
    assert totals["actor_tokens_in"] == 10
    assert totals["spec_tokens_in"] == 6
    assert totals["candidate_tokens_in"] == 8
    assert totals["tokens_in_total"] == 24
    assert totals["candidate_token_usage_unknown"] == 1
    assert totals["verb_failures"] == 1
    assert totals["verification_failures"] == 2


def test_regular_with_real_verifier_read_back_matches(tmp_path):
    """Positive verify-path coverage: a real :class:`Verifier` reading main
    through the run's ``root`` mount sees the exact expected post-move state
    each ply, so the run completes and committed moves are correct. Regular
    mode has no branch routing, so the mount IS the run's working directory.
    Uses the real ``Verifier`` (not a fake) to exercise the real
    ``snapshot_main`` read+parse path."""
    root = tmp_path / "work"
    root.mkdir()
    actor = ScriptedActor.moves(MOVES, delay_s=0.01)
    cfg = RunConfig(max_plies=6, results_dir=tmp_path / "results",
                    k=3, k_launch=2, candidate_timeout_s=5)
    # avfs is unused on the snapshot_main path; None is safe offline.
    verifier = Verifier(avfs=None, mount=root,
                        results_dir=tmp_path / "vresults")
    outcome = run_regular(root, actor, cfg, verifier=verifier)
    assert outcome.committed_moves == tuple(MOVES)
    assert outcome.state.moves == tuple(MOVES)
    assert verifier.failures == 0


class _CorruptingVerifier(Verifier):
    """Reads main back through the REAL ``snapshot_main`` (via super()), but
    first overwrites ``game.json`` on disk with a divergent state. This proves
    the per-ply compare catches an on-disk corruption through the real read
    path -- not a mocked comparison, but the actual read-back-vs-expected
    guard firing on a real divergence."""

    def snapshot_main(self):
        corrupt = GameState.initial()  # moves=(), ply=0 -- differs from any post-move state
        corrupt.save(self.mount / "game.json")
        return super().snapshot_main()


def test_regular_verifier_catches_divergent_read_back(tmp_path):
    """Negative verify-path coverage: when the read-back state diverges from the
    expected in-memory post-move state, ``run_regular`` aborts with
    ``RuntimeError`` (fail closed). This is the test that proves the per-ply
    verify is no longer tautological -- it compares against an independent
    expected reference and catches a wrong/corrupted on-disk state."""
    root = tmp_path / "work"
    root.mkdir()
    actor = ScriptedActor.moves(MOVES, delay_s=0.01)
    cfg = RunConfig(max_plies=6, results_dir=tmp_path / "results",
                    k=3, k_launch=2, candidate_timeout_s=5)
    verifier = _CorruptingVerifier(avfs=None, mount=root,
                                   results_dir=tmp_path / "vresults")
    with pytest.raises(RuntimeError, match="diverged"):
        run_regular(root, actor, cfg, verifier=verifier)
    assert verifier.failures == 0  # abort came from the runner, not Verifier._fail
