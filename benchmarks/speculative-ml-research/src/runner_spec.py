"""k-way speculative runner (Algorithm 1, k-way pre-launch) + replay check.

Per step, the Actor and Speculator calls race in worker threads:
  1. Speculator predicts k candidate edits (best-effort). If the Actor
     returns first, the window is missed: no pre-launch at all (cancel guard —
     a slow speculator can never sit on the critical path).
  2. Each appliable candidate: branch spec-<step>-<i>, cgroup-register, apply
     edit inside the cgroup, pre-launch training there (Alg. 1 lines 11-16).
  3. Actor returns. Validation: hit_idx = first candidate whose applied text
     normalized-matches. A hit counts on keep AND discard decisions — the
     branch holds the complete next file, so it is fork-base-independent.
  4. Hit: kill sibling branches first, wait for the winner's training, verify
     branch, merge once, delete branch (on a discard-hit the merge REPLACES
     the rollback; rolling back first would merge-conflict on train.py).
     Miss: kill process groups FIRST, then delete branches; on a discard,
     roll main back to best; execute the real edit.
"""
import hashlib
import sys
import time
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from pathlib import Path

from src import environment
from src.agentvfs import CtlError
from src.llm_client import edit_diff, normalize_python
from src.metrics import (StepRecord, compute_totals, warm_restore_metrics,
                         write_results)
from src.runner_regular import _train
from src.verifier import hash_tree


def run_spec(workdir, actor, speculator, k, cfg, avfs, verifier, shadow, cg_factory,
             chain=None, warm=None, k_launch=None):
    # k_launch < k = the paper's §5 selective branch launching: predict k
    # candidates (accuracy is scored over all of them) but pre-launch only
    # the top k_launch by confidence, bounding CPU contention per window
    k_launch = k if k_launch is None else k_launch
    workdir = Path(workdir)
    history = []
    edit_log = []  # per-step actor edits as diffs, fed back to the speculator
    steps = []
    committed = []
    orphans = 0
    degraded = 0
    tok = {"a_in": 0, "a_out": 0, "s_in": 0, "s_out": 0}
    best_ckpt = avfs.checkpoint("step-base", step=-1)
    best_state = hash_tree(workdir)
    best_bpb = None
    t0 = time.time()

    with ThreadPoolExecutor(max_workers=1) as pool, \
         ThreadPoolExecutor(max_workers=2) as spool:
        for step in range(cfg.steps):
            train_py = workdir.joinpath("train.py").read_text()
            actor_future = pool.submit(actor.decide, list(history), train_py)
            spec_future = spool.submit(speculator.predict, list(history),
                                       train_py, k,
                                       edit_history=list(edit_log[-6:]))

            atts = []  # positional: atts[i] belongs to guesses[i] (None = launch failed)
            spec_latency = 0.0
            apply_failures = 0
            window_missed = False
            head_start = 0.0
            guesses = []
            try:
                # --- speculate (best-effort, cancel guard) ---
                wait({actor_future, spec_future}, return_when=FIRST_COMPLETED)
                if actor_future.done():
                    # Actor won the race: no deliberation window remains, so
                    # pre-launching would save nothing. The in-flight
                    # speculator call is abandoned (its worker thread parks
                    # until the provider returns; the result is discarded).
                    window_missed = True
                else:
                    exc = spec_future.exception()  # spec is done: no blocking
                    if exc is not None:
                        # degrade to sequential for this step, but count it —
                        # a silent swallow would make a broken speculation path
                        # indistinguishable from "speculation never wins"
                        degraded += 1
                        print(f"WARN: speculator degraded at step {step}: {exc!r}",
                              file=sys.stderr)
                    else:
                        s = spec_future.result()
                        guesses, spec_latency = s.guesses, s.latency_s
                        apply_failures = getattr(s, "apply_failures", 0)
                        tok["s_in"] += s.tokens_in
                        tok["s_out"] += s.tokens_out
                    for i, g in enumerate(guesses[:k_launch]):
                        att = _SpecAttempt(avfs, shadow, f"spec-{step}-{i}", step)
                        try:
                            avfs.branch_create(att.branch, step=step)
                            shadow.fork("main", att.branch)
                            att.cg = cg_factory(att.branch)
                            # k < 8 keeps ids unique across (step, attempt)
                            avfs.session_register(att.cg.path,
                                                  session_id=100 + step * 8 + i,
                                                  branch=att.branch)
                            environment.apply_edit(workdir, g.new_train_py, cg=att.cg)
                            shadow.record_edit(att.branch, "train.py", g.new_train_py)
                            # the log lives OFF-mount (run dir, next to mnt/):
                            # launch_training's parent-side open() routes to
                            # main, and a main-side file created between two
                            # branch forks lands in the later fork's snapshot
                            # while main's copy keeps growing — a modify/modify
                            # merge conflict on the winning branch (seen under
                            # sudo as "merge conflicts /.spec-log-0-0.txt")
                            att.log = workdir.parent / f"spec-log-{step}-{i}.txt"
                            att.proc = environment.launch_training(
                                cfg.python_bin, workdir, cfg.budget_s, att.cg,
                                att.log, argv=cfg.trainer_argv)
                            att.t0 = time.time()
                        except (CtlError, RuntimeError, OSError):
                            orphans += att.cleanup()
                            att = None
                        atts.append(att)

                # isolation postcondition: main untouched while speculation runs
                verifier.check_main(f"step{step}-isolation")

                d = actor_future.result()
                tok["a_in"] += d.tokens_in
                tok["a_out"] += d.tokens_out
                decision = "keep" if (not history or d.keep) else "discard"

                target = normalize_python(d.new_train_py)
                hit_idx = next((i for i, g in enumerate(guesses)
                                if normalize_python(g.new_train_py) == target), None)
                predicted_hit = hit_idx is not None
                att_hit = (atts[hit_idx]
                           if hit_idx is not None and hit_idx < len(atts) else None)
                # a text match is valid on DISCARD steps too: apply_edit wrote
                # the COMPLETE next file into the branch, so its tracked state
                # is independent of the fork base (train.py is the only
                # tracked file that ever changes). 18/20 real-run decisions
                # were discards — voiding them threw away every correct guess.
                prelaunched_hit = att_hit is not None

                if history and not d.keep and not prelaunched_hit:
                    # discard MISS: void the attempts, restore best on main.
                    # On a discard HIT the rollback is skipped — the merge
                    # replaces main's tracked state wholesale, and a prior
                    # rollback would modify/modify-conflict on train.py.
                    for att in atts:
                        if att is not None:
                            orphans += att.cleanup()
                    avfs.rollback(best_ckpt, step=step)
                    verifier.check_rollback(best_state, f"step{step}-discard")
                    shadow.resync_tracked("main", best_state)

                train_wall = 0.0
                if prelaunched_hit:
                    head_start = time.time() - att_hit.t0
                    # losers die first: free their CPU for the winner's finish
                    for i, att in enumerate(atts):
                        if att is not None and i != hit_idx:
                            orphans += att.cleanup()
                    res = environment.collect_training(att_hit.proc, att_hit.log,
                                                       cfg.timeout_s)
                    train_wall = time.time() - att_hit.t0
                    if not res.ok:
                        # failed experiment on the branch: drop it, tell the
                        # Actor via history, continue (fail-closed is for
                        # isolation/verifier violations, not workload outcomes)
                        print(f"WARN: experiment failed at step {step} "
                              f"(speculative trainer rc!=0 or timeout)",
                              file=sys.stderr)
                        fail_log = cfg.results_dir / f"failed-step{step}.txt"
                        fail_log.parent.mkdir(parents=True, exist_ok=True)
                        fail_log.write_text(res.raw or "(no trainer output)\n")
                        orphans += att_hit.cleanup()
                        if history and not d.keep:
                            # the discard rollback was skipped for this hit,
                            # so main still holds the DISCARDED experiment's
                            # file — a failed hit must restore best (on a
                            # keep-decision main already equals best: keep is
                            # only decided when the last step set the best)
                            avfs.rollback(best_ckpt, step=step)
                            verifier.check_rollback(best_state,
                                                    f"step{step}-failed")
                            shadow.resync_tracked("main", best_state)
                        if chain is not None:
                            chain.append({"step": step, "val_bpb": None,
                                          "decision": "failed",
                                          "train_py_sha": hashlib.sha256(
                                              d.new_train_py.encode()).hexdigest()},
                                         step=step)
                        history.append({"step": step, "val_bpb": None,
                                        "decision": "failed"})
                        edit_log.append(f"step {step} (failed):\n"
                                        + edit_diff(train_py, d.new_train_py))
                        steps.append(StepRecord(step, d.latency_s, spec_latency,
                                                predicted_hit, prelaunched_hit,
                                                res.train_seconds, None, "failed",
                                                hit_idx=hit_idx,
                                                window_missed=window_missed,
                                                spec_apply_failures=apply_failures,
                                                head_start_s=round(head_start, 2),
                                                train_wall_s=round(train_wall, 2)))
                        continue
                    verifier.check_branch(att_hit.branch, att_hit.cg,
                                          f"step{step}-pre-merge")
                    shadow.merge(att_hit.branch, into="main")
                    avfs.branch_merge(att_hit.branch, into="main", step=step)
                    verifier.check_main(f"step{step}-post-merge")
                    orphans += att_hit.cleanup()
                else:
                    for att in atts:
                        if att is not None:
                            orphans += att.cleanup()
                    t_exec = time.time()
                    if warm is not None:
                        warm.restore(step)
                        verifier.check_rollback(warm.warm_tree,
                                                f"step{step}-warm-restore")
                        shadow.resync_tracked("main", warm.warm_tree)
                        environment.apply_edit(workdir, d.new_train_py)
                        shadow.record_edit("main", "train.py", d.new_train_py)
                        res = warm.run_experiment(step, cfg.timeout_s)
                    else:
                        environment.apply_edit(workdir, d.new_train_py)
                        shadow.record_edit("main", "train.py", d.new_train_py)
                        res = _train(workdir, cfg)
                    train_wall = time.time() - t_exec
                    if not res.ok:
                        # failed experiment applied to main: roll back to the
                        # best state (same treatment as a discard), record it,
                        # continue
                        print(f"WARN: experiment failed at step {step} "
                              f"(trainer rc!=0 or timeout); rolled back to best",
                              file=sys.stderr)
                        fail_log = cfg.results_dir / f"failed-step{step}.txt"
                        fail_log.parent.mkdir(parents=True, exist_ok=True)
                        fail_log.write_text(res.raw or "(no trainer output)\n")
                        avfs.rollback(best_ckpt, step=step)
                        verifier.check_rollback(best_state, f"step{step}-failed")
                        shadow.resync_tracked("main", best_state)
                        if chain is not None:
                            chain.append({"step": step, "val_bpb": None,
                                          "decision": "failed",
                                          "train_py_sha": hashlib.sha256(
                                              d.new_train_py.encode()).hexdigest()},
                                         step=step)
                        history.append({"step": step, "val_bpb": None,
                                        "decision": "failed"})
                        edit_log.append(f"step {step} (failed):\n"
                                        + edit_diff(train_py, d.new_train_py))
                        steps.append(StepRecord(step, d.latency_s, spec_latency,
                                                predicted_hit, prelaunched_hit,
                                                res.train_seconds, None, "failed",
                                                hit_idx=hit_idx,
                                                window_missed=window_missed,
                                                spec_apply_failures=apply_failures,
                                                train_wall_s=round(train_wall, 2)))
                        continue
            except BaseException:
                # fail-closed abort must not leak the in-flight trainers,
                # cgroups, sessions, or branches — a live writer would also make
                # main.py's unmount fail EBUSY and mask the original error
                for att in atts:
                    if att is not None:
                        att.cleanup()
                actor_future.cancel()
                spec_future.cancel()
                raise

            ckpt = avfs.checkpoint(f"step-{step}", step=step)
            tree = verifier.check_main(f"step{step}-post-train")
            if best_bpb is None or res.val_bpb < best_bpb:
                best_bpb, best_ckpt = res.val_bpb, ckpt
                best_state = tree

            # on a prelaunched hit, main holds the Speculator's text (hits only
            # require normalized equality) — record what actually landed
            committed.append(guesses[hit_idx].new_train_py if prelaunched_hit
                             else d.new_train_py)
            if chain is not None:
                chain.append({"step": step, "val_bpb": res.val_bpb,
                              "decision": decision,
                              "train_py_sha": hashlib.sha256(
                                  committed[-1].encode()).hexdigest()},
                             fs_commit=ckpt, step=step, tree=tree)
            history.append({"step": step, "val_bpb": res.val_bpb, "decision": decision})
            edit_log.append(f"step {step} ({decision}):\n"
                            + edit_diff(train_py, committed[-1]))
            steps.append(StepRecord(step, d.latency_s, spec_latency, predicted_hit,
                                    prelaunched_hit, res.train_seconds, res.val_bpb,
                                    decision, hit_idx=hit_idx,
                                    window_missed=window_missed,
                                    spec_apply_failures=apply_failures,
                                    head_start_s=round(head_start, 2),
                                    train_wall_s=round(train_wall, 2)))

    wall = time.time() - t0
    verbs = avfs.verbs
    extra = {"mode": "speculative", "orphaned_branches": orphans,
             "degraded_steps": degraded,
             "actor_tokens_in": tok["a_in"], "actor_tokens_out": tok["a_out"],
             "spec_tokens_in": tok["s_in"], "spec_tokens_out": tok["s_out"],
             "verify_failures": verifier.failures,
             "agent_state_records": len(chain.records) if chain else 0}
    extra.update(warm_restore_metrics(
        verbs, cfg.startup_baseline_s,
        warm.recovery_restores if warm is not None else 0))
    totals = compute_totals(steps, verbs, wall, extra)
    write_results(cfg.results_dir, steps, avfs.verbs, totals)
    return steps, wall, committed


class _SpecAttempt:
    """Everything created for one speculative pre-launch: branch + shadow fork
    + cgroup + trainer. cg/proc/log are filled in as creation progresses, so a
    partial failure can still be cleaned with the same call.

    cleanup() kills first, then deletes, and is idempotent — the intentional
    teardown sites and the fail-closed abort handler may all call it. Returns
    1 if the branch could not be deleted (orphaned), else 0.
    """

    def __init__(self, avfs, shadow, branch, step):
        self.avfs = avfs
        self.shadow = shadow
        self.branch = branch
        self.step = step
        self.cg = None
        self.proc = None
        self.log = None
        self.t0 = None  # set at launch; head-start clock on a hit
        self._done = False

    def cleanup(self) -> int:
        if self._done:
            return 0
        self._done = True
        orphaned = 0
        if self.cg is not None:
            self.cg.kill_all()
        if self.proc is not None:
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass
        if self.cg is not None:
            try:
                self.avfs.session_unregister(self.cg.path)
            except CtlError:
                pass
            self.cg.destroy()
        try:
            self.avfs.branch_delete(self.branch, step=self.step)
        except CtlError:
            orphaned = 1
        self.shadow.drop(self.branch)
        return orphaned


def replay_check(committed_edits, base_train_py, cfg, scratch, expected_final,
                 seed_dir=None):
    """Sequential replay on a plain directory; returns (content_match, trajectory).

    expected_final is the train.py content the speculative run's main branch
    actually holds (read from the live mount) — content_match is the design's
    losslessness claim (1): the replay must reproduce it byte-for-byte.

    seed_dir: base tree to copy into the scratch first. The real train.py
    imports its siblings (`from prepare import ...`), so replaying train.py
    alone crashes on step 0; pass the run's post-overlay seed directory.
    """
    scratch = Path(scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    if seed_dir is not None:
        environment.seed_base_tree(scratch, seed_dir)
    (scratch / "train.py").write_text(base_train_py)
    traj = []
    for content in committed_edits:
        (scratch / "train.py").write_text(content)
        res = _train(scratch, cfg)
        if not res.ok:
            return False, traj
        traj.append(res.val_bpb)
    return (scratch / "train.py").read_text() == expected_final, traj
