import sys

from src.llm_client import FakeActor
from src.metrics import StepRecord
from src.runner_regular import RunConfig, run_regular
from src.verifier import ShadowModel, Verifier
from tests.conftest import STUB_ARGV, make_avfs

E1 = "# experiment 1\n"
E2 = "# experiment 2\n# REGRESS\n"   # stub worsens val_bpb
E3 = "# experiment 3\n"


def _cfg(tmp_path, steps=3):
    return RunConfig(steps=steps, budget_s=1, timeout_s=60, python_bin=sys.executable,
                     trainer_argv=STUB_ARGV, results_dir=tmp_path / "results")


def test_regular_run_on_agentvfs_with_rollback(daemon, tmp_path):
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel()
    shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a, results_dir=tmp_path / "results")
    # step0: E1 improves (keep); step1: E2 regresses -> Actor discards at step2 and
    # proposes E3; discard must ROLL BACK to the step0 checkpoint.
    actor = FakeActor(script=[(True, E1), (True, E2), (False, E3)])
    cfg = _cfg(tmp_path)
    steps, wall = run_regular(daemon.mount, actor, cfg, avfs=a, verifier=v, shadow=shadow)

    assert len(steps) == 3
    assert steps[2].decision == "discard"
    assert any(r.verb == "rollback" for r in a.verbs), "discard must use agentvfs rollback"
    assert any(r.verb == "checkpoint" for r in a.verbs)
    assert v.failures == 0
    assert (cfg.results_dir / "perstep.csv").exists()
    assert (cfg.results_dir / "verbs.csv").exists()
    assert (cfg.results_dir / "totals.csv").exists()
    # after discard+rollback then applying E3, train.py must be E3 (post-edit)
    assert (daemon.mount / "train.py").read_text() == E3


def test_failed_experiment_rolls_back_and_continues(daemon, tmp_path):
    """A crashing/overweight edit is a workload outcome: roll back to best,
    record decision='failed', keep going (regression: 20-step real run
    aborted at step 11 on a slow actor edit)."""
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel()
    shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "results")
    actor = FakeActor(script=[(True, E1), (True, "# CRASH\n"), (True, E3)])
    steps, wall = run_regular(daemon.mount, actor, _cfg(tmp_path), avfs=a,
                              verifier=v, shadow=shadow)
    assert len(steps) == 3
    assert steps[1].decision == "failed" and steps[1].val_bpb is None
    assert steps[2].decision != "failed"
    assert v.failures == 0
    assert (daemon.mount / "train.py").read_text() == E3


def test_regular_run_native_mode(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "train.py").write_text("# base\n")
    actor = FakeActor(script=[(True, E1), (True, E2), (False, E3)])
    cfg = _cfg(tmp_path)
    steps, wall = run_regular(work, actor, cfg, avfs=None)
    assert len(steps) == 3 and wall > 0
    assert steps[0].val_bpb is not None
    # subprocess wall covers startup+eval, so it exceeds the stub's sleep and
    # is recorded even though the stub self-reports a fake training_seconds
    assert all(s.train_wall_s > 0 for s in steps)


def test_regular_run_with_warm_trainer(daemon, tmp_path):
    import sys as _sys
    from src.warm import WarmTrainer
    from tests.conftest import require_coop
    require_coop()
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    shadow = ShadowModel(); shadow.snapshot_baseline(daemon.mount)
    v = Verifier(mount=daemon.mount, shadow=shadow, avfs=a,
                 results_dir=tmp_path / "r")
    warm = WarmTrainer(a, daemon.mount, _sys.executable, tmp_path,
                       budget_s=1, warm_import="none",
                       snapshot_timeout_ms=30000)
    warm.launch()
    try:
        E_A = ("print('---')\nprint('val_bpb:          2.100000')\n"
               "print('training_seconds: 0.1')\n")
        E_B = ("print('---')\nprint('val_bpb:          2.050000')\n"
               "print('training_seconds: 0.1')\n")
        actor = FakeActor(script=[(True, E_A), (True, E_B)])
        cfg = RunConfig(steps=2, budget_s=1, timeout_s=30,
                        python_bin=_sys.executable, trainer_argv=None,
                        results_dir=tmp_path / "r")
        steps, wall = run_regular(daemon.mount, actor, cfg, avfs=a,
                                  verifier=v, shadow=shadow, warm=warm)
        assert [s.val_bpb for s in steps] == [2.1, 2.05]
        assert v.failures == 0
        assert (daemon.mount / "train.py").read_text() == E_B
        assert sum(1 for r in a.verbs if r.verb == "runtime_restore") == 2
    finally:
        warm.close()
