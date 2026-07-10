import sys

from src.llm_client import FakeActor, FakeSpeculator, Guess
from src.runner_regular import RunConfig
from src.runner_spec import replay_check, run_spec
from src.verifier import ShadowModel, Verifier
from tests.conftest import STUB_ARGV, cgroup_or_skip, make_avfs

E1 = "# experiment 1\n"
E2 = "# experiment 2\n"
# the wrong guess must differ in CODE, not comments: hit detection uses
# normalized (comment-stripped) equality, so comment-only texts all match
E2_WRONG = "wrong_guess = True\n"

_cg_factory = cgroup_or_skip


def _setup(daemon, tmp_path):
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel()
    shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "results")
    cfg = RunConfig(steps=2, budget_s=1, timeout_s=60, python_bin=sys.executable,
                    trainer_argv=STUB_ARGV, results_dir=tmp_path / "results")
    return a, shadow, v, cfg


def test_hit_path_merges_speculative_branch(daemon, tmp_path):
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    actor = FakeActor(script=[(True, E1), (True, E2)], latency_s=3.0)
    # step0 guess correct (E1), step1 guess correct (E2)
    spec = FakeSpeculator(script=[[Guess(E1, 0.9)], [Guess(E2, 0.9)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert all(s.prelaunched_hit for s in steps)
    assert any(r.verb == "merge" for r in a.verbs)
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E2
    assert committed == [E1, E2]
    # speculation overlapped actor latency: 2 steps × (3s actor ∥ 1s train) ≈ 6-8s,
    # sequential would be ≥ 2 × (3+1) = 8s; assert some overlap materialized
    assert wall < 2 * (3.0 + 1.0) + 1.0


def test_miss_path_kills_and_deletes(daemon, tmp_path):
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E1)], latency_s=2.0)
    spec = FakeSpeculator(script=[[Guess(E2_WRONG, 0.9)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert not steps[0].predicted_hit
    assert any(r.verb == "branch_delete" for r in a.verbs)
    assert not any(r.verb == "merge" for r in a.verbs)
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1  # real edit executed
    assert committed == [E1]


def test_speculator_failure_degrades_to_sequential(daemon, tmp_path):
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E1)], latency_s=0.5)
    spec = FakeSpeculator(script=[[Guess(E1, 0.9)]], fail_steps={0})
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert len(steps) == 1 and steps[0].spec_latency_s == 0.0
    assert not steps[0].predicted_hit
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1
    # the degrade must be counted in totals, not silently swallowed
    import csv
    with open(cfg.results_dir / "totals.csv") as f:
        totals = next(csv.DictReader(f))
    assert totals["degraded_steps"] == "1"


def test_failed_experiment_on_miss_path_continues(daemon, tmp_path):
    """Trainer failure on an actor edit must roll back and continue, not
    abort; the failed edit is not committed (replay must not see it).
    Speculator degrades at step 0 so no cgroup is needed."""
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    actor = FakeActor(script=[(True, "# CRASH\n"), (True, E1)])
    spec = FakeSpeculator(script=[[], []], fail_steps={0, 1})
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert len(steps) == 2
    assert steps[0].decision == "failed" and steps[0].val_bpb is None
    assert steps[1].decision != "failed"
    assert v.failures == 0
    assert committed == [E1]
    assert (daemon.mount / "train.py").read_text() == E1


def test_actor_first_skips_prelaunch_counts_window_missed(daemon, tmp_path):
    """Cancel guard: when the actor returns before the speculator, the step
    must skip pre-launch entirely (no branch verbs, no cgroup) and count the
    missed window; the real edit executes as usual."""
    def must_not_launch(name):
        raise AssertionError("pre-launch must not happen on a missed window")
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E1)], latency_s=0.0)
    spec = FakeSpeculator(script=[[Guess(E1, 0.9)]], latency_s=2.0)
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=must_not_launch)
    assert steps[0].window_missed
    assert not steps[0].predicted_hit and not steps[0].prelaunched_hit
    assert not any(r.verb == "branch_create" for r in a.verbs)
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1
    assert committed == [E1]
    import csv
    with open(cfg.results_dir / "totals.csv") as f:
        totals = next(csv.DictReader(f))
    assert totals["windows_missed"] == "1"


def test_kway_launch_failure_degrades_per_attempt(daemon, tmp_path):
    """Every guess gets its own launch attempt; a cg-factory failure poisons
    only that attempt and the step falls back to real execution (no cgroups
    needed — the factory fails before any cgroup exists)."""
    calls = []

    def failing_factory(name):
        calls.append(name)
        raise RuntimeError("no cgroups in this test")

    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E1)], latency_s=2.0)
    spec = FakeSpeculator(script=[[Guess(E2_WRONG, 0.9),
                                   Guess("also_wrong = 1\n", 0.5)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=2, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=failing_factory)
    assert len(calls) == 2                       # both attempts were tried
    assert sum(1 for r in a.verbs if r.verb == "branch_create") == 2
    assert sum(1 for r in a.verbs if r.verb == "branch_delete") == 2
    assert not steps[0].prelaunched_hit
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1
    assert committed == [E1]


def test_kway_hit_at_second_guess_merges_winner_deletes_all(daemon, tmp_path):
    """k-way pre-launch: a hit on a non-top guess merges THAT branch; every
    speculative branch is deleted afterwards (cgroup-gated)."""
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E2)], latency_s=3.0)
    spec = FakeSpeculator(script=[[Guess(E2_WRONG, 0.9), Guess(E2, 0.5)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=2, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert steps[0].prelaunched_hit and steps[0].hit_idx == 1
    assert steps[0].head_start_s > 0
    assert sum(1 for r in a.verbs if r.verb == "branch_create") == 2
    assert sum(1 for r in a.verbs if r.verb == "merge") == 1
    assert sum(1 for r in a.verbs if r.verb == "branch_delete") == 2
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E2
    assert committed == [E2]
    # spec logs must live OFF-mount: a main-side log created between two
    # branch forks lands in the later fork's snapshot and merge-conflicts
    assert not list(daemon.mount.glob("*spec-log*"))


def test_kway_all_miss_deletes_every_branch(daemon, tmp_path):
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E1)], latency_s=2.0)
    spec = FakeSpeculator(script=[[Guess(E2_WRONG, 0.9),
                                   Guess("also_wrong = 1\n", 0.5)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=2, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert not steps[0].predicted_hit and steps[0].hit_idx is None
    assert sum(1 for r in a.verbs if r.verb == "branch_create") == 2
    assert sum(1 for r in a.verbs if r.verb == "branch_delete") == 2
    assert not any(r.verb == "merge" for r in a.verbs)
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1
    assert committed == [E1]
    assert not list(daemon.mount.glob("*spec-log*"))  # no junk left on main


def test_k_launch_caps_prelaunches_but_not_accuracy(daemon, tmp_path):
    """Selective branch launching (paper §5): only the top k_launch guesses
    are pre-launched, but validation still scores ALL k guesses — a hit at an
    unlaunched index counts for accuracy without a prelaunched hit. Uses a
    failing cg factory, so no cgroups are needed."""
    calls = []

    def failing_factory(name):
        calls.append(name)
        raise RuntimeError("no cgroups in this test")

    a, shadow, v, cfg = _setup(daemon, tmp_path)
    cfg.steps = 1
    actor = FakeActor(script=[(True, E2)], latency_s=2.0)
    spec = FakeSpeculator(script=[[Guess(E2_WRONG, 0.9),
                                   Guess("also_wrong = 1\n", 0.8),
                                   Guess(E2, 0.7)]])
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=3, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=failing_factory, k_launch=2)
    assert len(calls) == 2                      # top-2 launched, third not
    assert sum(1 for r in a.verbs if r.verb == "branch_create") == 2
    assert steps[0].predicted_hit and steps[0].hit_idx == 2
    assert not steps[0].prelaunched_hit        # the hit was never launched
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E2
    assert committed == [E2]


def test_discard_hit_merges_without_rollback(daemon, tmp_path):
    """A text-matched branch is valid on a DISCARD step too: apply_edit writes
    the complete next file, so the branch's tracked state is independent of
    the fork base. The hit must merge (no main rollback — the merge subsumes
    it, and a prior rollback would modify/modify-conflict on train.py).
    Cgroup-gated. Regression: 20-step run voided 2/2 correct top-1 guesses
    because both landed on discard steps (18/20 decisions are discards)."""
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    # step 0: speculator degrades; main trains E1, best = step-0 checkpoint
    # step 1: actor DISCARDS and proposes E2; the guess matches E2
    actor = FakeActor(script=[(True, E1), (False, E2)], latency_s=3.0)
    spec = FakeSpeculator(script=[[], [Guess(E2, 0.9)]], fail_steps={0})
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert steps[1].decision == "discard"
    assert steps[1].prelaunched_hit and steps[1].hit_idx == 0
    assert not any(r.verb == "rollback" and r.step == 1 for r in a.verbs)
    assert sum(1 for r in a.verbs if r.verb == "merge") == 1
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E2
    assert committed == [E1, E2]


def test_discard_hit_failed_training_rolls_back_to_best(daemon, tmp_path):
    """A discard-step hit whose branch training FAILS must still restore main
    to the best state (the discard rollback was skipped for the hit, and main
    holds the discarded experiment's file). Cgroup-gated."""
    E_CRASH = 'crash = "CRASH"\n'  # code-different from E1; stub exits 1 on it
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    actor = FakeActor(script=[(True, E1), (False, E_CRASH)], latency_s=3.0)
    spec = FakeSpeculator(script=[[], [Guess(E_CRASH, 0.9)]], fail_steps={0})
    steps, wall, committed = run_spec(daemon.mount, actor, spec, k=1, cfg=cfg,
                                      avfs=a, verifier=v, shadow=shadow,
                                      cg_factory=_cg_factory)
    assert steps[1].decision == "failed"
    assert any(r.verb == "rollback" and r.step == 1 for r in a.verbs)
    assert not any(r.verb == "merge" for r in a.verbs)
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E1  # back at best
    assert committed == [E1]


def test_runner_accumulates_edit_history_for_speculator(daemon, tmp_path):
    """Each step's actor edit is recorded as a diff and passed to the next
    predict() call (no guesses -> no cgroups needed)."""
    class _Capturing(FakeSpeculator):
        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.seen = []

        def predict(self, history, train_py, k, edit_history=None):
            self.seen.append(list(edit_history or []))
            return super().predict(history, train_py, k)

    a, shadow, v, cfg = _setup(daemon, tmp_path)
    actor = FakeActor(script=[(True, E1), (True, E2)], latency_s=1.0)
    spec = _Capturing(script=[[], []])
    run_spec(daemon.mount, actor, spec, k=1, cfg=cfg, avfs=a, verifier=v,
             shadow=shadow, cg_factory=_cg_factory)
    assert spec.seen[0] == []                      # nothing before step 0
    assert len(spec.seen[1]) == 1                  # step 0's edit as a diff
    assert "+# experiment 1" in spec.seen[1][0]
    assert spec.seen[1][0].startswith("step 0")


def test_replay_check_lossless(tmp_path):
    cfg = RunConfig(steps=0, budget_s=1, timeout_s=60, python_bin=sys.executable,
                    trainer_argv=STUB_ARGV, results_dir=tmp_path / "r")
    ok, traj = replay_check([E1, E2], "# base\n", cfg, tmp_path / "replay",
                            expected_final=E2)
    assert ok
    assert len(traj) == 2
    traj2 = replay_check([E1, E2], "# base\n", cfg, tmp_path / "replay2",
                         expected_final=E2)[1]
    assert traj == traj2  # deterministic stub => identical trajectory
    # divergence between mount content and replay must be detected
    bad, _ = replay_check([E1, E2], "# base\n", cfg, tmp_path / "replay3",
                          expected_final=E1)
    assert not bad


def test_replay_check_seeds_full_base_tree(tmp_path):
    """The real train.py imports prepare.py — replay must copy the whole seed
    tree, not just train.py (regression: replay ModuleNotFoundError)."""
    seed = tmp_path / "seed"
    seed.mkdir()
    (seed / "train.py").write_text("# base\n")
    (seed / "prepare.py").write_text("MAX_SEQ_LEN = 1\n")
    cfg = RunConfig(steps=0, budget_s=1, timeout_s=60, python_bin=sys.executable,
                    trainer_argv=STUB_ARGV, results_dir=tmp_path / "r")
    ok, traj = replay_check([E1], "# base\n", cfg, tmp_path / "replay",
                            expected_final=E1, seed_dir=seed)
    assert ok and len(traj) == 1
    assert (tmp_path / "replay" / "prepare.py").read_text() == "MAX_SEQ_LEN = 1\n"


def test_spec_with_warm_main_trainer(daemon, tmp_path):
    """Warm main-path trainer coexists with a fresh speculative branch
    pre-launch (cgroup-gated; skips without cgroups via _cg_factory)."""
    import sys as _sys
    from src.warm import WarmTrainer
    from tests.conftest import require_coop
    require_coop()
    a, shadow, v, cfg = _setup(daemon, tmp_path)
    warm = WarmTrainer(a, daemon.mount, _sys.executable, tmp_path,
                       budget_s=1, warm_import="none",
                       snapshot_timeout_ms=30000)
    warm.launch()
    try:
        W1 = ("print('---')\nprint('val_bpb:          2.100000')\n"
              "print('training_seconds: 0.1')\n")
        cfg.steps = 1
        cfg.trainer_argv = None
        actor = FakeActor(script=[(True, W1)], latency_s=2.0)
        # code-different guess => guaranteed miss; the fresh branch
        # pre-launch runs it (exits 0 harmlessly) and is killed on miss.
        spec = FakeSpeculator(script=[[Guess("wrong_guess = True\n", 0.9)]])
        steps, wall, committed = run_spec(
            daemon.mount, actor, spec, k=1, cfg=cfg, avfs=a, verifier=v,
            shadow=shadow, cg_factory=_cg_factory, warm=warm)
        assert steps[0].val_bpb == 2.1          # miss -> warm main training
        assert not steps[0].predicted_hit
        assert v.failures == 0
    finally:
        warm.close()
