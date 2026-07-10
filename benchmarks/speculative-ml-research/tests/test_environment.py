import subprocess
import sys
from pathlib import Path

import pytest

from src import environment, overlay
from src.environment import VENDOR_DIR as VENDOR
from tests.conftest import STUB_ARGV


def test_overlay_patterns_match_vendored_tree(tmp_path):
    """Integrity guard: every overlay pattern must occur exactly
    expected_count times in the vendored files."""
    environment.seed_base_tree(tmp_path, VENDOR)
    overlay.apply_overlay(tmp_path)  # raises OverlayDriftError on mismatch
    text = (tmp_path / "train.py").read_text()
    assert "cuda" not in text.lower().replace("no-cuda-ok", "")
    assert "get_kernel" not in text
    prep = (tmp_path / "prepare.py").read_text()
    assert 'device="cuda"' not in prep
    assert "TRAIN_BUDGET_S" in prep
    # anchor splices must leave the files syntactically valid
    import py_compile
    py_compile.compile(str(tmp_path / "train.py"), doraise=True)
    py_compile.compile(str(tmp_path / "prepare.py"), doraise=True)


def test_data_preconditions_fail_closed_on_missing_cache(tmp_path, monkeypatch):
    """Real-training runs must abort at startup when ~/.cache/autoresearch is
    absent (e.g. `sudo env` reset HOME) instead of failing every experiment
    after the API spend."""
    import main as sml_main
    from src.runner_regular import RunConfig
    rc = RunConfig(steps=1, budget_s=1, timeout_s=1, python_bin=sys.executable,
                   trainer_argv=None, results_dir=tmp_path)
    monkeypatch.setenv("HOME", str(tmp_path))   # no dataset cache under here
    with pytest.raises(SystemExit):
        sml_main.data_preconditions(rc)
    rc.trainer_argv = STUB_ARGV                 # stub runs need no dataset
    sml_main.data_preconditions(rc)


def test_launch_training_cgroup_env_keeps_environ(tmp_path):
    """The cgroup trainer env must extend os.environ (HOME, PATH, ...), not
    replace it — train.py resolves its dataset via ~/.cache."""
    seen = {}

    class FakeCg:
        def run(self, argv, cwd, env, stdout):
            seen["env"] = env
            return subprocess.Popen(["true"], stdout=stdout)

    proc = environment.launch_training(
        sys.executable, tmp_path, budget_s=1, cg=FakeCg(),
        log_path=tmp_path / "log.txt")
    proc.wait(timeout=10)
    assert seen["env"]["TRAIN_BUDGET_S"] == "1"
    assert "HOME" in seen["env"] and "PATH" in seen["env"]


def test_collect_training_timeout_kills_process_group(tmp_path):
    """A timed-out trainer's own children (DataLoader workers in real runs)
    must die with it — trainers run with start_new_session=True, so the
    timeout kill must target the process group, not just the direct child."""
    import os
    import re
    import time
    script = tmp_path / "t.py"
    script.write_text(
        "import subprocess, sys, time\n"
        "child = subprocess.Popen([sys.executable, '-c',"
        " 'import time; time.sleep(60)'])\n"
        "print(f'CHILD {child.pid}', flush=True)\n"
        "time.sleep(60)\n")
    log = tmp_path / "log.txt"
    proc = environment.launch_training(sys.executable, tmp_path, budget_s=1,
                                       cg=None, log_path=log,
                                       argv=[sys.executable, str(script)])
    child_pid = None
    deadline = time.time() + 10
    while time.time() < deadline and child_pid is None:
        m = re.search(r"CHILD (\d+)", log.read_text()) if log.exists() else None
        if m:
            child_pid = int(m.group(1))
        time.sleep(0.05)
    assert child_pid, "trainer never reported its child"
    res = environment.collect_training(proc, log, timeout_s=1)
    assert not res.ok
    deadline = time.time() + 5
    while time.time() < deadline:
        try:
            os.kill(child_pid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        pytest.fail(f"trainer child {child_pid} survived the timeout kill")


def test_seed_does_not_touch_vendor(tmp_path):
    before = (VENDOR / "train.py").read_bytes()
    environment.seed_base_tree(tmp_path, VENDOR)
    overlay.apply_overlay(tmp_path)
    assert (VENDOR / "train.py").read_bytes() == before


class PassthroughSession:
    """Runs argv like CgroupSession.run_text but without cgroup enrollment,
    so the byte-fidelity of the cgroup write path is testable anywhere."""

    def run_text(self, argv, cwd, timeout=60, input_text=None) -> str:
        r = subprocess.run(argv, cwd=str(cwd), capture_output=True, text=True,
                           timeout=timeout, input=input_text)
        if r.returncode != 0:
            raise RuntimeError(f"run {argv} rc={r.returncode}: {r.stderr}")
        return r.stdout


@pytest.mark.parametrize("content", [
    "x = 1",              # no trailing newline
    "# experiment 1\n",   # trailing newline
    "a\n\nb\n",           # blank line inside
    "",                   # empty file
])
def test_apply_edit_roundtrip_via_cgroup_path(tmp_path, content):
    environment.apply_edit(tmp_path, content, cg=PassthroughSession())
    assert (tmp_path / "train.py").read_text() == content


def test_parse_train_output():
    raw = "Scaling AdamW...\n---\nval_bpb:          1.234567\ntraining_seconds: 30.1\n"
    r = environment.parse_train_output(raw)
    assert r.ok and abs(r.val_bpb - 1.234567) < 1e-9
    assert abs(r.train_seconds - 30.1) < 1e-9
    bad = environment.parse_train_output("Traceback (most recent call last):\n...")
    assert not bad.ok and bad.val_bpb is None


def test_stub_trainer_deterministic(tmp_path):
    def run_stub():
        out = subprocess.run(STUB_ARGV, cwd=tmp_path, capture_output=True,
                             text=True, env={"STUB_SLEEP_S": "0"}).stdout
        return environment.parse_train_output(out)

    (tmp_path / "train.py").write_text("# experiment 1\n")
    r1, r2 = run_stub(), run_stub()
    assert r1.ok and r1.val_bpb == r2.val_bpb
    (tmp_path / "train.py").write_text("# experiment 1\n# REGRESS\n")
    r3 = run_stub()
    assert r3.val_bpb > r1.val_bpb  # REGRESS marker worsens the metric


@pytest.mark.slow
def test_real_cpu_training_30s(tmp_path):
    """Real CPU training. Needs: uv sync --extra train, network-prepared data in
    ~/.cache/autoresearch (run prepare.py once). Run: uv run pytest -m slow"""
    torch = pytest.importorskip("torch")
    tok = Path.home() / ".cache" / "autoresearch" / "tokenizer" / "tokenizer.pkl"
    if not tok.exists():
        pytest.skip("autoresearch data not prepared (run prepare.py once)")
    environment.seed_base_tree(tmp_path, VENDOR)
    overlay.apply_overlay(tmp_path)
    r = environment.run_training(sys.executable, tmp_path, budget_s=30, timeout_s=900)
    assert r.ok, r.raw[-2000:]
    assert r.val_bpb is not None and 0 < r.val_bpb < 10


@pytest.mark.slow
def test_warm_real_training_cycle(daemon, tmp_path):
    """One real warm experiment: torch template + restore + 30s budget.
    This is where torch-after-fork/OpenMP gets its empirical answer; if the
    resumed generation hangs in its first parallel region, pin
    torch.set_num_threads(1) in the driver's post-boundary block."""
    import sys as _sys
    from src.warm import WarmTrainer
    from tests.conftest import make_avfs, require_coop
    require_coop()
    pytest.importorskip("torch")
    tok = Path.home() / ".cache" / "autoresearch" / "tokenizer" / "tokenizer.pkl"
    if not tok.exists():
        pytest.skip("autoresearch data not prepared (run prepare.py once)")
    environment.seed_base_tree(daemon.mount, VENDOR)
    overlay.apply_overlay(daemon.mount)
    a = make_avfs(daemon)
    w = WarmTrainer(a, daemon.mount, _sys.executable, tmp_path, budget_s=30,
                    warm_import="torch")
    w.launch()
    try:
        w.restore(0)
        res = w.run_experiment(0, timeout_s=900)
        assert res.ok, res.raw[-2000:]
        assert res.val_bpb is not None and 0 < res.val_bpb < 10
    finally:
        w.close()


def _sibling_import_workdir(tmp_path):
    work = tmp_path / "work"
    work.mkdir()
    (work / "sibling.py").write_text("VALUE = 2.5\n")
    (work / "train.py").write_text(
        "from sibling import VALUE\n"
        "print(f'val_bpb:          {VALUE:.6f}')\n"
        "print('training_seconds: 0.1')\n")
    return work


def test_trainer_pycache_lands_off_workdir_by_default(tmp_path, monkeypatch):
    """Python bytecode cache is host-local environment state, not workload:
    launch_training must point PYTHONPYCACHEPREFIX off the (FUSE) workdir so
    sibling imports don't stat/write __pycache__ through the mount — even
    when the caller's environment does not set it."""
    monkeypatch.delenv("PYTHONPYCACHEPREFIX", raising=False)
    monkeypatch.delenv("PYTHONDONTWRITEBYTECODE", raising=False)
    work = _sibling_import_workdir(tmp_path)
    res = environment.run_training(sys.executable, work, budget_s=1, timeout_s=30)
    assert res.ok and res.val_bpb == 2.5
    assert not (work / "__pycache__").exists()


def test_trainer_pycache_prefix_override_respected(tmp_path, monkeypatch):
    # the sudo gate runs pytest under PYTHONDONTWRITEBYTECODE=1, which the
    # trainer would inherit and write no pyc at all — clear it: this test is
    # about WHERE bytecode goes when it is written
    monkeypatch.delenv("PYTHONDONTWRITEBYTECODE", raising=False)
    monkeypatch.setenv("PYTHONPYCACHEPREFIX", str(tmp_path / "pyc"))
    work = _sibling_import_workdir(tmp_path)
    res = environment.run_training(sys.executable, work, budget_s=1, timeout_s=30)
    assert res.ok
    assert not (work / "__pycache__").exists()
    assert list((tmp_path / "pyc").rglob("sibling*.pyc"))
