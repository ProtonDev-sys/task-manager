"""Portable tooling regression tests. All processes and report files are owned fixtures."""

import contextlib
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import production
import taskmgr


class ToolingTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="Task Manager Tooling ")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.executable = self.root / "owned executable with spaces.exe"
        self.executable.write_bytes(b"owned executable fixture")
        self.directory = self.root / "reports with spaces"
        self.directory.mkdir()
        self.calls = []
        self.output = io.StringIO()
        self.redirect = contextlib.redirect_stdout(self.output)
        self.redirect.__enter__()
        self.addCleanup(self.redirect.__exit__, None, None, None)

    def benchmark_arguments(self, enforce=False):
        arguments = ["benchmark", "--seconds", "5", "--executable", str(self.executable),
                     "--output-dir", str(self.directory)]
        if enforce:
            arguments.append("--enforce-budgets")
        return taskmgr.parser().parse_args(arguments)

    def fake_run(self, payload='{"passed":true}', returncode=0, timeout=False):
        def run(command, **options):
            self.calls.append(command)
            self.assertEqual(command[-2], "--output")
            path = Path(command[-1])
            self.assertEqual(path.parent, self.directory)
            self.assertFalse(path.exists(), "A report from an earlier run was reused")
            self.assertFalse(options["check"])
            self.assertNotIn("shell", options)
            if payload is not None:
                path.write_text(payload, encoding="utf-8")
            if timeout:
                raise subprocess.TimeoutExpired(command, options["timeout"])
            return subprocess.CompletedProcess(command, returncode)
        return run

    def seed_stale_suite(self):
        for name in ["suite"] + [case.name for case in taskmgr.benchmark_cases(5)]:
            (self.directory / f"{name}.json").write_text('{"passed":true,"stale":true}', encoding="utf-8")
        (self.directory / "unrelated.txt").write_text("keep", encoding="utf-8")

    def assert_no_stale_summary(self):
        self.assertFalse((self.directory / "suite.json").exists())
        for path in self.directory.glob("*.json"):
            self.assertNotIn('"stale"', path.read_text(encoding="utf-8"))
        self.assertEqual((self.directory / "unrelated.txt").read_text(encoding="utf-8"), "keep")

    def test_complete_suite_keeps_all_21_cases_and_paths_with_spaces(self):
        self.seed_stale_suite()
        fake = self.fake_run()

        def run(command, **options):
            if not self.calls:
                self.assertEqual(list(self.directory.glob("*.json")), [])
            return fake(command, **options)

        with patch.object(taskmgr.subprocess, "run", side_effect=run):
            taskmgr.benchmark(self.benchmark_arguments())
        expected = ["self-test", "components", "sampler", "all-tabs", "interaction", "idle",
                    "process-sort-spam", "details-sort-spam", "tab-spam", "dark-tab-spam",
                    "high-speed", "low-speed", "paused", "minimized"] + [f"tab-{tab}" for tab in range(7)]
        summary = json.loads((self.directory / "suite.json").read_text(encoding="utf-8"))
        self.assertEqual([item["name"] for item in summary], expected)
        self.assertEqual(len(self.calls), 21)
        for item, command in zip(summary, self.calls):
            self.assertIs(item["passed"], True)
            self.assertEqual(command[0], str(self.executable))
            self.assertEqual(item["report"], command[-1])
            self.assertEqual(Path(item["report"]).stem, item["name"])

    def test_invalid_reports_cannot_reuse_old_success_or_continue(self):
        scenarios = [None, "{}", '{"passed":false}', '{"passed":"true"}', '{"passed":1}',
                     "true", '[{"passed":true}]', '{"passed":true,"error":"failure"}',
                     '{"passed":true,"error":false}', '{"passed":true,"error":""}',
                     '{"passed":false,"passed":true}', '{"passed":true,"metric":NaN}',
                     '{"passed":true,"metric":Infinity}', '{"passed":true,"metric":1e999}',
                     "{invalid"]
        for payload in scenarios:
            with self.subTest(payload=payload):
                self.calls.clear()
                self.seed_stale_suite()
                with patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(payload)):
                    with self.assertRaises(ValueError):
                        taskmgr.benchmark(self.benchmark_arguments())
                self.assertEqual(len(self.calls), 1)
                self.assert_no_stale_summary()

    def test_nonzero_exit_and_timeout_fail_before_using_a_success_report(self):
        for options, message in [({"returncode": 9}, "exit code 9"),
                                 ({"timeout": True}, "exceeded its deadline")]:
            with self.subTest(options=options):
                self.calls.clear()
                self.seed_stale_suite()
                with patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(**options)):
                    with self.assertRaisesRegex(ValueError, message):
                        taskmgr.benchmark(self.benchmark_arguments())
                self.assertEqual(len(self.calls), 1)
                self.assert_no_stale_summary()

    def test_missing_executable_invalidates_old_summary(self):
        self.seed_stale_suite()
        self.executable.unlink()
        with self.assertRaises(OSError):
            taskmgr.benchmark(self.benchmark_arguments())
        self.assert_no_stale_summary()

    def test_real_diagnostic_child_writes_fresh_report_with_quoted_paths(self):
        script = ("import json,pathlib,sys; "
                  "assert sys.argv[1] == 'value with spaces'; "
                  "pathlib.Path(sys.argv[-1]).write_text(json.dumps({'passed':True}),encoding='utf-8')")
        case = taskmgr.Case("owned-python", ("-c", script, "value with spaces"))
        report = taskmgr.run_diagnostic(sys.executable, case, self.directory / "child report.json", 10)
        self.assertIs(report["passed"], True)

    def test_real_timeout_kills_and_reaps_only_the_owned_child(self):
        processes = []
        original = subprocess.Popen

        def start(*args, **options):
            process = original(*args, **options)
            processes.append(process)
            return process

        case = taskmgr.Case("owned-timeout", ("-c", "import time; time.sleep(30)"))
        with patch.object(taskmgr.subprocess, "Popen", side_effect=start):
            with self.assertRaisesRegex(ValueError, "owned diagnostic process was stopped"):
                taskmgr.run_diagnostic(sys.executable, case, self.directory / "timeout.json", 0.05)
        self.assertEqual(len(processes), 1)
        self.assertIsNotNone(processes[0].poll())
        self.assertFalse((self.directory / "timeout.json").exists())

    def test_screenshot_capture_requires_fresh_complete_light_and_dark_pngs(self):
        arguments = taskmgr.parser().parse_args([
            "screenshots", "--executable", str(self.executable), "--output-dir", str(self.directory)])
        png = b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + struct.pack(">II", 800, 600) + b"\x00" * 9
        names = [f"{index:02d}-{view}.png" for index, view in enumerate(taskmgr.SCREENSHOT_VIEWS)]

        def capture(command, **options):
            directory = Path(command[2])
            self.assertEqual(directory.name == "dark", "--dark" in command)
            self.assertEqual(arguments.demo_data, "--demo-data" in command)
            for name in names:
                self.assertFalse((directory / name).exists())
                (directory / name).write_bytes(png)
            self.assertEqual(options["timeout"], 120)
            return subprocess.CompletedProcess(command, 0)

        with patch.object(taskmgr.subprocess, "run", side_effect=capture):
            taskmgr.screenshots(arguments)
        report = json.loads((self.directory / "screenshots.json").read_text(encoding="utf-8"))
        self.assertEqual(len(report["images"]), 38)
        self.assertEqual(report["dataSource"], "live")
        self.assertEqual({item["theme"] for item in report["images"]}, {"light", "dark"})
        self.assertTrue(all(item["width"] == 800 and item["height"] == 600 for item in report["images"]))
        arguments.demo_data = True
        with patch.object(taskmgr.subprocess, "run", side_effect=capture):
            taskmgr.screenshots(arguments)
        report = json.loads((self.directory / "screenshots.json").read_text(encoding="utf-8"))
        self.assertEqual(report["dataSource"], "synthetic")
        self.assertEqual(len(report["images"]), 38)
        with patch.object(taskmgr.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaises(OSError):
                taskmgr.screenshots(arguments)
        self.assertFalse((self.directory / "screenshots.json").exists())
        self.assertEqual(list((self.directory / "dark").glob("*.png")), [])

    def test_invalid_screenshot_header_or_dimensions_are_rejected(self):
        path = self.directory / "invalid.png"
        for contents in (b"", b"not an image", b"\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR" + b"\x00" * 17):
            with self.subTest(contents=contents):
                path.write_bytes(contents)
                with self.assertRaises(ValueError):
                    taskmgr.png_identity(path)

    def network_arguments(self):
        return taskmgr.parser().parse_args([
            "network-smoke", "--executable", str(self.executable), "--output-dir", str(self.directory)])

    def network_report(self, reason="", trace_status=0):
        skipped = bool(reason)
        return {"passed": not skipped, "skipped": skipped, "reason": reason,
                "traceStatus": trace_status, "socketStatus": 0,
                "sentBytes": 0 if skipped else 32768, "receivedBytes": 0 if skipped else 32768,
                "sentDatagrams": 0 if skipped else 32, "receivedDatagrams": 0 if skipped else 32,
                "samples": 0 if skipped else 2, "peakBytesPerSecond": 0 if skipped else 15000.5,
                "deliveredEvents": 0 if skipped else 100, "decodedEvents": 0 if skipped else 80,
                "ownUdpEvents": 0 if skipped else 64, "elapsedMilliseconds": 10.5}

    def test_network_smoke_retains_fresh_verified_evidence_and_bounded_timeout(self):
        for name in ("network-smoke.json", "result.json"):
            (self.directory / name).write_text('{"status":"stale"}', encoding="utf-8")
        fake = self.fake_run(json.dumps(self.network_report()))

        def run(command, **options):
            self.assertFalse((self.directory / "result.json").exists())
            self.assertEqual(options["timeout"], 30)
            self.assertEqual(command[1], "--network-smoke")
            return fake(command, **options)

        with patch.object(taskmgr.subprocess, "run", side_effect=run):
            taskmgr.network_smoke(self.network_arguments())
        evidence = json.loads((self.directory / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(evidence["status"], "verified")
        self.assertEqual(evidence["exitCode"], 0)
        self.assertEqual(evidence["executable"]["sha256"], taskmgr.digest(self.executable))
        self.assertIs(taskmgr.read_json_report(evidence["report"])["passed"], True)
        self.assertIn("32 owned UDP datagrams", self.output.getvalue())

    def test_network_smoke_accepts_only_documented_pre_traffic_trace_skips(self):
        for reason, trace_status in (("trace_permission", 5), ("trace_permission", 1314),
                                     ("trace_session_unavailable", 50), ("trace_session_unavailable", 1450)):
            with self.subTest(reason=reason, status=trace_status), \
                    patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(
                        json.dumps(self.network_report(reason, trace_status)), returncode=77)):
                taskmgr.network_smoke(self.network_arguments())
            evidence = json.loads((self.directory / "result.json").read_text(encoding="utf-8"))
            self.assertEqual(evidence["status"], "skipped")
            self.assertEqual(evidence["exitCode"], 77)
            self.assertNotIn("error", evidence)
        self.assertIn("Live network attribution remains unverified", self.output.getvalue())

    def test_network_smoke_rejects_forged_skips_and_incomplete_or_nonfinite_traffic(self):
        invalid = []
        for changes in ({"traceStatus": 0}, {"reason": "no_events"}, {"traceStatus": 50},
                        {"socketStatus": 10013}, {"samples": 1}, {"sentBytes": 1},
                        {"receivedDatagrams": 1}, {"ownUdpEvents": 1}, {"peakBytesPerSecond": 1},
                        {"passed": True}, {"skipped": "true"}):
            invalid.append(({**self.network_report("trace_permission", 5), **changes}, 77))
        for changes in ({"passed": 1}, {"skipped": True}, {"reason": "no_events"},
                        {"sentDatagrams": 31}, {"receivedBytes": 32767}, {"samples": 1},
                        {"samples": True}, {"socketStatus": 1}, {"traceStatus": 1},
                        {"ownUdpEvents": 0}, {"decodedEvents": 101}, {"ownUdpEvents": 81},
                        {"deliveredEvents": -1}, {"receivedBytes": "32768"},
                        {"peakBytesPerSecond": 0}, {"peakBytesPerSecond": float("nan")},
                        {"elapsedMilliseconds": float("inf")}, {"error": "failure"}):
            invalid.append(({**self.network_report(), **changes}, 0))
        invalid += [(self.network_report(), 77), (self.network_report("trace_permission", 5), 0),
                    (self.network_report(), 1), (self.network_report(), True), ({}, 77)]
        for report, code in invalid:
            with self.subTest(report=report, code=code), self.assertRaises(ValueError):
                taskmgr.validate_network_smoke(report, code)

    def test_network_smoke_failures_invalidate_old_success_and_retain_failure(self):
        for options in ({"payload": None}, {"payload": "invalid JSON"},
                        {"payload": '{"passed":false,"passed":true}'},
                        {"payload": json.dumps(self.network_report()), "returncode": 1},
                        {"payload": json.dumps(self.network_report()), "returncode": 77},
                        {"payload": None, "timeout": True}):
            with self.subTest(options=options):
                for name in ("network-smoke.json", "result.json"):
                    (self.directory / name).write_text('{"status":"stale"}', encoding="utf-8")
                with patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(**options)):
                    with self.assertRaises(ValueError):
                        taskmgr.network_smoke(self.network_arguments())
                evidence = json.loads((self.directory / "result.json").read_text(encoding="utf-8"))
                self.assertEqual(evidence["status"], "failed")
                self.assertIn("error", evidence)
                self.assertNotIn("stale", json.dumps(evidence))
        self.assertFalse((self.directory / "network-smoke.json").exists())

    def test_network_smoke_missing_executable_removes_stale_native_report(self):
        (self.directory / "network-smoke.json").write_text(json.dumps(self.network_report()), encoding="utf-8")
        self.executable.unlink()
        with patch.object(taskmgr.subprocess, "run") as run, self.assertRaises(OSError):
            taskmgr.network_smoke(self.network_arguments())
        run.assert_not_called()
        self.assertFalse((self.directory / "network-smoke.json").exists())
        evidence = json.loads((self.directory / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(evidence["status"], "failed")
        self.assertIsNone(evidence["exitCode"])

    def ui_report(self):
        report = {"passed": True, "firstSampleMilliseconds": 100}
        for name in ("uiUpdate", "cachedSort", "tabSwitch", "messageQueue", "letterNavigation"):
            report[name] = {"count": 1, "p95Milliseconds": 1}
        return report

    def test_responsiveness_budget_checks_each_measured_metric(self):
        taskmgr.enforce_budgets(self.ui_report())
        for name, value in [("uiUpdate", 51), ("cachedSort", 51), ("tabSwitch", 51),
                            ("messageQueue", 101), ("letterNavigation", 101),
                            ("firstSampleMilliseconds", 2001)]:
            with self.subTest(name=name):
                report = self.ui_report()
                if name == "firstSampleMilliseconds":
                    report[name] = value
                else:
                    report[name]["p95Milliseconds"] = value
                with self.assertRaisesRegex(ValueError, "Responsiveness budget exceeded"):
                    taskmgr.enforce_budgets(report)

    def test_missing_and_non_numeric_metrics_cannot_bypass_budgets(self):
        for invalid in (None, "0", True, -1, float("nan"), float("inf")):
            with self.subTest(value=invalid):
                report = self.ui_report()
                report["uiUpdate"]["p95Milliseconds"] = invalid
                with self.assertRaises(ValueError):
                    taskmgr.enforce_budgets(report)
        report = self.ui_report()
        del report["cachedSort"]
        with self.assertRaises(ValueError):
            taskmgr.enforce_budgets(report)

    def comparison_arguments(self):
        return taskmgr.parser().parse_args([
            "compare", "--baseline-executable", str(self.executable),
            "--executable", str(self.executable), "--output-dir", str(self.directory),
            "--repetitions", "2", "--samples", "100", "--seconds", "5", "--warmup-seconds", "0"])

    def test_comparison_alternates_order_and_preserves_complete_evidence(self):
        payload = json.dumps({"passed": True, "cpuMillisecondsPerSample": 1.5,
                              "privateBytes": 1234, "sampler": {"meanMilliseconds": 2, "p95Milliseconds": 3}})
        with patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(payload)), \
                patch.object(taskmgr.platform, "platform", return_value="Owned test OS"):
            taskmgr.compare(self.comparison_arguments())
        report = json.loads((self.directory / "comparison.json").read_text(encoding="utf-8"))
        self.assertEqual(len(self.calls), 12)
        self.assertEqual([run["variant"] for run in report["runs"]],
                         ["baseline", "candidate", "candidate", "baseline"] * 3)
        self.assertEqual(len(report["diagnosticExecutables"]), 2)
        self.assertEqual(report["diagnosticExecutables"][0]["sha256"], taskmgr.digest(self.executable))
        self.assertEqual(report["productionExecutables"], [])
        for item in report["summary"]:
            self.assertEqual(item["changePercent"], 0)
            self.assertEqual(item["baselineMin"], item["candidateMax"])

    def test_comparison_rejects_invalid_reports_and_clears_older_repetitions(self):
        for payload in ('{"passed":"true"}', '{"passed":true}',
                        '{"passed":true,"cpuMillisecondsPerSample":"0"}'):
            with self.subTest(payload=payload):
                (self.directory / "comparison.json").write_text('{"success":true}', encoding="utf-8")
                old = self.directory / "dark-tabs-baseline-10.json"
                old.write_text('{"passed":true}', encoding="utf-8")
                with patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(payload)):
                    with self.assertRaises(ValueError):
                        taskmgr.compare(self.comparison_arguments())
                self.assertFalse((self.directory / "comparison.json").exists())
                self.assertFalse(old.exists())

    def test_summary_medians_ranges_zero_baseline_and_missing_values(self):
        case = taskmgr.Case("sampler", ())
        runs = [{"scenario": "sampler", "variant": variant, "metrics": {"cpu": value, "zero": 0}}
                for variant, values in (("baseline", [1, 9]), ("candidate", [2, 4])) for value in values]
        summary = {item["metric"]: item for item in taskmgr.summarize(runs, [case], 2)}
        self.assertEqual(summary["cpu"]["baselineMedian"], 5)
        self.assertEqual(summary["cpu"]["candidateMedian"], 3)
        self.assertEqual(summary["cpu"]["changePercent"], -40)
        self.assertEqual(summary["cpu"]["baselineMin"], 1)
        self.assertEqual(summary["cpu"]["baselineMax"], 9)
        self.assertIsNone(summary["zero"]["changePercent"])
        del runs[-1]["metrics"]["cpu"]
        with self.assertRaisesRegex(ValueError, "Inconsistent or missing metrics"):
            taskmgr.summarize(runs, [case], 2)

    def test_atomic_json_failure_does_not_leave_a_partial_file(self):
        path = self.directory / "summary.json"
        with self.assertRaises(ValueError):
            taskmgr.write_json(path, {"value": float("nan")})
        self.assertEqual(list(self.directory.iterdir()), [])

    def test_build_never_publishes_after_ctest_failure(self):
        app = self.root / "app"
        app.mkdir()
        for name in ("TaskManager.exe", *taskmgr.RELEASE_FILES):
            (app / name).write_text("stale success", encoding="utf-8")
        commands = []

        def run(command, **options):
            commands.append([str(value) for value in command])
            if command[0] == "ctest":
                raise subprocess.CalledProcessError(1, command)
            return subprocess.CompletedProcess(command, 0)

        arguments = taskmgr.parser().parse_args(["build", "--test-configuration", "Release"])
        with patch.object(taskmgr, "ARTIFACTS", self.root), patch.object(production, "require_windows"), \
                patch.object(taskmgr.subprocess, "run", side_effect=run), \
                patch.object(production, "check_production") as check:
            with self.assertRaises(subprocess.CalledProcessError):
                taskmgr.build(arguments)
        self.assertEqual(list(app.iterdir()), [])
        check.assert_not_called()
        configurations = [command for command in commands if "-S" in command]
        self.assertEqual(len(configurations), 2)
        self.assertIn("-DTASKMGR_DIAGNOSTICS=OFF", configurations[0])
        self.assertIn("-DTASKMGR_DIAGNOSTICS=ON", configurations[1])

    def test_generator_auto_detection_and_explicit_override_keep_x64(self):
        for generator in (None, "Visual Studio 18 2026"):
            with self.subTest(generator=generator), patch.object(taskmgr, "command") as command:
                taskmgr.configure(self.directory, True, generator)
                arguments = command.call_args.args[0]
                self.assertEqual(arguments[arguments.index("-A") + 1], "x64")
                if generator is None:
                    self.assertNotIn("-G", arguments)
                else:
                    self.assertEqual(arguments[arguments.index("-G") + 1], generator)
        self.assertIsNone(taskmgr.parser().parse_args(["build"]).generator)
        self.assertIsNone(taskmgr.parser().parse_args(["compiler-study"]).generator)

    def test_compiler_study_builds_all_variants_checks_production_and_reverses_order(self):
        payload = json.dumps({"passed": True, "cpuMillisecondsPerSample": 1,
                              "sort": {"meanMilliseconds": 1, "p95Milliseconds": 2}})
        arguments = taskmgr.parser().parse_args([
            "compiler-study", "--output-dir", str(self.directory), "--repetitions", "2", "--samples", "100"])

        def compile_native(directory, configuration, jobs, log=None):
            executable = directory / "Release/TaskManager.exe"
            executable.parent.mkdir(parents=True, exist_ok=True)
            executable.write_bytes(b"owned compiler fixture")

        with patch.object(production, "require_windows"), patch.object(taskmgr, "configure") as configure, \
                patch.object(taskmgr, "compile_native", side_effect=compile_native), \
                patch.object(production, "check_production") as check, \
                patch.object(taskmgr.subprocess, "run", side_effect=self.fake_run(payload)), \
                patch.object(taskmgr.platform, "platform", return_value="Owned test OS"):
            taskmgr.compiler_study(arguments)
        self.assertEqual(configure.call_count, 8)
        self.assertEqual(check.call_count, 4)
        self.assertTrue(all(call.kwargs == {"runtime": False} for call in check.call_args_list))
        study = json.loads((self.directory / "study.json").read_text(encoding="utf-8"))
        self.assertEqual(len(study["binaries"]), 8)
        self.assertEqual(len(study["runs"]), 24)
        self.assertEqual([run["variant"] for run in study["runs"][:8]],
                         ["balanced", "speed", "no-lto", "inline3", "inline3", "no-lto", "speed", "balanced"])

        with patch.object(production, "require_windows"), \
                patch.object(taskmgr, "configure", side_effect=ValueError("owned build failure")):
            with self.assertRaisesRegex(ValueError, "owned build failure"):
                taskmgr.compiler_study(arguments)
        self.assertFalse((self.directory / "study.json").exists())
        self.assertFalse((self.directory / "binaries.json").exists())

    def test_release_source_versions_and_drift(self):
        for name in ("CMakeLists.txt", "app.manifest", "native/app.rc"):
            target = self.root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((taskmgr.ROOT / name).read_bytes())
        current = taskmgr.source_version(self.root)
        self.assertRegex(current, r"^\d+\.\d+\.\d+$")
        for name in ("app.manifest", "native/app.rc"):
            target = self.root / name
            original = target.read_text(encoding="utf-8")
            with self.subTest(name=name):
                target.write_text(original.replace(current + ".0", "9.9.9.0"), encoding="utf-8")
                with self.assertRaises(ValueError):
                    taskmgr.source_version(self.root)
                target.write_text(original, encoding="utf-8")

    def test_package_versions_checksum_commit_and_failure_invalidation(self):
        executable = self.directory / "TaskManager.exe"
        executable.write_bytes(self.executable.read_bytes())
        sha = "a" * 40
        arguments = taskmgr.parser().parse_args(["package", "--artifact-dir", str(self.directory), "--sha", sha])
        version = taskmgr.source_version()
        with patch.object(production, "check_production"), \
                patch.object(production, "executable_versions", return_value=[version + ".0"] * 4):
            taskmgr.package(arguments)
        metadata = json.loads((self.directory / "release.json").read_text(encoding="utf-8"))
        self.assertEqual(metadata, {"version": version, "sha": sha})
        self.assertEqual((self.directory / "SHA256SUMS.txt").read_text(encoding="ascii"),
                         taskmgr.digest(executable) + "  TaskManager.exe\n")
        with patch.object(production, "check_production"), \
                patch.object(production, "executable_versions", return_value=["0.0.0.0"]):
            with self.assertRaisesRegex(ValueError, "Executable version differs"):
                taskmgr.package(arguments)
        for name in taskmgr.RELEASE_FILES:
            self.assertFalse((self.directory / name).exists())
        self.assertTrue(executable.exists())


class ProductionTests(unittest.TestCase):
    def test_manifest_elevation_and_ui_access_are_mandatory(self):
        valid = '<assembly xmlns="urn:test"><requestedExecutionLevel level="requireAdministrator" uiAccess="false" /></assembly>'
        production.validate_manifest(valid)
        for invalid in (valid.replace("requireAdministrator", "asInvoker"),
                        valid.replace('uiAccess="false"', 'uiAccess="true"'),
                        '<assembly />', valid.replace("</assembly>", '<requestedExecutionLevel /></assembly>')):
            with self.subTest(manifest=invalid), self.assertRaises(ValueError):
                production.validate_manifest(invalid)

    def test_ascii_and_unaligned_wide_diagnostic_markers_are_rejected(self):
        production.check_markers(b"owned production binary fixture")
        for marker in production.DIAGNOSTIC_MARKERS:
            for encoding in ("ascii", "utf-16-le"):
                with self.subTest(marker=marker, encoding=encoding), self.assertRaises(ValueError):
                    production.check_markers(b"x" + marker.encode(encoding) + b"tail")

    def test_elevated_runtime_rejects_every_diagnostic_command(self):
        with patch.object(production.subprocess, "run", return_value=subprocess.CompletedProcess([], 2)) as run:
            production.check_runtime(Path("owned.exe"), elevated=True)
        self.assertEqual([call.args[0][1] for call in run.call_args_list], list(production.DIAGNOSTIC_COMMANDS))
        for call in run.call_args_list:
            self.assertEqual(call.kwargs["timeout"], 10)

    def test_elevated_runtime_rejects_wrong_exit_and_written_reports(self):
        with patch.object(production.subprocess, "run", return_value=subprocess.CompletedProcess([], 0)):
            with self.assertRaisesRegex(ValueError, "did not reject"):
                production.check_runtime(Path("owned.exe"), elevated=True)

        def write_report(command, **options):
            Path(command[-1]).write_text("unexpected diagnostics", encoding="utf-8")
            return subprocess.CompletedProcess(command, 2)

        with patch.object(production.subprocess, "run", side_effect=write_report):
            with self.assertRaisesRegex(ValueError, "wrote diagnostic output"):
                production.check_runtime(Path("owned.exe"), elevated=True)

    def test_unelevated_launch_accepts_only_windows_elevation_required(self):
        error = OSError("Elevation required")
        error.winerror = 740
        with patch.object(production.subprocess, "run", side_effect=error), contextlib.redirect_stdout(io.StringIO()):
            production.check_runtime(Path("owned.exe"), elevated=False)
        with patch.object(production.subprocess, "run", side_effect=OSError("different error")):
            with self.assertRaises(OSError):
                production.check_runtime(Path("owned.exe"), elevated=False)
        with patch.object(production.subprocess, "run", return_value=subprocess.CompletedProcess([], 2)):
            with self.assertRaisesRegex(ValueError, "started without administrator"):
                production.check_runtime(Path("owned.exe"), elevated=False)

    @unittest.skipUnless(os.name == "nt", "Windows resource API integration")
    def test_windows_version_api_reads_current_python_executable(self):
        versions = production.executable_versions(Path(sys.executable))
        self.assertGreaterEqual(len(versions), 4)
        self.assertTrue(versions[0].startswith(f"{sys.version_info.major}.{sys.version_info.minor}."))

    @unittest.skipUnless(os.name == "nt", "Windows resource API integration")
    def test_windows_manifest_api_reads_current_python_without_running_it(self):
        manifest = production.read_manifest(Path(sys.executable))
        root = production.ET.fromstring(manifest)
        self.assertIsNotNone(root.find(".//{*}requestedExecutionLevel"))


if __name__ == "__main__":
    unittest.main()
