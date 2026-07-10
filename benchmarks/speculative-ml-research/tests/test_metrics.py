from src.agentvfs import VerbRecord
from src.metrics import warm_restore_metrics


def _restores(n, latency_us=10_000):
    return [VerbRecord("runtime_restore", s, latency_us, "ok", 0)
            for s in range(n)]


def test_warm_restore_metrics_excludes_recovery_restores():
    """Pins the M6 arithmetic: a recovery restore is a real restore
    observation (restores/mean) but saves no startup."""
    verbs = _restores(3) + [VerbRecord("checkpoint", 0, 5_000, "ok", 0)]
    extra = warm_restore_metrics(verbs, startup_baseline_s=2.01,
                                 recovery_restores=1)
    assert extra["restores"] == 3
    assert extra["mean_restore_ms"] == 10.0
    assert extra["startup_saved_s"] == 4.0   # (2.01 - 0.01) x 2 main-path


def test_warm_restore_metrics_no_restores():
    extra = warm_restore_metrics([], startup_baseline_s=2.0,
                                 recovery_restores=0)
    assert extra == {"restores": 0, "mean_restore_ms": 0.0,
                     "startup_saved_s": 0.0}


def test_step_record_speculation_bookkeeping_and_totals():
    from src.metrics import StepRecord, compute_totals
    # pre-realignment call sites pass 8 positional fields; new fields default
    plain = StepRecord(0, 1.0, 0.5, False, False, 2.0, 2.1, "keep")
    assert plain.hit_idx is None
    assert plain.window_missed is False
    assert plain.spec_apply_failures == 0
    assert plain.head_start_s == 0.0
    hit = StepRecord(1, 1.0, 0.5, True, True, 2.0, 2.0, "keep",
                     hit_idx=1, spec_apply_failures=2, head_start_s=1.5)
    missed = StepRecord(2, 1.0, 0.0, False, False, 2.0, 2.2, "keep",
                        window_missed=True)
    t = compute_totals([plain, hit, missed], [], 10.0)
    assert t["windows_missed"] == 1
    assert t["spec_apply_failures"] == 2


def test_step_record_train_wall_and_total():
    """train_wall_s is the SUBPROCESS wall (launch->collect) — startup, data
    load, eval and log I/O included — vs train_time_s (trainer's self-reported
    in-loop seconds). The FUSE tax hides in the difference."""
    from src.metrics import StepRecord, compute_totals
    plain = StepRecord(0, 1.0, 0.0, False, False, 2.0, 2.1, "keep")
    assert plain.train_wall_s == 0.0
    timed = StepRecord(1, 1.0, 0.0, False, False, 2.0, 2.1, "keep",
                       train_wall_s=3.5)
    t = compute_totals([plain, timed], [], 10.0)
    assert t["train_wall_total_s"] == 3.5
