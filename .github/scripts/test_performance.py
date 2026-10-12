"""Performance workflow tests use only owned files and fake Git output."""

import argparse
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import performance


BASELINE = "a" * 40
CANDIDATE = "b" * 40


class PerformanceWorkflowTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="Task Manager Performance ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def test_only_exact_sha_references_can_reach_checkout_output(self):
        output = self.root / "github-output"
        output.write_text("", encoding="utf-8")
        with patch.dict(os.environ, GITHUB_OUTPUT=str(output), TASKMGR_BASELINE_SHA="A" * 40), \
                contextlib.redirect_stdout(io.StringIO()):
            performance.validate_baseline()
        self.assertEqual(output.read_text(encoding="utf-8"), f"sha={BASELINE}\n")
        for value in (None, "main", "refs/heads/main", "a" * 39, "z" * 40, BASELINE + "\nvalue=bad", "$(command)"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                performance.exact_sha(value)

    def test_source_record_checks_both_checkout_identities_and_invalidates_old_success(self):
        arguments = argparse.Namespace(baseline_source=self.root / "baseline", output_dir=self.root)
        for path in (self.root / "reports/comparison.json", self.root / "screenshots/screenshots.json",
                     self.root / "network-smoke/network-smoke.json", self.root / "network-smoke/result.json"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('{"stale":true}', encoding="utf-8")
        with patch.dict(os.environ, TASKMGR_BASELINE_SHA=BASELINE, GITHUB_SHA=CANDIDATE, GITHUB_EVENT_NAME="pull_request"), \
                patch.object(performance, "git_sha", side_effect=[BASELINE, CANDIDATE]):
            performance.record(arguments)
        sources = json.loads((self.root / "sources.json").read_text(encoding="utf-8"))
        self.assertEqual(sources["baselineSha"], BASELINE)
        self.assertEqual(sources["candidateSha"], CANDIDATE)
        self.assertEqual(sources["candidateKind"], "pull-request merge")
        self.assertFalse((self.root / "reports/comparison.json").exists())
        self.assertFalse((self.root / "screenshots/screenshots.json").exists())
        self.assertFalse((self.root / "network-smoke/result.json").exists())
        self.assertFalse((self.root / "network-smoke/network-smoke.json").exists())
        with patch.dict(os.environ, TASKMGR_BASELINE_SHA=BASELINE, GITHUB_SHA=CANDIDATE), \
                patch.object(performance, "git_sha", side_effect=[CANDIDATE, CANDIDATE]):
            with self.assertRaisesRegex(ValueError, "Baseline checkout differs"):
                performance.record(arguments)
        self.assertFalse((self.root / "sources.json").exists())

    def write_evidence(self):
        (self.root / "reports").mkdir(exist_ok=True)
        (self.root / "sources.json").write_text(json.dumps({"baselineSha": BASELINE, "candidateSha": CANDIDATE}), encoding="utf-8")
        report = {"repetitions": 3, "summary": [
            {"scenario": scenario, "metric": metric, "baselineMedian": 10, "candidateMedian": 8,
             "baselineMin": 5, "baselineMax": 15, "candidateMin": 4, "candidateMax": 12}
            for scenario, metric in performance.KEY_METRICS],
            "productionExecutables": [
                {"variant": variant, "bytes": 1234567, "sha256": "a" * 64}
                for variant in ("baseline", "candidate")]}
        path = self.root / "reports/comparison.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        return report, path

    def test_summary_includes_exact_sources_ranges_sizes_and_measurement_limits(self):
        self.write_evidence()
        result = performance.render_summary(self.root)
        self.assertIn(BASELINE, result)
        self.assertIn(CANDIDATE, result)
        self.assertIn("10 [5, 15]", result)
        self.assertIn("8 [4, 12]", result)
        self.assertIn("-20.00%", result)
        self.assertIn("1,234,567", result)
        self.assertIn("one logical core as 100%", result)
        self.assertIn("Screenshot capture did not complete", result)

    def test_incomplete_comparison_never_claims_a_performance_result(self):
        result = performance.render_summary(self.root)
        self.assertIn("comparison did not complete", result)
        self.assertNotIn("| Scenario", result)

    def test_missing_duplicate_and_nonfinite_metrics_are_rejected(self):
        for defect in ("missing", "duplicate", "nonfinite"):
            with self.subTest(defect=defect):
                report, path = self.write_evidence()
                if defect == "missing":
                    report["summary"].pop()
                elif defect == "duplicate":
                    report["summary"].append(report["summary"][0])
                else:
                    report["summary"][0]["baselineMedian"] = float("nan")
                path.write_text(json.dumps(report), encoding="utf-8")
                with self.assertRaises(ValueError):
                    performance.render_summary(self.root)

    def test_network_summary_distinguishes_verified_skipped_failed_and_absent(self):
        self.assertIn("remains unverified", performance.network_summary(self.root))
        directory = self.root / "network-smoke"
        directory.mkdir()
        report = {"passed": True, "skipped": False, "reason": "", "traceStatus": 0, "socketStatus": 0,
                  "sentBytes": 32768, "receivedBytes": 32768, "sentDatagrams": 32, "receivedDatagrams": 32,
                  "samples": 2, "peakBytesPerSecond": 16000, "deliveredEvents": 100, "decodedEvents": 80,
                  "ownUdpEvents": 64, "elapsedMilliseconds": 10}
        (directory / "network-smoke.json").write_text(json.dumps(report), encoding="utf-8")
        (directory / "result.json").write_text('{"status":"verified","exitCode":0}', encoding="utf-8")
        verified = performance.network_summary(self.root)
        self.assertIn("Verified owned traffic", verified)
        self.assertIn("32,768 bytes", verified)
        self.assertIn("64 UDP events", verified)
        for name in ("sentBytes", "receivedBytes", "sentDatagrams", "receivedDatagrams", "samples",
                     "peakBytesPerSecond", "deliveredEvents", "decodedEvents", "ownUdpEvents"):
            report[name] = 0
        report.update(passed=False, skipped=True, reason="trace_permission", traceStatus=5)
        (directory / "network-smoke.json").write_text(json.dumps(report), encoding="utf-8")
        (directory / "result.json").write_text('{"status":"skipped","exitCode":77}', encoding="utf-8")
        skipped = performance.network_summary(self.root)
        self.assertIn("Explicit environment skip", skipped)
        self.assertIn("not a successful traffic test", skipped)
        (directory / "result.json").write_text('{"status":"failed","exitCode":1}', encoding="utf-8")
        self.assertIn("**Failed.**", performance.network_summary(self.root))

    def test_network_summary_revalidates_native_evidence_before_claiming_success(self):
        directory = self.root / "network-smoke"
        directory.mkdir()
        (directory / "result.json").write_text('{"status":"verified","exitCode":0}', encoding="utf-8")
        (directory / "network-smoke.json").write_text('{"passed":true}', encoding="utf-8")
        with self.assertRaises(ValueError):
            performance.network_summary(self.root)


if __name__ == "__main__":
    unittest.main()
