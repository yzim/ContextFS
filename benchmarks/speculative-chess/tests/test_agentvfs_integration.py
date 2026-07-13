"""Deterministic six-ply AgentVFS integration scenario (Task 7, Step 4).

This test is marked ``@pytest.mark.integration``: it drives the REAL agentvfs
daemon, a REAL FUSE mount, REAL cgroup v2 routing, and REAL candidate-worker
subprocesses. It is skipped cleanly by the ``daemon`` fixture's prerequisite
gate whenever a platform prerequisite is missing -- in this non-root dev
environment that gate skips on cgroup delegation. On a delegation-capable host
(an operator step requiring sudo and ``HOME="$HOME"``) it runs end to end.

Determinism is achieved by scripting the worker replies from
``CHESS_SCRIPTED_RESPONSES`` (no network, no model key) and by giving the Actor
and Speculator per-ply sleeps so the bounded two-future race resolves to the
window the brief's table specifies:

    ply 0  speculator wins  predictions d2d4,e2e4   -> hit index 1 (e2e4)
    ply 1  prepared         consume e7e5            -> no window
    ply 2  speculator wins  predictions d2d3,c2c4   -> miss (g1f3)
    ply 3  actor first      prediction  a7a6        -> window missed, no launch
    ply 4  speculator wins  predictions f1c4,f1b5   -> f1c4 FAILs, hit index 1
    ply 5  prepared         consume a7a6            -> no window

The postcondition asserts the committed moves replay to the final FEN, no
``chess-spec-*`` branch survives, no candidate session or cgroup remains,
verification recorded zero failures, and the fixture teardown proves unmount.
"""

import json
import sys
import time
from pathlib import Path

import chess
import pytest

from src.agentvfs import AgentVFS
from src.candidate_worker import CandidateFactory
from src.cgroup import CgroupSession
from src.game import replay
from src.llm import MoveResult, PredictionResult
from src.metrics import RunConfig
from src.runner_spec import run_spec
from src.verifier import Verifier

# Resolve the ctl binary the same way tests/conftest.py does (the daemon fixture
# yields a handle without it): mirror config.yml's ../../build/ convention.
_PROJECT = Path(__file__).resolve().parents[1]   # benchmarks/speculative-chess
_CTL_BIN = (_PROJECT / "../../build/agentvfs-ctl").resolve()


class _WindowedActor:
    """Replays Actor moves with a per-call sleep so the speculator wins or loses
    the bounded race deterministically on each ply."""

    def __init__(self, moves, delays):
        self._moves = list(moves)
        self._delays = list(delays)
        self._i = 0

    def choose_move(self, position):
        assert self._i < len(self._moves), "WindowedActor script exhausted"
        move = self._moves[self._i]
        delay = self._delays[self._i]
        self._i += 1
        if delay:
            time.sleep(delay)
        return MoveResult(move=move, latency_s=delay)


class _WindowedSpeculator:
    """Replays prediction sets with a per-call sleep."""

    def __init__(self, predictions, delays):
        self._preds = [tuple(p) for p in predictions]
        self._delays = list(delays)
        self._i = 0

    def predict_moves(self, position, k):
        assert self._i < len(self._preds), "WindowedSpeculator script exhausted"
        preds = self._preds[self._i]
        delay = self._delays[self._i]
        self._i += 1
        if delay:
            time.sleep(delay)
        return PredictionResult(moves=preds, latency_s=delay)


@pytest.mark.integration
def test_six_ply_speculative_scenario(daemon, tmp_path):
    committed = ["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6"]

    # predicted_move -> prepared_move (the opposing Actor's reply), or "FAIL" to
    # make that candidate's worker exit nonzero. a7a6 is a PREDICTION at ply 3
    # (window missed, no candidate launched), so it is intentionally absent here.
    mapping = {
        "d2d4": "d7d5",
        "e2e4": "e7e5",
        "d2d3": "b8c6",
        "c2c4": "g8f6",
        "f1c4": "FAIL",
        "f1b5": "a7a6",
    }

    # Actor is consulted at plies 0, 2, 3, 4 (plies 1 and 5 consume prepared
    # replies). Slow at 0/2/4 so the speculator wins; fast at 3 so the actor
    # wins and the window is missed.
    actor = _WindowedActor(
        moves=["e2e4", "g1f3", "b8c6", "f1b5"],
        delays=[0.05, 0.05, 0.001, 0.05])
    speculator = _WindowedSpeculator(
        predictions=[("d2d4", "e2e4"), ("d2d3", "c2c4"), ("a7a6",),
                     ("f1c4", "f1b5")],
        delays=[0.001, 0.001, 0.05, 0.001])

    results_dir = tmp_path / "results"
    cfg = RunConfig(max_plies=6, results_dir=results_dir, k=3, k_launch=2,
                    candidate_timeout_s=30)

    avfs = AgentVFS(sock=daemon.sock, store_objects=daemon.store,
                    ctl_bin=_CTL_BIN)
    verifier = Verifier(avfs, daemon.mount, results_dir)

    worker_argv = [sys.executable, "-m", "tests.scripted_worker"]
    worker_env = {
        "PYTHONPATH": str(_PROJECT),
        "CHESS_SCRIPTED_RESPONSES": json.dumps(mapping),
    }
    candidate_factory = CandidateFactory(
        avfs=avfs, mount=daemon.mount, results_dir=results_dir,
        cgroup_factory=lambda name: CgroupSession(name),
        worker_argv=worker_argv, worker_env=worker_env)

    outcome = run_spec(daemon.mount, actor, speculator, cfg, avfs, verifier,
                       candidate_factory)

    # --- lossless: the committed moves replay to the final FEN ---------------
    assert outcome.committed_moves == tuple(committed)
    expected = replay(chess.STARTING_FEN, committed)
    assert outcome.state.fen == expected.fen

    # --- leak-free: no speculative branch, session, or cgroup survives -------
    branches = {b["name"] for b in avfs.branch_list()}
    assert not any(name.startswith("chess-spec-") for name in branches)
    assert avfs.active_sessions == set()
    # check_cleanup (folded into verify_failures) proves every candidate cgroup
    # path is absent; assert the verifier agrees.
    assert verifier.failures == 0

    # --- the deterministic windows played out as scripted --------------------
    by_ply = {step.ply: step for step in outcome.steps}
    # ply 0: speculator wins, e2e4 hits launched candidate index 1.
    assert by_ply[0].source == "spec"
    assert by_ply[0].prelaunched_hit is True
    assert by_ply[0].hit_idx == 1
    # ply 1: prepared reply e7e5 consumed, no window.
    assert by_ply[1].source == "prepared"
    assert by_ply[1].committed_move == "e7e5"
    # ply 2: speculator wins but g1f3 is not among the predictions -> miss.
    assert by_ply[2].prelaunched_hit is False
    assert by_ply[2].committed_move == "g1f3"
    # ply 3: actor first -> window missed, no candidate launched.
    assert by_ply[3].window_missed is True
    assert by_ply[3].committed_move == "b8c6"
    # ply 4: speculator wins; the f1c4 candidate worker exits nonzero (FAIL) but
    # that is a collect-time event for a non-match sibling -- it is cleaned
    # without being collected, so it is NOT counted in candidate_failures (which
    # counts only creation-loop failures). The healthy f1b5 match still hits at
    # launched index 1, proving a failing sibling cannot discard a healthy hit.
    assert by_ply[4].source == "spec"
    assert by_ply[4].prelaunched_hit is True
    assert by_ply[4].hit_idx == 1
    assert by_ply[4].candidate_failures == 0
    # ply 5: prepared reply a7a6 consumed.
    assert by_ply[5].source == "prepared"
    assert by_ply[5].committed_move == "a7a6"

    # The daemon fixture's ``finally`` proves unmount: if the mount remained
    # after stop_daemon it would pytest.fail, so reaching this line means the
    # teardown contract held.
