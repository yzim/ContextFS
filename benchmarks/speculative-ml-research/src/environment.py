"""Autoresearch environment: seed tree, apply edits, run training, parse metric."""
import os
import re
import shutil
import signal
import subprocess
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

VENDOR_DIR = Path(__file__).resolve().parents[1] / "vendor" / "autoresearch"
SEED_FILES = ["train.py", "prepare.py", "pyproject.toml", "program.md"]
VAL_RE = re.compile(r"^val_bpb:\s+([0-9.]+)", re.MULTILINE)
SECS_RE = re.compile(r"^training_seconds:\s+([0-9.]+)", re.MULTILINE)


@dataclass
class TrainResult:
    val_bpb: float | None
    train_seconds: float
    ok: bool
    raw: str


def seed_base_tree(dest: Path, autoresearch_dir: Path = VENDOR_DIR) -> None:
    dest.mkdir(parents=True, exist_ok=True)
    for f in SEED_FILES:
        src = Path(autoresearch_dir) / f
        if src.exists():
            shutil.copy2(src, Path(dest) / f)


def parse_train_output(text: str) -> TrainResult:
    m = VAL_RE.search(text)
    s = SECS_RE.search(text)
    if m:
        return TrainResult(float(m.group(1)), float(s.group(1)) if s else 0.0, True, text)
    return TrainResult(None, 0.0, False, text)


def apply_edit(workdir: Path, content: str, cg=None) -> None:
    if cg is None:
        (Path(workdir) / "train.py").write_text(content)
    else:
        # write via a child inside the cgroup so it routes to the branch;
        # pipe through stdin so the bytes land exactly (a heredoc would
        # append a newline and break shadow-hash verification)
        cg.run_text(["bash", "-c", "cat > train.py"],
                    cwd=workdir, timeout=30, input_text=content)


def launch_training(python_bin: str, workdir: Path, budget_s: int, cg, log_path: Path,
                    argv: list | None = None):
    argv = argv or [python_bin, "train.py"]
    log = open(log_path, "wb")
    # Both paths extend os.environ: train.py resolves its dataset via
    # ~/.cache (HOME) and the venv interpreter needs PATH intact.
    # PYTHONPYCACHEPREFIX keeps bytecode caching but points it off the
    # workdir: sibling imports otherwise stat/write __pycache__ through the
    # zero-cache FUSE mount on every fresh launch (host-local environment
    # state, not workload). An existing os.environ value wins.
    env = {**os.environ,
           "PYTHONPYCACHEPREFIX": os.environ.get(
               "PYTHONPYCACHEPREFIX",
               os.path.join(tempfile.gettempdir(), "sml-pycache")),
           "TRAIN_BUDGET_S": str(budget_s), "OMP_NUM_THREADS": "4"}
    if cg is None:
        proc = subprocess.Popen(argv, cwd=str(workdir),
                                env=env, stdout=log,
                                stderr=subprocess.STDOUT, start_new_session=True)
    else:
        proc = cg.run(argv, cwd=workdir, env=env, stdout=log)
    # The child holds its own dup of the fd; close the parent's copy so it
    # doesn't leak. collect_training reads the log by PATH, not via this fd.
    log.close()
    return proc


def collect_training(proc, log_path: Path, timeout_s: int) -> TrainResult:
    try:
        proc.wait(timeout=timeout_s)
    except subprocess.TimeoutExpired:
        # trainers run with start_new_session=True: kill the whole group so
        # trainer-spawned children (DataLoader workers) die too
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            proc.kill()
        proc.wait()
    text = Path(log_path).read_text(errors="replace")
    r = parse_train_output(text)
    if proc.returncode != 0:
        return TrainResult(r.val_bpb, r.train_seconds, False, text)
    return r


def run_training(python_bin: str, workdir: Path, budget_s: int, timeout_s: int,
                 cg=None, log_path: Path | None = None,
                 argv: list | None = None) -> TrainResult:
    log_path = log_path or Path(workdir) / ".train-log.txt"
    t0 = time.time()
    proc = launch_training(python_bin, workdir, budget_s, cg, log_path, argv=argv)
    res = collect_training(proc, log_path, timeout_s)
    return TrainResult(res.val_bpb, res.train_seconds or (time.time() - t0), res.ok, res.raw)
