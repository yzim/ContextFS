"""Offline tests for the speculative runner (Task 6).

Every test is fully offline: ``ScriptedActor`` / ``ScriptedSpeculator`` replay
canned model output (with real ``time.sleep`` so the two-future race is
deterministic), and local ``FakeCandidateFactory`` / ``FakeAgentVFS`` /
``FakeVerifier`` objects stand in for the daemon, FUSE mount, cgroup, and
routed-postcondition layer. No real ``agentvfs-ctl``, daemon, FUSE mount,
cgroup, root, network, or model key is required.

The candidate fake maps ``predicted_move -> prepared_move`` (or a
``CandidateWorkError`` to fail that candidate), materializes a branch-local
``GameState`` + ``PreparedResponse`` on ``collect`` so a hit's
``branch_merge`` can stage them on main, and tracks cleanup status so
``all_resources_gone`` can prove the outer exception handler released every
live attempt. The verifier fake records which postcondition methods ran and in
what order.
"""

import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from src.agentvfs import CtlError
from src.candidate_worker import (CandidateResult, CandidateWorkError,
                                  CleanupError, PreparedResponse)
from src.game import GameState, StateError
from src.llm import LLMOutputError
from src.metrics import RunConfig
from src.runner_spec import consume_prepared, require_cleanup, run_spec
from src.verifier import VerificationError
from tests.fakes import ScriptedActor, ScriptedSpeculator


# --- local fakes ------------------------------------------------------------
# All three are LOCAL to this module (per the brief); they are not added to
# tests/fakes.py and no configurable harness class is built around them.


class FakeCandidateAttempt:
    """One in-memory candidate: branch-local state on collect, tracked cleanup.

    ``outcome`` is either the prepared-move string the opposing actor would
    reply with, or a :class:`CandidateWorkError` to fail this candidate's
    collect. On ``collect`` the attempt reconstructs the branch-local
    ``GameState`` (main + predicted_move) and a validating
    :class:`PreparedResponse` and registers them on the factory keyed by
    branch, so a later ``FakeAgentVFS.branch_merge`` can stage them on main.
    """

    def __init__(self, branch, predicted_move, outcome, factory, latency_s):
        self.branch = branch
        self.predicted_move = predicted_move
        self._outcome = outcome
        self._factory = factory
        self.latency_s = latency_s
        self.launched = False
        self.collected = False
        self.cleaned = False
        self._cleanup_calls = 0
        self._cleanup_fail_first = 0
        # Attributes the real CandidateAttempt exposes; unused by FakeVerifier
        # but present so the real Verifier shape is mirrored.
        self.cgroup = None
        self.process_live = False

    def launch(self):
        # A candidate mapped to a CandidateWorkError fails at launch time, so a
        # test can script one candidate to fail while a healthy sibling still
        # launches and hits.
        if isinstance(self._outcome, CandidateWorkError):
            self._factory.events.append(("launch-fail", self.branch))
            raise self._outcome
        self.launched = True
        self._factory.launches.append(self.branch)
        self._factory.events.append(("launch", self.branch))

    def collect(self, timeout_s):
        self.collected = True
        self._factory.events.append(("collect", self.branch))
        if isinstance(self._outcome, CandidateWorkError):
            raise self._outcome
        main = GameState.load(self._factory.root / "game.json")
        cand_state = main.apply(self.predicted_move)
        prepared = PreparedResponse(schema=1, predicted_move=self.predicted_move,
                                    prepared_move=self._outcome,
                                    position_fen=cand_state.fen)
        self._factory.branch_states[self.branch] = (cand_state, prepared)
        return CandidateResult(predicted_move=self.predicted_move,
                               prepared_move=self._outcome,
                               position_fen=cand_state.fen,
                               latency_s=self.latency_s,
                               tokens_in=self._factory.tokens_in,
                               tokens_out=self._factory.tokens_out)

    def cleanup(self):
        self._cleanup_calls += 1
        if self._cleanup_calls <= self._cleanup_fail_first:
            self._factory.events.append(("cleanup-fail", self.branch))
            return False
        self.cleaned = True
        self._factory.events.append(("cleanup", self.branch))
        return True


class FakeCandidateFactory:
    """Maps predicted moves to prepared replies (or candidate failures).

    ``cleanup_fail_first`` scripts the first N ``cleanup()`` calls on every
    created attempt to return False, so the cleanup failure path can make a
    single ``require_cleanup`` (two passes) fail while the outer handler's
    retry then succeeds.
    """

    def __init__(self, mapping, latency_s=0.05, cleanup_fail_first=0,
                 tokens_in=0, tokens_out=0):
        self.root = None
        self.mapping = dict(mapping)
        self.latency_s = latency_s
        self.cleanup_fail_first = cleanup_fail_first
        self.tokens_in = tokens_in
        self.tokens_out = tokens_out
        self.launches = []
        self.events = []
        self.branch_states = {}
        self.created = []

    def create(self, *, step, index, predicted_move):
        if predicted_move not in self.mapping:
            raise CandidateWorkError(f"no candidate mapping for {predicted_move}")
        outcome = self.mapping[predicted_move]
        branch = f"chess-spec-{step}-{index}"
        attempt = FakeCandidateAttempt(branch, predicted_move, outcome, self,
                                       self.latency_s)
        attempt._cleanup_fail_first = self.cleanup_fail_first
        self.created.append(attempt)
        self.events.append(("create", branch))
        return attempt

    def all_resources_gone(self):
        return all(a.cleaned for a in self.created)


class FakeAgentVFS:
    """Records checkpoint/merge calls and materializes a hit onto main."""

    def __init__(self, root=None, factory=None, fail_verb=None):
        self.root = root
        self.factory = factory
        self.fail_verb = fail_verb
        self.checkpoints = []
        self.merges = []
        self.verbs = []
        self.active_sessions = set()

    def checkpoint(self, label, step=-1, branch=None):
        if self.fail_verb == "checkpoint":
            raise CtlError("checkpoint failed (scripted)")
        self.checkpoints.append((label, step))
        return "ok"

    def branch_merge(self, source, into="main", step=-1):
        if self.fail_verb == "merge":
            raise CtlError("branch merge failed (scripted)")
        self.merges.append(source)
        # A real merge brings the candidate branch's game.json +
        # prepared_response.json onto main; mirror that so consume_prepared
        # can read them next ply.
        if self.factory is not None and source in self.factory.branch_states:
            cand_state, prepared = self.factory.branch_states[source]
            cand_state.save(self.root / "game.json")
            prepared.save(self.root / "prepared_response.json")
        return "ok"

    def branch_list(self):
        return []

    def status(self):
        return {"ok": True}


class FakeVerifier:
    """Records postcondition method calls in order; optionally fails one."""

    def __init__(self, fail_check=None):
        self.fail_check = fail_check
        self.failures = 0
        self.calls = []

    def snapshot_main(self):
        self.calls.append("snapshot_main")
        return None

    def check_main_unchanged(self):
        self.calls.append("check_main_unchanged")
        if self.fail_check == "check_main_unchanged":
            self.failures += 1
            raise VerificationError("main mutated (scripted)")

    def check_candidate_base(self, attempt):
        self.calls.append("check_candidate_base")
        if self.fail_check == "check_candidate_base":
            self.failures += 1
            raise VerificationError("candidate base mismatch (scripted)")

    def check_candidate(self, attempt, result):
        self.calls.append("check_candidate")
        if self.fail_check == "check_candidate":
            self.failures += 1
            raise VerificationError("candidate result mismatch (scripted)")

    def check_hit_merge(self, attempt):
        self.calls.append("check_hit_merge")
        if self.fail_check == "check_hit_merge":
            self.failures += 1
            raise VerificationError("hit merge mismatch (scripted)")

    def check_cleanup(self, attempts, branch_names):
        self.calls.append("check_cleanup")


class _RaisingActor:
    """An actor whose ``choose_move`` always raises (e.g. malformed output)."""

    def __init__(self, error):
        self._error = error

    def choose_move(self, position):
        raise self._error


class _RaisingSpeculator:
    """A speculator whose ``predict_moves`` always raises."""

    def __init__(self, error):
        self._error = error

    def predict_moves(self, position, k):
        raise self._error


# --- harness helpers --------------------------------------------------------


def run_case(tmp_path, actor, speculator, candidates, max_plies=1, k=2,
             k_launch=2):
    """Build a fresh root + config + fakes around ``run_spec`` and run it.

    Seeds ``game.json`` at the start position, points the supplied candidate
    factory at the root, wires the fake AgentVFS/Verifier, and returns the
    :class:`RunOutcome`.
    """
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates.root = root
    cfg = RunConfig(max_plies=max_plies, results_dir=tmp_path / "results",
                    k=k, k_launch=k_launch, candidate_timeout_s=5)
    avfs = FakeAgentVFS(root=root, factory=candidates)
    verifier = FakeVerifier()
    return run_spec(root, actor, speculator, cfg, avfs, verifier, candidates)


def failing_case(tmp_path, failure):
    """Four explicit failing setups selected by key.

    ``cleanup`` scripts the candidate cleanup to fail the first two passes
    (one whole ``require_cleanup``) and succeed on the outer retry; the other
    three inject a verifier / control / state error mid-ply while at least one
    candidate is live, so the outer handler's cleanup retry is exercised.
    """
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results",
                    k=2, k_launch=2, candidate_timeout_s=5)
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.03)
    speculator = ScriptedSpeculator.predictions([["d2d4"]], delay_s=0.001)
    candidates = None  # set per-case below; root is assigned after construction

    if failure == "cleanup":
        candidates = FakeCandidateFactory({"d2d4": "d7d5"}, cleanup_fail_first=2)
        verifier = FakeVerifier()
        avfs = FakeAgentVFS(root=root, factory=candidates)
    elif failure == "verifier":
        candidates = FakeCandidateFactory({"d2d4": "d7d5"})
        verifier = FakeVerifier(fail_check="check_candidate_base")
        avfs = FakeAgentVFS(root=root, factory=candidates)
    elif failure == "control":
        candidates = FakeCandidateFactory({"e2e4": "e7e5"})
        verifier = FakeVerifier()
        avfs = FakeAgentVFS(root=root, factory=candidates, fail_verb="merge")
        speculator = ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001)
    elif failure == "state":
        candidates = FakeCandidateFactory({"d2d4": "d7d5"})
        verifier = FakeVerifier()
        avfs = FakeAgentVFS(root=root, factory=candidates)
        actor = ScriptedActor.moves(["e7e5"], delay_s=0.03)  # illegal at start
    else:  # pragma: no cover - guarded by parametrize
        raise AssertionError(f"unknown failure key {failure!r}")

    candidates.root = root  # the factory reads main's game.json on collect

    def run():
        return run_spec(root, actor, speculator, cfg, avfs, verifier, candidates)

    return SimpleNamespace(run=run, candidates=candidates)


# --- Step 1: hit / miss / prepared-response / replay ------------------------


def test_hit_merges_winner_then_consumes_prepared_response(tmp_path):
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.03)
    speculator = ScriptedSpeculator.predictions([["d2d4", "e2e4"]], delay_s=0.001)
    candidates = FakeCandidateFactory({"d2d4": "d7d5", "e2e4": "e7e5"})
    outcome = run_case(tmp_path, actor, speculator, candidates, max_plies=2)
    assert outcome.committed_moves == ("e2e4", "e7e5")
    assert outcome.steps[0].hit_idx == 1
    assert outcome.steps[0].prelaunched_hit
    assert outcome.steps[0].latency_saved_s > 0
    assert outcome.steps[1].source == "prepared"


def test_hit_checks_main_unchanged_before_merge(tmp_path):
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates = FakeCandidateFactory({"e2e4": "e7e5"})
    candidates.root = root
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=1,
                    k_launch=1, candidate_timeout_s=5)
    avfs = FakeAgentVFS(root=root, factory=candidates)
    verifier = FakeVerifier()

    run_spec(root, ScriptedActor.moves(["e2e4"], delay_s=0.03),
             ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001),
             cfg, avfs, verifier, candidates)

    assert verifier.calls.index("check_main_unchanged") < \
        verifier.calls.index("check_hit_merge")


def test_both_completed_uses_actual_actor_first_order(monkeypatch, tmp_path):
    def wait_for_both(futures, return_when):
        for future in futures:
            future.result()
        return set(futures), set()

    monkeypatch.setattr("src.runner_spec.wait", wait_for_both)
    candidates = FakeCandidateFactory({"e2e4": "e7e5"})
    outcome = run_case(
        tmp_path,
        ScriptedActor.moves(["e2e4"], delay_s=0.001),
        ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.02),
        candidates,
        max_plies=1,
    )

    assert outcome.steps[0].window_missed is True
    assert candidates.launches == []


def test_head_start_uses_model_completion_not_candidate_setup(tmp_path):
    class SlowFactory(FakeCandidateFactory):
        def create(self, **kwargs):
            time.sleep(0.05)
            return super().create(**kwargs)

    candidates = SlowFactory({"e2e4": "e7e5"})
    outcome = run_case(
        tmp_path,
        ScriptedActor.moves(["e2e4"], delay_s=0.01),
        ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001),
        candidates,
        max_plies=1,
    )

    assert outcome.steps[0].head_start_s < 0.03


def test_match_outside_k_launch_is_scored_but_applied_sequentially(tmp_path):
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.03)
    speculator = ScriptedSpeculator.predictions(
        [["d2d4", "g1f3", "e2e4"]], delay_s=0.001)
    outcome = run_case(tmp_path, actor, speculator,
                       FakeCandidateFactory({"d2d4": "d7d5"}),
                       max_plies=1, k=3, k_launch=1)
    assert outcome.steps[0].predicted_hit
    assert not outcome.steps[0].prelaunched_hit
    assert outcome.committed_moves == ("e2e4",)


@pytest.mark.parametrize("hit_at", [0, 1, 2])
def test_launched_hit_at_first_middle_last_index(tmp_path, hit_at):
    others = ["d2d4", "g1f3"]
    preds = others[:hit_at] + ["e2e4"] + others[hit_at:]
    mapping = {"d2d4": "d7d5", "g1f3": "b8c6", "e2e4": "e7e5"}
    outcome = run_case(
        tmp_path,
        ScriptedActor.moves(["e2e4"], delay_s=0.03),
        ScriptedSpeculator.predictions([preds], delay_s=0.001),
        FakeCandidateFactory(mapping), max_plies=1, k=3, k_launch=3)
    assert outcome.steps[0].hit_idx == hit_at
    assert outcome.steps[0].prelaunched_hit
    assert outcome.steps[0].latency_saved_s > 0


def test_miss_cleans_candidates_and_checks_cleanup_before_main_mutation(tmp_path):
    """On a miss, candidate cleanup and ``check_cleanup`` precede the commit
    of the actor move to main. The verifier records check_cleanup before
    check_main_unchanged, and only afterward does the runner save+checkpoint."""
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates = FakeCandidateFactory({"d2d4": "d7d5"})
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=2,
                    k_launch=2, candidate_timeout_s=5)
    avfs = FakeAgentVFS(root=root, factory=candidates)
    verifier = FakeVerifier()
    outcome = run_spec(root, ScriptedActor.moves(["e2e4"], delay_s=0.03),
                       ScriptedSpeculator.predictions([["d2d4"]], delay_s=0.001),
                       cfg, avfs, verifier, candidates)
    assert outcome.committed_moves == ("e2e4",)
    assert not outcome.steps[0].predicted_hit
    assert candidates.created
    assert all(a.cleaned for a in candidates.created)
    final_unchanged = max(
        idx for idx, call in enumerate(verifier.calls)
        if call == "check_main_unchanged")
    assert verifier.calls.index("check_cleanup") < final_unchanged


def test_four_ply_speculative_game_json_matches_regular(tmp_path):
    """A four-ply all-miss speculative run applies actor moves sequentially, so
    the final ``game.json`` is byte-identical to a regular run for the same
    committed moves (lossless)."""
    moves = ["e2e4", "e7e5", "g1f3", "b8c6"]
    spec_root = tmp_path / "spec"
    spec_root.mkdir()
    GameState.initial().save(spec_root / "game.json")
    candidates = FakeCandidateFactory({"d2d4": "d7d5"})
    spec_cfg = RunConfig(max_plies=4, results_dir=tmp_path / "spec-results",
                         k=2, k_launch=2, candidate_timeout_s=5)
    spec_outcome = run_spec(
        spec_root, ScriptedActor.moves(moves, delay_s=0.03),
        ScriptedSpeculator.predictions([["d2d4"]] * 4, delay_s=0.001),
        spec_cfg, FakeAgentVFS(root=spec_root, factory=candidates),
        FakeVerifier(), candidates)
    assert spec_outcome.committed_moves == tuple(moves)

    reg_root = tmp_path / "reg"
    reg_root.mkdir()
    GameState.initial().save(reg_root / "game.json")
    from src.runner_regular import run_regular
    reg_cfg = RunConfig(max_plies=4, results_dir=tmp_path / "reg-results",
                        k=2, k_launch=2, candidate_timeout_s=5)
    run_regular(reg_root, ScriptedActor.moves(moves), reg_cfg)
    assert (spec_root / "game.json").read_bytes() == \
        (reg_root / "game.json").read_bytes()


# --- Step 2: Actor-first / candidate-local failure / fail-closed ------------


def test_actor_first_joins_bounded_speculator_without_launching(tmp_path):
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.001)
    speculator = ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.03)
    candidates = FakeCandidateFactory({"e2e4": "e7e5"})
    outcome = run_case(tmp_path, actor, speculator, candidates, max_plies=1)
    assert outcome.steps[0].window_missed
    assert outcome.steps[0].missed_window_wait_s > 0
    assert candidates.launches == []


def test_failed_candidate_does_not_discard_healthy_matching_sibling(tmp_path):
    candidates = FakeCandidateFactory(
        {"d2d4": CandidateWorkError("failed"), "e2e4": "e7e5"})
    outcome = run_case(tmp_path, ScriptedActor.moves(["e2e4"], delay_s=0.03),
                       ScriptedSpeculator.predictions([["d2d4", "e2e4"]],
                                                      delay_s=0.001),
                       candidates, max_plies=1)
    assert outcome.steps[0].prelaunched_hit
    assert outcome.steps[0].candidate_failures == 1


def test_create_failed_candidate_does_not_abort_ply(tmp_path):
    """A CandidateWorkError raised from create() (NOT launch()) removes only
    that candidate and the run completes. Contrast
    test_failed_candidate_does_not_discard_healthy_matching_sibling, which
    scripts the failure from launch() (the mapping VALUE is the error, so
    create() succeeds). Here the absent mapping for "d2d4" makes create()
    itself raise before any attempt is constructed -- mirroring how the real
    CandidateFactory._abort already cleans partial state before the
    CandidateWorkError escapes, so the runner has NO attempt to clean and
    must simply drop the candidate and keep launching siblings."""
    # "d2d4" is ABSENT -> FakeCandidateFactory.create raises
    # CandidateWorkError (create-failure). "e2e4" -> healthy reply that
    # matches the Actor move.
    candidates = FakeCandidateFactory({"e2e4": "e7e5"})
    outcome = run_case(tmp_path, ScriptedActor.moves(["e2e4"], delay_s=0.03),
                       ScriptedSpeculator.predictions([["d2d4", "e2e4"]],
                                                      delay_s=0.001),
                       candidates, max_plies=1)
    assert outcome.steps[0].prelaunched_hit is True
    assert outcome.steps[0].candidate_failures == 1
    assert outcome.steps[0].hit_idx == 1
    assert outcome.committed_moves == ("e2e4",)
    # The create-failed candidate never constructed an attempt, so the only
    # created attempt is the healthy sibling, which was cleaned on the hit.
    assert len(candidates.created) == 1
    assert all(a.cleaned for a in candidates.created)


def test_winning_collect_failure_counts_candidate_failure_and_uses_miss(tmp_path):
    class CollectFailAttempt(FakeCandidateAttempt):
        def launch(self):
            self.launched = True
            self._factory.launches.append(self.branch)

        def collect(self, timeout_s):
            raise CandidateWorkError("worker timeout")

    class CollectFailFactory(FakeCandidateFactory):
        def create(self, *, step, index, predicted_move):
            attempt = CollectFailAttempt(
                f"chess-spec-{step}-{index}", predicted_move, "e7e5", self,
                self.latency_s)
            self.created.append(attempt)
            return attempt

    candidates = CollectFailFactory({"e2e4": "e7e5"})
    outcome = run_case(
        tmp_path,
        ScriptedActor.moves(["e2e4"], delay_s=0.03),
        ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001),
        candidates,
        max_plies=1,
    )

    assert outcome.steps[0].source == "actor"
    assert outcome.steps[0].candidate_failures == 1


@pytest.mark.parametrize("failure", ["cleanup", "verifier", "control", "state"])
def test_correctness_failures_abort_after_retrying_cleanup(tmp_path, failure):
    case = failing_case(tmp_path, failure)
    with pytest.raises((CleanupError, VerificationError, CtlError, StateError)):
        case.run()
    assert case.candidates.all_resources_gone()


def test_cleanup_failure_is_chained_without_masking_verifier_error(tmp_path):
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates = FakeCandidateFactory(
        {"e2e4": "e7e5"}, cleanup_fail_first=100)
    candidates.root = root
    verifier = FakeVerifier(fail_check="check_candidate_base")
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=1,
                    k_launch=1, candidate_timeout_s=5)

    with pytest.raises(VerificationError) as exc_info:
        run_spec(root, ScriptedActor.moves(["e2e4"], delay_s=0.03),
                 ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001),
                 cfg, FakeAgentVFS(root=root, factory=candidates), verifier,
                 candidates)

    assert isinstance(exc_info.value.__cause__, CleanupError)


def test_spec_resumes_existing_trajectory_to_total_max_plies(tmp_path):
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().apply("e2e4").save(root / "game.json")
    candidates = FakeCandidateFactory({})
    candidates.root = root
    cfg = RunConfig(max_plies=2, results_dir=tmp_path / "results", k=1,
                    k_launch=1, candidate_timeout_s=5)

    outcome = run_spec(
        root, ScriptedActor.moves(["e7e5"], delay_s=0.001),
        ScriptedSpeculator.predictions([["e7e5"]], delay_s=0.02), cfg,
        FakeAgentVFS(root=root, factory=candidates), FakeVerifier(), candidates)

    assert outcome.committed_moves == ("e2e4", "e7e5")
    assert [step.ply for step in outcome.steps] == [1]


def test_hit_records_known_and_unknown_candidate_token_usage(tmp_path):
    candidates = FakeCandidateFactory(
        {"d2d4": "d7d5", "e2e4": "e7e5"}, tokens_in=8, tokens_out=2)
    outcome = run_case(
        tmp_path,
        ScriptedActor.moves(["e2e4"], delay_s=0.03),
        ScriptedSpeculator.predictions([["d2d4", "e2e4"]], delay_s=0.001),
        candidates,
        max_plies=1,
    )

    step = outcome.steps[0]
    assert (step.candidate_tokens_in, step.candidate_tokens_out) == (8, 2)
    assert step.candidate_token_usage_unknown == 1


def test_malformed_speculator_writes_excerpt_and_degrades_to_actor(tmp_path):
    """A speculator that raises ``LLMOutputError`` degrades to the actor move
    (no abort) and writes the bounded, sanitized 4 KiB excerpt to
    ``invalid-speculator-ply0.txt``."""
    excerpt = "MALFORMED-SPEC-" + "x" * 6000  # > 4 KiB -> capped by the error
    error = LLMOutputError(
        "speculator yielded no legal moves", role="speculator",
        raw_excerpt=excerpt, latency_s=0.2, tokens_in=12, tokens_out=3)
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.001)
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates = FakeCandidateFactory({})
    candidates.root = root
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=2,
                    k_launch=2, candidate_timeout_s=5)
    outcome = run_spec(root, actor, _RaisingSpeculator(error), cfg,
                       FakeAgentVFS(root=root, factory=candidates),
                       FakeVerifier(), candidates)
    assert outcome.committed_moves == ("e2e4",)
    assert outcome.steps[0].spec_latency_s == 0.2
    assert (outcome.steps[0].spec_tokens_in,
            outcome.steps[0].spec_tokens_out) == (12, 3)
    diag = (tmp_path / "results" / "invalid-speculator-ply0.txt").read_text()
    assert diag == excerpt[:4096]
    # No candidates were launched because there were no predictions.
    assert candidates.launches == []


def test_malformed_actor_writes_excerpt_and_aborts(tmp_path):
    """Malformed actor output writes ``invalid-actor-ply0.txt`` and ABORTS the
    run (re-raises ``LLMOutputError``); cleanup is still retried first."""
    excerpt = "BAD-ACTOR-OUTPUT"
    error = LLMOutputError("actor output rejected", role="actor",
                           raw_excerpt=excerpt)
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates = FakeCandidateFactory({"d2d4": "d7d5"})
    candidates.root = root
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=2,
                    k_launch=2, candidate_timeout_s=5)
    with pytest.raises(LLMOutputError):
        run_spec(root, _RaisingActor(error),
                 ScriptedSpeculator.predictions([["d2d4"]], delay_s=0.03),
                 cfg, FakeAgentVFS(root=root, factory=candidates),
                 FakeVerifier(), candidates)
    diag = (tmp_path / "results" / "invalid-actor-ply0.txt").read_text()
    assert diag == excerpt


# --- consume_prepared / require_cleanup unit coverage -----------------------


def test_consume_prepared_applies_and_unlinks(tmp_path):
    state = GameState.initial().apply("e2e4")  # black to move
    state.save(tmp_path / "game.json")
    PreparedResponse(schema=1, predicted_move="e2e4", prepared_move="e7e5",
                     position_fen=state.fen).save(
        tmp_path / "prepared_response.json")
    move = consume_prepared(tmp_path)
    assert move == "e7e5"
    assert not (tmp_path / "prepared_response.json").exists()
    assert GameState.load(tmp_path / "game.json").moves == ("e2e4", "e7e5")


def test_require_cleanup_clears_list_when_all_succeed(tmp_path):
    factory = FakeCandidateFactory({"d2e4": "d7d5"})
    factory.root = tmp_path
    a = FakeCandidateAttempt("b1", "d2d4", "d7d5", factory, 0.01)
    b = FakeCandidateAttempt("b2", "e2e4", "e7e5", factory, 0.01)
    attempts = [a, b]
    require_cleanup(attempts)
    assert attempts == []
    assert a.cleaned and b.cleaned


def test_require_cleanup_raises_when_resources_remain(tmp_path):
    factory = FakeCandidateFactory({"d2d4": "d7d5"})
    factory.root = tmp_path
    sticky = FakeCandidateAttempt("sticky", "d2d4", "d7d5", factory, 0.01)
    sticky._cleanup_fail_first = 10  # always fails
    attempts = [sticky]
    with pytest.raises(CleanupError):
        require_cleanup(attempts)


# --- Regression tests (whole-branch review) ---------------------------------


class _ReflectingHitVerifier(FakeVerifier):
    """Mirrors the real Verifier.check_hit_merge semantics so the C1 ordering
    bug is visible offline. The real check (src/verifier.py:125, docstring
    "After the winner is merged into main") reads main's game.json at call
    time and requires it to equal what the candidate cgroup observes -- i.e.
    main must ALREADY hold the merged candidate state. FakeAgentVFS.branch_merge
    stages that candidate state onto main, so under the correct (merge-then-
    check) order main matches; under the old (check-before-merge) order main
    still held the pre-window state and the real check always raised."""

    def __init__(self, root, factory):
        super().__init__()
        self.root = root
        self.factory = factory

    def check_hit_merge(self, attempt):
        self.calls.append("check_hit_merge")
        main_state = GameState.load(self.root / "game.json")
        if attempt.branch not in self.factory.branch_states:
            raise VerificationError(
                f"no branch state recorded for {attempt.branch}")
        cand_state, _prepared = self.factory.branch_states[attempt.branch]
        if main_state.fen != cand_state.fen:
            raise VerificationError(
                "hit-merge mismatch: main not yet merged "
                f"(main fen {main_state.fen!r} != branch fen "
                f"{cand_state.fen!r})")


def test_check_hit_merge_runs_after_branch_merge(tmp_path):
    """Regression (C1): check_hit_merge is a POST-merge check, so branch_merge
    must run FIRST. Under the old order (check_hit_merge then branch_merge)
    main still held the pre-window state (S0) while the branch held
    S0+predicted_move, so the real Verifier.check_hit_merge ALWAYS raised
    VerificationError on a hit -- invisible because the offline FakeVerifier
    is a no-op stub. The reflecting verifier above fails under the old order
    (raises before the run completes) and passes after the swap."""
    actor = ScriptedActor.moves(["e2e4"], delay_s=0.03)
    speculator = ScriptedSpeculator.predictions([["e2e4"]], delay_s=0.001)
    candidates = FakeCandidateFactory({"e2e4": "e7e5"})
    root = tmp_path / "work"
    root.mkdir()
    GameState.initial().save(root / "game.json")
    candidates.root = root
    cfg = RunConfig(max_plies=1, results_dir=tmp_path / "results", k=2,
                    k_launch=2, candidate_timeout_s=5)
    avfs = FakeAgentVFS(root=root, factory=candidates)
    verifier = _ReflectingHitVerifier(root=root, factory=candidates)
    outcome = run_spec(root, actor, speculator, cfg, avfs, verifier, candidates)
    assert outcome.steps[0].prelaunched_hit
    assert outcome.committed_moves == ("e2e4",)
    assert "check_hit_merge" in verifier.calls
