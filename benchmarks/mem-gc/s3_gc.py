#!/usr/bin/env python3
"""S3: store growth, orphan share, GC reclamation, retention semantics
(spec curve 3 / gate G3). After-build only — gc.* commands do not exist on
the before build; before rows carry store_bytes growth only.

Deterministic-by-construction targets (gate G3, verified at run time):
  * Rollbacks are confined to the FIRST HALF of steps (step < steps//2), so
    the second half builds one long REACHABLE first-parent chain.
  * `auto-{gone}` (gone = steps - 8, nudged off a multiple of 10) sits on that
    final chain, OUTSIDE keep_last=3 and UNPINNED -> its DATA is COMPACTED
    (metadata kept) -> `rollback auto-{gone}` fails with the
    "checkpoint compacted by retention policy" error (Task 9).
  * Every retained target (HEAD, the keep_last window, and every keep_label
    pin) is rolled back to in newest-to-oldest order and must succeed.
  * The pre-GC sleep ages the deterministic workload, so the post-GC dry run
    must report zero residual orphan bytes.
"""
import json, sys, time
from pathlib import Path
from driver import (DaemonProc, du_bytes, gen_tree, write_csv, workdir,
                    wait_for_fold)


def main():
    bin_path, label, out_dir = sys.argv[1], sys.argv[2], Path(sys.argv[3])
    steps = int(sys.argv[4]) if len(sys.argv) > 4 else 40

    wd = workdir(f"s3-{label}")
    expected_files = 5_000
    generated = gen_tree(wd / "src", expected_files, 500)
    if generated != expected_files:
        raise RuntimeError(
            f"generated {generated} source files; expected {expected_files}")
    d = DaemonProc(bin_path, wd / "src", wd / "mnt", wd / "store", wd / "ctl.sock")
    rows, summary = [], {}
    checkpoints = []
    try:
        d.start()
        d.force_full_ingest(generated)
        # GC + hygiene need main folded/authoritative: GcRunner's mark phase
        # walks the live root set (WorkingTree entries across branches), and
        # the deterministic compacted/retained targets assume a stable commit
        # graph built on top of a folded base. The daemon auto-folds via its
        # background source walk; wait for it before checkpoint/churn. No-op on
        # the before build (stats.memory absent).
        wait_for_fold(d, label)
        mnt = Path(d.mnt)
        r = d.ctl("checkpoint keep-base"); assert r.get("ok"), r
        checkpoints.append((-1, "keep-base", r["commit"]))
        rollback_half = steps // 2
        for step in range(steps):
            # churn: overwrite a rotating slice of files (creates orphan blobs)
            for i in range(50):
                (mnt / f"dir{i % 500:05d}").mkdir(exist_ok=True)
                (mnt / f"dir{i % 500:05d}" / "hot.txt").write_bytes(
                    b"v" * 256 + f"{step}-{i}".encode())
            label_arg = f"keep-{step}" if step % 10 == 0 else f"auto-{step}"
            r = d.ctl(f"checkpoint {label_arg}"); assert r.get("ok"), r
            checkpoints.append((step, label_arg, r["commit"]))
            # Rollbacks ORPHAN everything after the rollback point (those
            # commits become side chains unreachable from the final head).
            # Confine rollbacks to the first half so the second half builds
            # one long REACHABLE chain whose middle gets COMPACTED (metadata
            # kept, data swept) — both garbage classes are then present
            # deterministically.
            if step % 7 == 3 and step < rollback_half:
                r = d.ctl("rollback keep-base"); assert r.get("ok"), r
            rows.append([label, step, "checkpoint", du_bytes(d.store), "", ""])
        gc_ok = True
        if label == "after":
            assert steps >= 24, "S3 needs >= 24 steps for deterministic targets"
            # Age every object past the GC age fence before sweeping. The sweep
            # protects objects with mtime + 2 >= mark_start (Task 8: a concurrent
            # FUSE publisher may have written a fresh, not-yet-referenced
            # object). On a fast run the churn loop finishes in a few seconds,
            # so a compacted commit's tree object can still be fence-fresh and
            # survive the sweep -> its rollback would wrongly SUCCEED, breaking
            # the compacted_rollback_error target. Sleeping > 2s makes the
            # deterministic-by-construction compacted/retained targets hold at
            # ANY scale (the paper-scale run is naturally slow, but this makes
            # the smoke run deterministic too).
            time.sleep(3)
            # Pins: keep-base plus every keep-N label on the reachable final
            # chain (N >= rollback_half). Compacted probe: an auto- commit on
            # the final chain, outside keep_last=3 and unpinned.
            pins = ["keep-base"] + [name for step, name, _commit in checkpoints
                                    if step >= rollback_half and
                                    name.startswith("keep-")]
            gone = steps - 8
            if gone % 10 == 0: gone -= 1
            pin_args = " ".join(f"keep_label={x}" for x in pins)
            dry = d.ctl("gc.run dry_run=1")
            rows.append([label, steps, "gc-dry", du_bytes(d.store),
                         dry.get("swept_bytes", 0), ""])
            store_before_gc = du_bytes(d.store)
            real = d.ctl(f"gc.run keep_last=3 {pin_args}")
            store_after_gc = du_bytes(d.store)
            rows.append([label, steps + 1, "gc-run", store_after_gc, "",
                         real.get("swept_bytes", 0)])
            ver = d.ctl(f"gc.verify keep_last=3 {pin_args}")
            redry = d.ctl(f"gc.run dry_run=1 keep_last=3 {pin_args}")
            # Probe the COMPACTED target FIRST, while the branch head is still
            # the final head (auto-{gone} is an ANCESTOR of head, so it
            # resolves; its data was swept -> "compacted by retention policy").
            # A successful rollback moves the head, so probing the retained
            # target first would strand auto-{gone} as a non-ancestor of the
            # new head and the probe would fail with "target commit not found"
            # instead of the compacted error.
            rb_gone = d.ctl(f"rollback auto-{gone}")

            # Roll back retained checkpoints newest-to-oldest. Each target is
            # therefore an ancestor of the preceding target, so one branch can
            # prove every retained data set without probe branches changing the
            # GC root set.
            retained = {}
            by_label = {name: (step, commit)
                        for step, name, commit in checkpoints}

            def retain(name, category):
                target = retained.setdefault(name, {
                    "label": name,
                    "step": by_label[name][0],
                    "commit": by_label[name][1],
                    "categories": [],
                })
                if category not in target["categories"]:
                    target["categories"].append(category)

            head_label = checkpoints[-1][1]
            retain(head_label, "head")
            for _step, name, _commit in checkpoints[-3:]:
                retain(name, "keep_last")
            for name in pins:
                retain(name, "pin")

            rollback_results = []
            for target in sorted(retained.values(),
                                 key=lambda item: item["step"], reverse=True):
                response = d.ctl(f"rollback {target['label']}")
                rollback_results.append({
                    **target,
                    "ok": response.get("ok") is True,
                    "error": response.get("error", ""),
                })

            retained_ok = bool(rollback_results) and all(
                result["ok"] for result in rollback_results)
            du_reclaimed = store_before_gc - store_after_gc
            summary = {
                "dry_run_ok": dry.get("ok") is True,
                "gc_run_ok": real.get("ok") is True,
                "sweep_errors": real.get("sweep_errors", -1),
                "reclaimed_bytes": real.get("swept_bytes", 0),
                "store_bytes_before_gc": store_before_gc,
                "store_bytes_after_gc": store_after_gc,
                "du_reclaimed_bytes": du_reclaimed,
                "residual_scan_ok": redry.get("ok") is True,
                "residual_sweep_errors": redry.get("sweep_errors", -1),
                "residual_orphan_bytes": redry.get("swept_bytes", -1),
                "verify_ok": (ver.get("ok") is True and
                              ver.get("missing_objects", -1) == 0),
                "verify_missing_objects": ver.get("missing_objects", -1),
                "retained_rollback_ok": retained_ok,
                "retained_rollback_passed": sum(
                    1 for result in rollback_results if result["ok"]),
                "retained_rollback_total": len(rollback_results),
                "retained_rollbacks": rollback_results,
                "compacted_rollback_error":
                    (not rb_gone.get("ok")) and
                    "compacted by retention policy" in rb_gone.get("error", ""),
                "gc_duration_ms": real.get("duration_ms", 0),
                "compacted_commits": real.get("compacted_commits", 0),
            }
            gc_ok = (summary["dry_run_ok"] and summary["gc_run_ok"] and
                     summary["sweep_errors"] == 0 and
                     summary["reclaimed_bytes"] > 0 and
                     summary["du_reclaimed_bytes"] > 0 and
                     summary["residual_scan_ok"] and
                     summary["residual_sweep_errors"] == 0 and
                     summary["residual_orphan_bytes"] == 0 and
                     summary["verify_ok"] and
                     summary["retained_rollback_ok"] and
                     summary["compacted_rollback_error"])
            (out_dir / "s3_summary.json").write_text(json.dumps(summary, indent=1))
            print(f"[s3:{label}] gc: reclaimed={summary['reclaimed_bytes']}B "
                  f"compacted_commits={summary['compacted_commits']} "
                  f"duration={summary['gc_duration_ms']}ms "
                  f"du_reclaimed={summary['du_reclaimed_bytes']}B "
                  f"sweep_errors={summary['sweep_errors']} "
                  f"residual={summary['residual_orphan_bytes']}B "
                  f"verify={summary['verify_ok']} "
                  f"retained_rb={summary['retained_rollback_passed']}/"
                  f"{summary['retained_rollback_total']} "
                  f"compacted_err={summary['compacted_rollback_error']}",
                  flush=True)
    finally:
        d.stop()
    write_csv(out_dir / f"s3-{label}.csv",
              ["bin_label", "step", "event", "store_bytes",
               "orphan_bytes", "swept_bytes"], rows)
    if label == "after" and not gc_ok:
        print(f"[s3] summary failures: {summary}", file=sys.stderr); sys.exit(1)


if __name__ == "__main__":
    main()
