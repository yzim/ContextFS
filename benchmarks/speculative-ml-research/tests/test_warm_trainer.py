import sys

import pytest

from src.agentvfs import CtlError
from src.warm import WarmTrainer
from src.verifier import hash_tree
from tests.conftest import make_avfs, require_coop

T1 = "print('---')\nprint('val_bpb:          2.100000')\nprint('training_seconds: 0.1')\n"
T2 = "print('---')\nprint('val_bpb:          2.050000')\nprint('training_seconds: 0.1')\n"
T_BOOM = "raise RuntimeError('broken edit')\n"
T_EXIT = ("print('---')\nprint('val_bpb:          2.100000')\n"
          "print('training_seconds: 0.1')\nimport sys\nsys.exit(3)\n")
T_HANG = "import time\ntime.sleep(30)\n"


def _setup(daemon, tmp_path):
    require_coop()
    (daemon.mount / "train.py").write_text("# base\n")
    a = make_avfs(daemon)
    w = WarmTrainer(a, daemon.mount, sys.executable, tmp_path, budget_s=1,
                    warm_import="none", snapshot_timeout_ms=30000)
    w.launch()
    return a, w


def test_warm_cycle_restore_edit_train(daemon, tmp_path):
    a, w = _setup(daemon, tmp_path)
    try:
        for step, (content, bpb) in enumerate([(T1, 2.1), (T2, 2.05)]):
            w.restore(step)
            assert hash_tree(daemon.mount) == w.warm_tree  # coupled rollback
            (daemon.mount / "train.py").write_text(content)
            res = w.run_experiment(step, timeout_s=30)
            assert res.ok and abs(res.val_bpb - bpb) < 1e-6
        assert sum(1 for r in a.verbs if r.verb == "runtime_restore") == 2
    finally:
        w.close()


def test_warm_broken_edit_is_failed_experiment(daemon, tmp_path):
    a, w = _setup(daemon, tmp_path)
    try:
        w.restore(0)
        (daemon.mount / "train.py").write_text(T_BOOM)
        res = w.run_experiment(0, timeout_s=30)
        assert not res.ok and "TRAINER-EXCEPTION" in res.raw
        w.restore(1)
        (daemon.mount / "train.py").write_text(T1)
        assert w.run_experiment(1, timeout_s=30).ok
    finally:
        w.close()


def test_warm_nonzero_exit_is_failed_experiment(daemon, tmp_path):
    """Parity with fresh mode: collect_training fails rc!=0 even when
    val_bpb was printed, so the warm driver must too."""
    a, w = _setup(daemon, tmp_path)
    try:
        w.restore(0)
        (daemon.mount / "train.py").write_text(T_EXIT)
        res = w.run_experiment(0, timeout_s=30)
        assert not res.ok and "SystemExit(3)" in res.raw
    finally:
        w.close()


def test_warm_timeout_recovery_restore(daemon, tmp_path):
    a, w = _setup(daemon, tmp_path)
    try:
        w.restore(0)
        (daemon.mount / "train.py").write_text(T_HANG)
        res = w.run_experiment(0, timeout_s=2)
        assert not res.ok and "timeout" in res.raw
        assert w.recovery_restores == 1   # excluded from startup_saved_s
        # the recovery restore leaves the trainer serviceable
        w.restore(1)
        (daemon.mount / "train.py").write_text(T1)
        assert w.run_experiment(1, timeout_s=30).ok
    finally:
        w.close()


def test_warm_restore_postcondition_fails_closed(daemon, tmp_path):
    a, w = _setup(daemon, tmp_path)
    try:
        w.restore(0)                    # gen 1 (snapshot) -> gen 2
        assert w.generation == 2
        orig = a.runtime_restore
        # a restore that silently forks no new process (daemon state
        # untouched: same active pgid) must fail closed
        a.runtime_restore = lambda *args, **kw: {
            "ok": True, "target_generation": w.generation}
        try:
            with pytest.raises(RuntimeError, match="postcondition"):
                w.restore(1)
        finally:
            a.runtime_restore = orig
    finally:
        w.close()


def test_warm_close_drops_template(daemon, tmp_path):
    a, w = _setup(daemon, tmp_path)
    union = w.union_id
    w.close()
    import time as _t
    _t.sleep(0.2)   # template consumes the drop
    with pytest.raises(CtlError):
        a.runtime_restore(union)
