#!/usr/bin/env python3
"""Evaluate mem-gc gates. SKIP is a failure unless --allow-skip is given."""
import argparse, csv, json, math, sys
from pathlib import Path

def load(path):
    if not Path(path).exists(): return []
    with open(path) as f: return list(csv.DictReader(f))

def rss_at(rows, label, k):
    for r in rows:
        if r["bin_label"] == label and int(r["k"]) == k:
            return int(r["rss_kb"])
    return None

def gate_g1(rows, out):
    a1, a32 = rss_at(rows, "after", 1), rss_at(rows, "after", 32)
    if a1 is None or a32 is None:
        out.append("G1: SKIP (missing after rows)"); return None
    marginal = (a32 - a1) / 31.0
    ok = a1 > 0 and a32 <= 1.25 * a1 and marginal <= 0.05 * a1
    b1, b32 = rss_at(rows, "before", 1), rss_at(rows, "before", 32)
    out.append(f"G1: {'PASS' if ok else 'FAIL'} — after k=32 {a32}kB vs "
               f"1.25x k=1 {a1}kB cap {1.25*a1:.0f}kB; marginal/branch "
               f"{marginal:.0f}kB (cap {0.05*a1:.0f}kB)")
    if b1 is not None and b32 is not None:
        out.append(f"    before-build documented: k=1 {b1}kB -> k=32 {b32}kB "
                   f"({(b32-b1)/31:.0f}kB/branch)")
    return ok

def gate_g2(rows, out):
    """Resident entries stay bounded through created-then-deleted churn.

    G2 checks the LAST after-build sample: resident (base + delta) must be
    <= 1.02x base. On the after build, tombstone hygiene erases each
    created-then-deleted path from the delta (base_authoritative_), so the
    delta returns to ~empty after every churn cycle and resident stays flat.
    On the before build, tombstones accumulate forever (documented, not gated).

    Only after-build rows with populated base_entries count; before-build rows
    have empty base_entries (stats.memory absent) and are skipped.
    """
    after = [r for r in rows if r["bin_label"] == "after" and r["base_entries"]]
    if not after:
        out.append("G2: SKIP (missing after rows)"); return None
    last = after[-1]
    base = int(last["base_entries"])
    resident = base + int(last["delta_entries"])
    cap = 1.02 * base
    ok = base > 0 and resident <= cap
    out.append(f"G2: {'PASS' if ok else 'FAIL'} — resident {resident} vs "
               f"1.02x base {base} cap {cap:.0f} "
               f"(delta={last['delta_entries']}, "
               f"tombstones={last['delta_tombstones']})")
    cps = [float(r["checkpoint_ms"]) for r in after if r["checkpoint_ms"]]
    if len(cps) >= 2:
        out.append(f"    checkpoint latency after-build: first {cps[0]}ms, "
                   f"median {sorted(cps)[len(cps)//2]}ms")
    return ok

def gate_g3(rd, out):
    """GC reclaims unreachable objects AND retention semantics are correct.

    This slept scenario is deterministic: the real run must succeed without
    sweep errors, reduce actual store size, and leave no aged garbage. Verify,
    every retained rollback, and the compacted rollback error must also pass.
    """
    p = rd / "s3_summary.json"
    if not p.exists():
        out.append("G3: SKIP (no s3_summary.json)"); return None
    s = json.loads(p.read_text())
    rollback_total = s.get("retained_rollback_total", 0)
    rollback_passed = s.get("retained_rollback_passed", 0)
    rollback_results = s.get("retained_rollbacks", [])
    rollback_categories = {
        category
        for result in rollback_results if isinstance(result, dict)
        for category in result.get("categories", [])
    } if isinstance(rollback_results, list) else set()
    rollback_details_ok = (
        isinstance(rollback_results, list) and rollback_total > 0 and
        len(rollback_results) == rollback_total and
        sum(result.get("ok") is True for result in rollback_results
            if isinstance(result, dict)) == rollback_passed == rollback_total and
        {"head", "keep_last", "pin"}.issubset(rollback_categories))
    ok = (s.get("dry_run_ok") is True and
          s.get("gc_run_ok") is True and
          s.get("sweep_errors") == 0 and
          s.get("reclaimed_bytes", 0) > 0 and
          s.get("du_reclaimed_bytes", 0) > 0 and
          s.get("residual_scan_ok") is True and
          s.get("residual_sweep_errors") == 0 and
          s.get("residual_orphan_bytes") == 0 and
          s.get("verify_ok") is True and
          s.get("verify_missing_objects") == 0 and
          s.get("retained_rollback_ok") is True and
          rollback_details_ok and
          s.get("compacted_rollback_error") is True)
    out.append(f"G3: {'PASS' if ok else 'FAIL'} — gc.run={s.get('gc_run_ok')}, "
               f"sweep_errors={s.get('sweep_errors')}, "
               f"reported-reclaimed={s.get('reclaimed_bytes')}B, "
               f"du-reclaimed={s.get('du_reclaimed_bytes')}B")
    out.append(f"    residual-aged-orphans={s.get('residual_orphan_bytes')}B, "
               f"verify={s.get('verify_ok')}, retained-rollbacks="
               f"{rollback_passed}/{rollback_total}, "
               f"compacted-error={s.get('compacted_rollback_error')}")
    out.append(f"    gc.run duration {s.get('gc_duration_ms')}ms "
               f"(proxy for the publication-gated mark+sweep pause), "
               f"compacted_commits={s.get('compacted_commits')}")
    return ok

def gate_g4(ratio, out):
    """No FUSE regression: after/before stat-existing ops/s >= 0.97.

    The ratio is computed externally from benchmarks/fuse-io single-client
    stat-existing median ops/s for the two binaries and passed via --g4-ratio.
    SKIPs when no ratio is supplied. Main treats SKIP as failure unless the
    caller explicitly selected --allow-skip.
    """
    if ratio is None:
        out.append("G4: SKIP (run benchmarks/fuse-io for both bins, pass --g4-ratio)")
        return None
    ok = math.isfinite(ratio) and ratio > 0 and ratio >= 0.97
    out.append(f"G4: {'PASS' if ok else 'FAIL'} — fuse-io stat-existing "
               f"after/before {ratio:.3f} (finite, positive, floor 0.97)")
    return ok


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_dir", type=Path)
    parser.add_argument("--g4-ratio", type=float)
    parser.add_argument(
        "--allow-skip", action="store_true",
        help="allow missing gates for reduced smoke runs (FAIL still exits non-zero)")
    return parser.parse_args()

def main():
    args = parse_args()
    rd = args.results_dir
    out = []
    gates = [
        gate_g1(load(rd / "s1.csv"), out),
        gate_g2(load(rd / "s2.csv"), out),
        gate_g3(rd, out),
        gate_g4(args.g4_ratio, out),
    ]
    failed = any(gate is False for gate in gates) if args.allow_skip else any(
        gate is not True for gate in gates)
    (rd / "report.md").write_text(
        "# mem-gc gate report\n\n" + "\n".join(f"- {l}" for l in out) + "\n")
    print("\n".join(out))
    sys.exit(1 if failed else 0)

if __name__ == "__main__":
    main()
