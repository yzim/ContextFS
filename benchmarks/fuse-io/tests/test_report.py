import importlib.util
import pathlib
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("fuse_io_report", ROOT / "report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


class ReportTests(unittest.TestCase):
    def test_percentile_uses_nearest_rank(self):
        self.assertEqual(report.percentile([1, 2, 3, 4, 5], 0.95), 5)

    def test_summarize_groups_target_and_case(self):
        rows = [
            {"target": "fuse", "case": "stat-existing", "elapsed_ns": n,
             "operations": 100, "ops_per_second": rate}
            for n, rate in [(100, 10.0), (90, 12.0), (110, 8.0)]
        ]
        got = report.summarize(rows)[("fuse", "stat-existing")]
        self.assertEqual(got["runs"], 3)
        self.assertEqual(got["median_elapsed_ns"], 100)
        self.assertEqual(got["median_ops_per_second"], 10.0)

    def test_acceptance_uses_blob_path_cases_and_git_status(self):
        cases = report.PRIMARY_SYNTHETIC_CASES
        baseline = {("fuse", c): {"median_ops_per_second": 100.0,
                                   "median_elapsed_ns": 1000}
                    for c in cases}
        optimized = {("fuse", c): {"median_ops_per_second": 210.0,
                                    "median_elapsed_ns": 450}
                     for c in cases}
        baseline[("fuse", "git-status")] = {
            "median_ops_per_second": 1.0, "median_elapsed_ns": 1_000_000}
        optimized[("fuse", "git-status")] = {
            "median_ops_per_second": 1.3, "median_elapsed_ns": 750_000}
        result = report.acceptance(baseline, optimized)
        self.assertTrue(result["throughput_pass"])
        self.assertTrue(result["git_status_pass"])
        self.assertAlmostEqual(result["throughput_ratio"], 2.1)

    def test_acceptance_rejects_guardrail_regression(self):
        base = {("fuse", "seq-read"): {"median_ops_per_second": 100.0,
                                        "median_elapsed_ns": 100}}
        new = {("fuse", "seq-read"): {"median_ops_per_second": 89.0,
                                       "median_elapsed_ns": 112}}
        self.assertEqual(report.regressions(base, new), [("seq-read", 0.89)])

    def test_routed_regressions_gate_at_ninety_percent(self):
        summary = {
            ("fuse", "stat-existing"): {"median_ops_per_second": 100.0},
            ("fuse-routed", "stat-existing"): {"median_ops_per_second": 95.0},
            ("fuse", "read-small"): {"median_ops_per_second": 100.0},
            ("fuse-routed", "read-small"): {"median_ops_per_second": 89.0},
        }
        self.assertEqual(report.routed_regressions(summary),
                         [("read-small", 0.89)])

    def test_acceptance_folds_routed_rows_into_guardrails(self):
        cases = report.PRIMARY_SYNTHETIC_CASES
        baseline = {("fuse", c): {"median_ops_per_second": 100.0,
                                   "median_elapsed_ns": 1000}
                    for c in cases}
        optimized = {("fuse", c): {"median_ops_per_second": 210.0,
                                    "median_elapsed_ns": 450}
                     for c in cases}
        for summary in (baseline, optimized):
            summary[("fuse", "git-status")] = {
                "median_ops_per_second": 1.0, "median_elapsed_ns": 1_000_000}
        optimized[("fuse-routed", "stat-existing")] = {
            "median_ops_per_second": 180.0, "median_elapsed_ns": 500}
        result = report.acceptance(baseline, optimized)
        self.assertEqual(result["routed_regressions"],
                         [("stat-existing", round(180.0 / 210.0, 4))])
        self.assertFalse(result["guardrails_pass"])

    def test_verdict_accepts_operator_approved_floor(self):
        comparison = {"throughput_pass": False, "git_status_pass": True,
                      "guardrails_pass": True}
        self.assertEqual(report.verdict(comparison), (False, "FAIL"))
        comparison["floor_reference"] = "plan.md 'Phase 1 gate verdict'"
        self.assertEqual(report.verdict(comparison),
                         (True, "PASS (accepted throughput floor)"))

    def test_verdict_floor_does_not_mask_other_gates(self):
        comparison = {"throughput_pass": False, "git_status_pass": True,
                      "guardrails_pass": False,
                      "floor_reference": "plan.md"}
        self.assertEqual(report.verdict(comparison), (False, "FAIL"))

    def test_compare_rejects_different_hosts(self):
        baseline = {"kernel": "6.17", "libfuse": "3.14.0", "cpu": "cpu-a",
                    "fixture_files": 10000, "fixture_small_file_bytes": 4096,
                    "fixture_large_file_bytes": 1073741824}
        optimized = dict(baseline, kernel="6.18")
        with self.assertRaisesRegex(ValueError, "kernel"):
            report.require_comparable(baseline, optimized)


if __name__ == "__main__":
    unittest.main()
