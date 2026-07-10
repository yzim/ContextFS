"""CSV + summary output in the agent-sim line-oriented style."""
import csv
from dataclasses import dataclass, asdict
from pathlib import Path


@dataclass
class StepRecord:
    step: int
    actor_latency_s: float
    spec_latency_s: float
    predicted_hit: bool
    prelaunched_hit: bool
    train_time_s: float
    val_bpb: float | None
    decision: str
    hit_idx: int | None = None        # index into the launched guesses, or None
    window_missed: bool = False       # actor returned before the speculator
    spec_apply_failures: int = 0      # candidates whose anchors didn't apply
    head_start_s: float = 0.0         # training wall already banked at validation
    train_wall_s: float = 0.0         # subprocess wall (launch->collect):
                                      # startup/data-load/eval/log I/O included,
                                      # unlike the self-reported train_time_s


def write_results(results_dir: Path, steps: list, verbs: list, totals: dict) -> None:
    results_dir = Path(results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)
    with open(results_dir / "perstep.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(asdict(steps[0]).keys()) if steps else
                           ["step"])
        w.writeheader()
        for s in steps:
            w.writerow(asdict(s))
    with open(results_dir / "verbs.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["verb", "step", "latency_us", "status", "store_bytes_after"])
        for r in verbs:
            w.writerow([r.verb, r.step, r.latency_us, r.status, r.store_bytes_after])
    with open(results_dir / "totals.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(list(totals.keys()))
        w.writerow(list(totals.values()))
    lines = ["# Run summary", ""]
    lines += [f"- **{k}**: {v}" for k, v in totals.items()]
    (results_dir / "summary.md").write_text("\n".join(lines) + "\n")


def warm_restore_metrics(verbs: list, startup_baseline_s: float,
                         recovery_restores: int = 0) -> dict:
    """Restore count/latency plus startup time saved vs fresh launches.
    Recovery restores (timeout cleanup) replace no fresh launch, so they
    count in `restores` but not in the savings."""
    restores = [r for r in verbs if r.verb == "runtime_restore"]
    mean_ms = (sum(r.latency_us for r in restores) / len(restores) / 1000.0
               if restores else 0.0)
    productive = max(0, len(restores) - recovery_restores)
    return {
        "restores": len(restores),
        "mean_restore_ms": round(mean_ms, 1),
        "startup_saved_s": round(
            max(0.0, startup_baseline_s - mean_ms / 1000.0) * productive, 1),
    }


def compute_totals(steps: list, verbs: list, wall_s: float, extra: dict | None = None) -> dict:
    hits = sum(1 for s in steps if s.predicted_hit)
    pre = sum(1 for s in steps if s.prelaunched_hit)
    t = {
        "wall_s": round(wall_s, 2),
        "n_steps": len(steps),
        "failed_steps": sum(1 for s in steps if s.decision == "failed"),
        "predicted_hits": hits,
        "prelaunched_hits": pre,
        "accuracy_any": round(hits / len(steps), 4) if steps else 0.0,
        "peak_store_bytes": max((r.store_bytes_after for r in verbs), default=0),
        "verb_failures": sum(1 for r in verbs if r.status != "ok"),
        "windows_missed": sum(1 for s in steps if s.window_missed),
        "spec_apply_failures": sum(s.spec_apply_failures for s in steps),
        "train_wall_total_s": round(sum(s.train_wall_s for s in steps), 1),
    }
    t.update(extra or {})
    return t
