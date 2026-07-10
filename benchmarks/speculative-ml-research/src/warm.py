"""Harness side of the warm cooperative trainer."""
import os
import signal
import subprocess
import time
from pathlib import Path

from src import environment
from src.agentvfs import CtlError
from src.environment import TrainResult
from src.verifier import hash_tree

REPO = Path(__file__).resolve().parents[3]
RUN_BIN = REPO / "build" / "agentvfs-run"


class WarmTrainer:
    def __init__(self, avfs, mount, python_bin, run_dir, budget_s,
                 warm_import="torch", snapshot_timeout_ms=180000):
        self.avfs = avfs
        self.mount = Path(mount)
        self.python_bin = python_bin
        self.run_dir = Path(run_dir)
        self.budget_s = budget_s
        self.warm_import = warm_import
        self.snapshot_timeout_ms = snapshot_timeout_ms
        self.runtime_id = "sml-warm"
        self.proc = None
        self.union_id = self.template_id = self.warm_commit = None
        self.warm_tree = None
        self.generation = self.snapshot_generation = 0
        self.active_pgid = 0
        self.recovery_restores = 0   # timeout cleanups; replace no fresh launch

    def launch(self, agent_state=None):
        (self.mount / ".sml-run").mkdir(exist_ok=True)
        env = {**os.environ,
               "SML_WARM_IMPORT": self.warm_import,
               "TRAIN_BUDGET_S": str(self.budget_s),
               "PYTHONDONTWRITEBYTECODE": "1"}
        env.setdefault("OMP_NUM_THREADS", "4")
        driver = Path(__file__).resolve().parent / "warm_trainer.py"
        with open(self.run_dir / "warm-driver.log", "wb") as log:
            self.proc = subprocess.Popen(
                [str(RUN_BIN), "--sock", self.avfs.sock, "--branch", "main",
                 "--id", self.runtime_id, "--",
                 self.python_bin, str(driver)],
                cwd=str(self.mount), env=env,
                stdout=log, stderr=subprocess.STDOUT)
        # agentvfs-run registers the runtime AFTER forking the child; wait
        # for registration, then snapshot (the driver offers boundaries in a
        # loop, so the blocking request latches the next offer).
        deadline = time.time() + 30
        while True:
            try:
                self.avfs.runtime_status(self.runtime_id)
                break
            except CtlError:
                if time.time() > deadline:
                    raise
                time.sleep(0.05)
        snap = self.avfs.runtime_snapshot(
            self.runtime_id, agent_state=agent_state,
            timeout_ms=self.snapshot_timeout_ms)
        self.union_id = snap["union_state_id"]
        self.template_id = snap["template_id"]
        self.warm_commit = snap["fs_commit"]
        self.warm_tree = hash_tree(self.mount)
        st = self.avfs.runtime_status(self.runtime_id)
        self.generation = self.snapshot_generation = int(
            st.get("generation") or 0)
        self.active_pgid = int(st.get("active_process_group_id") or 0)

    def restore(self, step):
        # Postcondition (process half of the coupled restore): a fresh
        # generation must be live — the tree-hash check alone would pass
        # with the old process still running. Every restore targets
        # snapshot_generation + 1 (restore_runtime: us.runtime_generation
        # + 1), so the generation REPEATS across restores from one template;
        # the proof a new process was forked is the pgid change. The status
        # read is race-free: runtime_restore returns after generation-ready.
        res = self.avfs.runtime_restore(self.union_id, step=step)
        gen = int(res.get("target_generation") or 0)
        st = self.avfs.runtime_status(self.runtime_id)
        live = int(st.get("generation") or 0)
        pgid = int(st.get("active_process_group_id") or 0)
        if not (gen == self.snapshot_generation + 1 and live == gen
                and pgid > 1 and pgid != self.active_pgid):
            raise RuntimeError(
                f"warm restore postcondition failed: target_generation={gen} "
                f"live_generation={live} "
                f"snapshot_generation={self.snapshot_generation} "
                f"active_pgid={pgid} prev_pgid={self.active_pgid}")
        self.generation = gen
        self.active_pgid = pgid

    def run_experiment(self, step, timeout_s) -> TrainResult:
        run = self.mount / ".sml-run"
        t0 = time.time()
        # tmp + rename: the driver polls go's existence, so it must never
        # observe a partially written file
        (run / "go.tmp").write_text(str(step))
        os.rename(run / "go.tmp", run / "go")
        done = run / f"done-{step}"
        while not done.exists():
            if time.time() - t0 > timeout_s:
                raw = self._out_text()   # capture BEFORE the recovery restore
                self.recovery_restores += 1
                self.restore(step)       # freezes/retires the runaway gen
                return TrainResult(None, 0.0, False,
                                   f"warm experiment timeout\n{raw}")
            time.sleep(0.05)
        res = environment.parse_train_output(self._out_text())
        # a driver-reported exception fails the experiment even if a val_bpb
        # line made it out first (parity with fresh mode's rc!=0 handling)
        ok = res.ok and "TRAINER-EXCEPTION" not in res.raw
        return TrainResult(res.val_bpb,
                           res.train_seconds or (time.time() - t0),
                           ok, res.raw)

    def _out_text(self) -> str:
        out = self.mount / ".sml-run" / "out.txt"
        return out.read_text(errors="replace") if out.exists() else ""

    def close(self):
        # Restored generations live in their OWN process groups — terminate()
        # on the launcher never reaches them. Kill the active generation via
        # the daemon's view, then drop the template, then reap the launcher.
        try:
            st = self.avfs.runtime_status(self.runtime_id)
            pgid = int(st.get("active_process_group_id") or 0)
            if pgid > 1:
                os.killpg(pgid, signal.SIGKILL)
        except (CtlError, ProcessLookupError, PermissionError, ValueError):
            pass
        if self.template_id:
            try:
                self.avfs.runtime_drop(self.template_id)
            except CtlError:
                pass
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=10)
