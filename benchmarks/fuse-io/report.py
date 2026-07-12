#!/usr/bin/env python3
import argparse
import json
import math
import pathlib
import statistics

PRIMARY_SYNTHETIC_CASES = (
    "stat-existing", "open-close", "tree-walk", "read-small",
)
GUARDRAIL_CASES = ("lookup-missing", "seq-read", "random-read",
                   "create-write-close", "create-unlink")
COMPARISON_KEYS = ("kernel", "libfuse", "cpu", "fixture_files",
                   "fixture_small_file_bytes", "fixture_large_file_bytes",
                   "repetitions", "iteration_divisor")


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        raise ValueError("percentile requires at least one sample")
    rank = max(1, math.ceil(fraction * len(ordered)))
    return ordered[rank - 1]


def summarize(rows):
    groups = {}
    for row in rows:
        groups.setdefault((row["target"], row["case"]), []).append(row)
    result = {}
    for key, samples in groups.items():
        elapsed = [sample["elapsed_ns"] for sample in samples]
        rates = [sample["ops_per_second"] for sample in samples]
        result[key] = {
            "runs": len(samples),
            "median_elapsed_ns": statistics.median(elapsed),
            "p95_elapsed_ns": percentile(elapsed, 0.95),
            "median_ops_per_second": statistics.median(rates),
        }
    return result


def regressions(baseline, optimized):
    bad = []
    for case in PRIMARY_SYNTHETIC_CASES + ("git-status",) + GUARDRAIL_CASES:
        key = ("fuse", case)
        if key not in baseline or key not in optimized:
            continue
        ratio = (optimized[key]["median_ops_per_second"] /
                 baseline[key]["median_ops_per_second"])
        if ratio < 0.90:
            bad.append((case, round(ratio, 4)))
    return bad


def routed_ratios(summary):
    ratios = []
    for target, case in sorted(summary):
        if target != "fuse-routed" or ("fuse", case) not in summary:
            continue
        ratios.append((case,
                       summary[(target, case)]["median_ops_per_second"] /
                       summary[("fuse", case)]["median_ops_per_second"]))
    return ratios


def routed_regressions(summary):
    return [(case, round(ratio, 4))
            for case, ratio in routed_ratios(summary) if ratio < 0.90]


def acceptance(baseline, optimized):
    ratios = []
    for case in PRIMARY_SYNTHETIC_CASES:
        key = ("fuse", case)
        ratios.append(optimized[key]["median_ops_per_second"] /
                      baseline[key]["median_ops_per_second"])
    throughput_ratio = math.prod(ratios) ** (1.0 / len(ratios))
    git_ratio = (optimized[("fuse", "git-status")]["median_elapsed_ns"] /
                 baseline[("fuse", "git-status")]["median_elapsed_ns"])
    bad = regressions(baseline, optimized)
    routed_bad = routed_regressions(optimized)
    return {
        "throughput_ratio": throughput_ratio,
        "throughput_pass": throughput_ratio >= 2.0,
        "git_status_elapsed_ratio": git_ratio,
        "git_status_pass": git_ratio <= 0.80,
        "regressions": bad,
        "routed_regressions": routed_bad,
        "guardrails_pass": not bad and not routed_bad,
    }


def verdict(comparison):
    floor = comparison.get("floor_reference")
    throughput_ok = comparison["throughput_pass"] or bool(floor)
    passed = all((throughput_ok, comparison["git_status_pass"],
                  comparison["guardrails_pass"]))
    if passed and not comparison["throughput_pass"]:
        return True, "PASS (accepted throughput floor)"
    return passed, "PASS" if passed else "FAIL"


def load_results(path):
    with open(path, encoding="utf-8") as stream:
        payload = json.load(stream)
    return payload["metadata"], summarize(payload["samples"])


def require_comparable(baseline, optimized):
    mismatches = [key for key in COMPARISON_KEYS
                  if baseline.get(key) != optimized.get(key)]
    if mismatches:
        raise ValueError("benchmark metadata differs: " + ", ".join(mismatches))


def write_summary(path, metadata, summary, comparison=None):
    lines = ["# Linux FUSE I/O Benchmark", "", "## Host", ""]
    for key in sorted(metadata):
        lines.append(f"- `{key}`: `{metadata[key]}`")
    lines += ["", "## Results", "",
              "| Target | Case | Runs | Median ops/s | Median ms | p95 ms |",
              "|---|---|---:|---:|---:|---:|"]
    for (target, case), row in sorted(summary.items()):
        lines.append(
            f"| {target} | {case} | {row['runs']} | "
            f"{row['median_ops_per_second']:.2f} | "
            f"{row['median_elapsed_ns'] / 1e6:.3f} | "
            f"{row['p95_elapsed_ns'] / 1e6:.3f} |")
    routed = routed_ratios(summary)
    if routed:
        lines += ["", "## Routed Guardrail (routed/main ops ratio)", ""]
        for case, ratio in routed:
            state = "PASS" if ratio >= 0.90 else "FAIL"
            lines.append(f"- `{case}`: `{ratio:.3f}x` — {state}")
    if comparison is not None:
        passed, label = verdict(comparison)
        floor = comparison.get("floor_reference")
        throughput_line = (
            f"- Primary throughput ratio: `{comparison['throughput_ratio']:.3f}x`")
        if floor and not comparison["throughput_pass"]:
            throughput_line += (
                f" (< 2.0x gate; structural floor accepted per {floor})")
        lines += ["", "## Acceptance", "",
                  throughput_line,
                  f"- Repository scan elapsed ratio: `{comparison['git_status_elapsed_ratio']:.3f}`",
                  f"- Regressions over 10%: `{comparison['regressions']}`"]
        if routed:
            lines.append(
                f"- Routed-branch regressions over 10%: `{comparison['routed_regressions']}`")
        lines.append(f"- Overall: `{label}`")
    pathlib.Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    one = sub.add_parser("summarize")
    one.add_argument("results")
    one.add_argument("--output", required=True)
    compare = sub.add_parser("compare")
    compare.add_argument("baseline")
    compare.add_argument("optimized")
    compare.add_argument("--output", required=True)
    compare.add_argument("--require", action="store_true")
    compare.add_argument(
        "--accept-throughput-floor", metavar="REFERENCE",
        help="treat a sub-2.0x primary throughput ratio as an "
             "operator-accepted structural floor; REFERENCE names the plan "
             "section recording that decision")
    args = parser.parse_args()
    if args.command == "summarize":
        metadata, summary = load_results(args.results)
        write_summary(args.output, metadata, summary)
        return 0
    base_meta, baseline = load_results(args.baseline)
    new_meta, optimized = load_results(args.optimized)
    require_comparable(base_meta, new_meta)
    result = acceptance(baseline, optimized)
    if args.accept_throughput_floor:
        result["floor_reference"] = args.accept_throughput_floor
    merged = dict(new_meta,
                  baseline_label=base_meta.get("label"),
                  baseline_git_commit=base_meta.get("git_commit"))
    write_summary(args.output, merged, optimized, result)
    passed, _ = verdict(result)
    return 0 if passed or not args.require else 1


if __name__ == "__main__":
    raise SystemExit(main())
