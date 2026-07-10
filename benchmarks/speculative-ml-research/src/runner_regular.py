"""Sequential baseline: Actor decides, environment executes; agentvfs mode adds
checkpoint-per-kept-step, rollback-on-discard, and shadow verification."""
import hashlib
import sys
import time
from dataclasses import dataclass
from pathlib import Path

from src import environment
from src.metrics import (StepRecord, compute_totals, warm_restore_metrics,
                         write_results)
from src.verifier import hash_tree


@dataclass
class RunConfig:
    steps: int
    budget_s: int
    timeout_s: int
    python_bin: str
    results_dir: Path
    trainer_argv: list | None = None
    startup_baseline_s: float = 0.0


def _train(workdir: Path, cfg: RunConfig):
    return environment.run_training(cfg.python_bin, workdir, cfg.budget_s,
                                    cfg.timeout_s, argv=cfg.trainer_argv)


def run_regular(workdir: Path, actor, cfg: RunConfig,
                avfs=None, verifier=None, shadow=None, chain=None, warm=None):
    workdir = Path(workdir)
    history = []
    best_ckpt = None
    best_bpb = None
    native_best_train_py = workdir.joinpath("train.py").read_text()
    steps = []
    actor_tokens = [0, 0]
    t0 = time.time()

    if avfs is not None:
        best_ckpt = avfs.checkpoint("step-base", step=-1)
        best_state = hash_tree(workdir)

    for step in range(cfg.steps):
        d = actor.decide(history, workdir.joinpath("train.py").read_text())
        decision = "keep" if (not history or d.keep) else "discard"

        if history and not d.keep:
            if avfs is not None:
                avfs.rollback(best_ckpt, step=step)
                if verifier is not None:
                    verifier.check_rollback(best_state, f"step{step}-discard")
                    shadow.resync_tracked("main", best_state)
            else:
                workdir.joinpath("train.py").write_text(native_best_train_py)

        t_exec = time.time()
        if warm is not None:
            warm.restore(step)
            if verifier is not None:
                verifier.check_rollback(warm.warm_tree,
                                        f"step{step}-warm-restore")
            if shadow is not None:
                shadow.resync_tracked("main", warm.warm_tree)
            environment.apply_edit(workdir, d.new_train_py)
            if shadow is not None:
                shadow.record_edit("main", "train.py", d.new_train_py)
            res = warm.run_experiment(step, cfg.timeout_s)
        else:
            environment.apply_edit(workdir, d.new_train_py)
            if shadow is not None:
                shadow.record_edit("main", "train.py", d.new_train_py)
            res = _train(workdir, cfg)
        train_wall = time.time() - t_exec
        if not res.ok:
            # failed experiment (crash/timeout on the Actor's edit) is a
            # workload outcome, not a harness fault: roll back to the best
            # state, tell the Actor via history, and continue. Fail-closed
            # aborts are reserved for isolation/verifier violations.
            print(f"WARN: experiment failed at step {step} "
                  f"(trainer rc!=0 or timeout); rolled back to best",
                  file=sys.stderr)
            fail_log = cfg.results_dir / f"failed-step{step}.txt"
            fail_log.parent.mkdir(parents=True, exist_ok=True)
            fail_log.write_text(res.raw or "(no trainer output)\n")
            if avfs is not None:
                avfs.rollback(best_ckpt, step=step)
                if verifier is not None:
                    verifier.check_rollback(best_state, f"step{step}-failed")
                    shadow.resync_tracked("main", best_state)
            else:
                workdir.joinpath("train.py").write_text(native_best_train_py)
            if chain is not None:
                chain.append({"step": step, "val_bpb": None,
                              "decision": "failed",
                              "train_py_sha": hashlib.sha256(
                                  d.new_train_py.encode()).hexdigest()},
                             step=step)
            history.append({"step": step, "val_bpb": None, "decision": "failed"})
            steps.append(StepRecord(step, d.latency_s, 0.0, False, False,
                                    res.train_seconds, None, "failed",
                                    train_wall_s=round(train_wall, 2)))
            actor_tokens[0] += d.tokens_in
            actor_tokens[1] += d.tokens_out
            continue

        if avfs is not None:
            ckpt = avfs.checkpoint(f"step-{step}", step=step)
            tree = (verifier.check_main(f"step{step}-post-train")
                    if verifier is not None else None)
            if chain is not None:
                chain.append({"step": step, "val_bpb": res.val_bpb,
                              "decision": decision,
                              "train_py_sha": hashlib.sha256(
                                  d.new_train_py.encode()).hexdigest()},
                             fs_commit=ckpt, step=step, tree=tree)
            if best_bpb is None or res.val_bpb < best_bpb:
                best_bpb, best_ckpt = res.val_bpb, ckpt
                best_state = tree if tree is not None else hash_tree(workdir)
        else:
            if best_bpb is None or res.val_bpb < best_bpb:
                best_bpb = res.val_bpb
                native_best_train_py = d.new_train_py

        history.append({"step": step, "val_bpb": res.val_bpb, "decision": decision})
        steps.append(StepRecord(step, d.latency_s, 0.0, False, False,
                                res.train_seconds, res.val_bpb, decision,
                                train_wall_s=round(train_wall, 2)))
        actor_tokens[0] += d.tokens_in
        actor_tokens[1] += d.tokens_out

    wall = time.time() - t0
    verbs = avfs.verbs if avfs is not None else []
    extra = {"mode": "agentvfs" if avfs else "native",
             "actor_tokens_in": actor_tokens[0],
             "actor_tokens_out": actor_tokens[1],
             "verify_failures": verifier.failures if verifier else 0,
             "agent_state_records": len(chain.records) if chain else 0}
    extra.update(warm_restore_metrics(
        verbs, cfg.startup_baseline_s,
        warm.recovery_restores if warm is not None else 0))
    totals = compute_totals(steps, verbs, wall, extra)
    write_results(cfg.results_dir, steps, verbs, totals)
    return steps, wall
