#!/usr/bin/env python3
"""Build, validate, benchmark and package Task Manager with Python's standard library."""

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import tempfile

import production


ROOT = Path(__file__).resolve().parents[1]
ARTIFACTS = ROOT / "artifacts"
DEVELOPER = ARTIFACTS / "native-tests/Release/TaskManager.exe"
PRODUCTION = ARTIFACTS / "native-build/Release/TaskManager.exe"
RELEASE_FILES = ("release.json", "SHA256SUMS.txt", "RELEASE_NOTES.md")


@dataclass(frozen=True)
class Case:
    name: str
    arguments: tuple[str, ...]


def bounded_integer(low, high):
    def parse(value):
        number = int(value)
        if not low <= number <= high:
            raise argparse.ArgumentTypeError(f"must be between {low} and {high}")
        return number
    return parse


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def identity(path, **attributes):
    path = Path(path).resolve(strict=True)
    if not path.is_file() or path.stat().st_size == 0:
        raise ValueError(f"Expected a nonempty executable: {path}")
    return {**attributes, "path": str(path), "bytes": path.stat().st_size, "sha256": digest(path)}


def write_json(path, value):
    # Publish summaries only after every case succeeds, using an atomic replacement.
    path = Path(path)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=path.name + ".", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(value, stream, indent=2, allow_nan=False)
            stream.write("\n")
        except BaseException:
            stream.close()
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def clear_outputs(directory, filenames):
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    # Explicit owned names only: never recursively delete a user-supplied directory.
    for filename in filenames:
        (directory / filename).unlink(missing_ok=True)
    return directory


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate JSON property: {key}")
        result[key] = value
    return result


def reject_constant(value):
    raise ValueError(f"Non-finite JSON number: {value}")


def finite_float(value):
    result = float(value)
    if not math.isfinite(result):
        return reject_constant(value)
    return result


def read_report(path):
    path = Path(path)
    if not path.is_file():
        raise ValueError(f"Benchmark produced no fresh report: {path}")
    try:
        report = json.loads(path.read_text(encoding="utf-8-sig"),
                            object_pairs_hook=unique_object, parse_constant=reject_constant,
                            parse_float=finite_float)
    except (ValueError, UnicodeError) as error:
        raise ValueError(f"Benchmark returned invalid JSON: {path}: {error}") from error
    if type(report) is not dict or report.get("passed") is not True or report.get("error") is not None:
        raise ValueError(f"Benchmark did not report success: {path}")
    return report


def run_diagnostic(executable, case, path, timeout):
    path = Path(path)
    path.unlink(missing_ok=True)
    try:
        result = subprocess.run([str(executable), *case.arguments, "--output", str(path)],
                                timeout=timeout, check=False, **production.process_options())
    except subprocess.TimeoutExpired as error:
        # subprocess.run kills and reaps only its owned child before raising.
        raise ValueError(f"Benchmark {case.name} exceeded its deadline; "
                         "only its owned diagnostic process was stopped.") from error
    if result.returncode:
        raise ValueError(f"Benchmark {case.name} failed with exit code {result.returncode}. See {path}")
    return read_report(path)


def number(value, name):
    if type(value) not in (int, float):
        raise ValueError(f"Metric {name} must be a finite nonnegative number.")
    try:
        valid = math.isfinite(value) and value >= 0
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(f"Metric {name} must be a finite nonnegative number.")
    return value


def enforce_budgets(report):
    budgets = {"uiUpdate": 50, "cachedSort": 50, "tabSwitch": 50,
               "messageQueue": 100, "letterNavigation": 100}
    violations = []
    for name, limit in budgets.items():
        metric = report.get(name)
        if type(metric) is not dict:
            raise ValueError(f"Missing UI responsiveness metric: {name}")
        count = metric.get("count")
        if type(count) is not int or count < 0:
            raise ValueError(f"Metric {name}.count must be a nonnegative integer.")
        percentile = number(metric.get("p95Milliseconds"), f"{name}.p95Milliseconds")
        if count and percentile > limit:
            violations.append(f"{name} p95 {percentile:g} ms > {limit} ms")
    first = number(report.get("firstSampleMilliseconds"), "firstSampleMilliseconds")
    if first > 2000:
        violations.append(f"first sample {first:g} ms > 2000 ms")
    if violations:
        raise ValueError("Responsiveness budget exceeded: " + "; ".join(violations))


def benchmark_cases(seconds):
    ui = ("--ui-benchmark", "--seconds", str(seconds))
    cases = [Case("self-test", ("--self-test",)),
             Case("components", ("--component-benchmark",)),
             Case("sampler", ("--benchmark", "--samples", "30", "--interval", "100")),
             Case("all-tabs", ui), Case("interaction", ui), Case("idle", ui + ("--idle",)),
             Case("process-sort-spam", ui + ("--idle", "--sort-spam", "--tab", "0")),
             Case("details-sort-spam", ui + ("--idle", "--sort-spam", "--tab", "5")),
             Case("tab-spam", ui + ("--idle", "--tab-spam")),
             Case("dark-tab-spam", ui + ("--dark", "--idle", "--tab-spam")),
             Case("high-speed", ui + ("--interval", "500")),
             Case("low-speed", ui + ("--interval", "4000")),
             Case("paused", ui + ("--idle", "--interval", "0")),
             Case("minimized", ui + ("--minimized",))]
    return cases + [Case(f"tab-{tab}", ui + ("--tab", str(tab))) for tab in range(7)]


def benchmark(arguments):
    cases = benchmark_cases(arguments.seconds)
    directory = clear_outputs(arguments.output_dir, ["suite.json"] + [f"{case.name}.json" for case in cases])
    executable = identity(arguments.executable)["path"]
    results = []
    for case in cases:
        path = directory / f"{case.name}.json"
        report = run_diagnostic(executable, case, path, arguments.seconds + 60)
        if arguments.enforce_budgets and "--ui-benchmark" in case.arguments:
            try:
                enforce_budgets(report)
            except ValueError as error:
                raise ValueError(f"{case.name}: {error}. See {path}") from error
        print(f"{case.name}: passed", flush=True)
        results.append({"name": case.name, "passed": True, "report": str(path)})
    write_json(directory / "suite.json", results)


def comparison_cases(arguments):
    ui = ("--ui-benchmark", "--idle", "--warmup", str(arguments.warmup_seconds),
          "--seconds", str(arguments.seconds))
    cases = [Case("sampler", ("--benchmark", "--samples", str(arguments.samples),
                              "--interval", "0", "--warmup-samples", "10")),
             Case("idle", ui + ("--tab", "0")), Case("components", ("--component-benchmark",))]
    if arguments.theme_comparison:
        cases += [Case("dark-idle", ui + ("--dark", "--tab", "0")),
                  Case("light-performance", ui + ("--tab", "1")),
                  Case("dark-performance", ui + ("--dark", "--tab", "1")),
                  Case("light-tabs", ui + ("--tab-spam",)),
                  Case("dark-tabs", ui + ("--dark", "--tab-spam",))]
    return cases


def extract_metrics(report):
    scalar_names = {"processCpuPercentOneCore", "cpuMillisecondsPerSample", "privateBytes",
                    "workingSetBytes", "peakWorkingSetBytes", "handles", "minimumProcesses",
                    "maximumProcesses", "firstSampleMilliseconds", "initializationMilliseconds"}
    metrics = {}
    for name, value in report.items():
        if name in scalar_names:
            metrics[name] = number(value, name)
        elif type(value) is dict and "meanMilliseconds" in value:
            for field in ("meanMilliseconds", "p95Milliseconds"):
                key = f"{name}.{field}"
                metrics[key] = number(value.get(field), key)
    return metrics


def summarize(runs, cases, repetitions):
    summary = []
    for case in cases:
        selected = [run for run in runs if run["scenario"] == case.name]
        keys = set(selected[0]["metrics"])
        if not keys or any(set(run["metrics"]) != keys for run in selected):
            raise ValueError(f"Inconsistent or missing metrics for {case.name}; cannot compare incomplete reports.")
        for key in sorted(keys):
            before = [run["metrics"][key] for run in selected if run["variant"] == "baseline"]
            after = [run["metrics"][key] for run in selected if run["variant"] == "candidate"]
            if len(before) != repetitions or len(after) != repetitions:
                raise ValueError(f"Incomplete repetitions for {case.name}.")
            baseline, candidate = statistics.median(before), statistics.median(after)
            summary.append({"scenario": case.name, "metric": key,
                            "baselineMedian": baseline, "candidateMedian": candidate,
                            "changePercent": (candidate - baseline) * 100 / baseline if baseline else None,
                            "baselineMin": min(before), "baselineMax": max(before),
                            "candidateMin": min(after), "candidateMax": max(after)})
    return summary


def environment():
    return {"timestampUtc": datetime.now(timezone.utc).isoformat(),
            "operatingSystem": platform.platform(), "logicalProcessors": os.cpu_count()}


def compare(arguments):
    cases = comparison_cases(arguments)
    # Clear all known repetitions, including those from an earlier, longer attempt.
    directory = clear_outputs(arguments.output_dir, ["comparison.json"] + [
        f"{case.name}-{variant}-{repetition}.json" for case in comparison_cases(
            argparse.Namespace(**{**vars(arguments), "theme_comparison": True}))
        for variant in ("baseline", "candidate") for repetition in range(1, 11)])
    binaries = [identity(arguments.baseline_executable, variant="baseline"),
                identity(arguments.executable, variant="candidate")]
    production_binaries = []
    for variant, path in (("baseline", arguments.baseline_production_executable),
                          ("candidate", arguments.production_executable)):
        if path is not None:
            production_binaries.append(identity(path, variant=variant))
    by_variant = {binary["variant"]: binary["path"] for binary in binaries}
    runs = []
    for case in cases:
        for repetition in range(1, arguments.repetitions + 1):
            order = ("baseline", "candidate") if repetition % 2 else ("candidate", "baseline")
            for variant in order:
                path = directory / f"{case.name}-{variant}-{repetition}.json"
                report = run_diagnostic(by_variant[variant], case, path,
                                        arguments.seconds + arguments.warmup_seconds + 90)
                metrics = extract_metrics(report)
                if case.name == "sampler" and "cpuMillisecondsPerSample" not in metrics:
                    raise ValueError("Both executables need the instrumented headless sampler benchmark.")
                runs.append({"scenario": case.name, "variant": variant, "repetition": repetition,
                             "report": str(path), "metrics": metrics})
                print(f"{case.name} {variant} repetition {repetition}: passed", flush=True)
    summary = summarize(runs, cases, arguments.repetitions)
    write_json(directory / "comparison.json", {
        **environment(), "repetitions": arguments.repetitions, "samples": arguments.samples,
        "seconds": arguments.seconds, "warmupSeconds": arguments.warmup_seconds,
        "diagnosticExecutables": binaries, "productionExecutables": production_binaries,
        "summary": summary, "runs": runs})
    print(f"Comparison with medians and observed ranges: {directory / 'comparison.json'}")


def command(arguments, log=None):
    if log is None:
        subprocess.run([str(value) for value in arguments], cwd=ROOT, check=True)
    else:
        with Path(log).open("w", encoding="utf-8") as stream:
            result = subprocess.run([str(value) for value in arguments], cwd=ROOT, stdout=stream,
                                    stderr=subprocess.STDOUT, check=False)
        if result.returncode:
            raise ValueError(f"{arguments[0]} failed with exit {result.returncode}. See {log}")


def configure(directory, diagnostics, generator, *definitions, log=None):
    selection = ["-G", generator] if generator is not None else []
    command(["cmake", "-S", ROOT, "-B", directory, *selection, "-A", "x64",
             f"-DTASKMGR_DIAGNOSTICS={'ON' if diagnostics else 'OFF'}", *definitions], log)


def compile_native(directory, configuration, jobs, log=None):
    command(["cmake", "--build", directory, "--config", configuration, "--parallel", str(jobs)], log)


def build(arguments):
    production.require_windows()
    destination = clear_outputs(ARTIFACTS / "app", ("TaskManager.exe", *RELEASE_FILES))
    release, tests = ARTIFACTS / "native-build", ARTIFACTS / "native-tests"
    configure(release, False, arguments.generator)
    compile_native(release, "Release", arguments.jobs)
    configure(tests, True, arguments.generator)
    compile_native(tests, arguments.test_configuration, arguments.jobs)
    command(["ctest", "--test-dir", tests, "-C", arguments.test_configuration, "--output-on-failure"])
    production.check_production(PRODUCTION)
    target = destination / "TaskManager.exe"
    shutil.copyfile(PRODUCTION, target)
    if digest(PRODUCTION) != digest(target):
        target.unlink(missing_ok=True)
        raise ValueError("Published executable differs from the verified production build.")
    print(f"Verified production executable: {target}; {arguments.test_configuration} developer tests passed.")


COMPILER_VARIANTS = (
    ("balanced", "ON", "ON", "/O2 /Ob2 /DNDEBUG"),
    ("speed", "OFF", "ON", "/O2 /Ob2 /DNDEBUG"),
    ("no-lto", "ON", "OFF", "/O2 /Ob2 /DNDEBUG"),
    ("inline3", "ON", "ON", "/O2 /Ob3 /DNDEBUG"),
)


def compiler_study(arguments):
    production.require_windows()
    directory = clear_outputs(arguments.output_dir, ["study.json", "binaries.json"] + [
        f"{scenario}-{variant}-{repetition}.json" for scenario in ("self-test", "components", "sampler")
        for variant, *_ in COMPILER_VARIANTS for repetition in range(1, 11)])
    binaries = []
    for variant, favor_size, lto, flags in COMPILER_VARIANTS:
        for kind in ("production", "diagnostic"):
            build_dir = directory / f"{variant}-{kind}"
            configure(build_dir, kind == "diagnostic", arguments.generator,
                      f"-DTASKMGR_FAVOR_SIZE={favor_size}", f"-DTASKMGR_LTO={lto}",
                      f"-DCMAKE_CXX_FLAGS_RELEASE={flags}", log=directory / f"{variant}-{kind}-configure.log")
            compile_native(build_dir, "Release", arguments.jobs, directory / f"{variant}-{kind}-build.log")
            executable = build_dir / "Release/TaskManager.exe"
            if kind == "production":
                production.check_production(executable, runtime=False)
            binaries.append(identity(executable, variant=variant, kind=kind,
                                     flags=flags, favorSize=favor_size, lto=lto))
            print(f"Built {variant} {kind}", flush=True)
    write_json(directory / "binaries.json", binaries)
    cases = [Case("self-test", ("--self-test",)), Case("components", ("--component-benchmark",)),
             Case("sampler", ("--benchmark", "--samples", str(arguments.samples),
                              "--interval", "0", "--warmup-samples", "10"))]
    runs = []
    diagnostics = [binary for binary in binaries if binary["kind"] == "diagnostic"]
    for case in cases:
        for repetition in range(1, arguments.repetitions + 1):
            for binary in diagnostics if repetition % 2 else reversed(diagnostics):
                path = directory / f"{case.name}-{binary['variant']}-{repetition}.json"
                report = run_diagnostic(binary["path"], case, path, 120)
                extract_metrics(report)
                if case.name == "sampler" and "cpuMillisecondsPerSample" not in report:
                    raise ValueError("Compiler studies require the instrumented sampler benchmark.")
                runs.append({"scenario": case.name, "variant": binary["variant"], "repetition": repetition,
                             "reportPath": str(path), "measurements": report})
                print(f"Passed {case.name} {binary['variant']} repetition {repetition}", flush=True)
    write_json(directory / "study.json", {**environment(), "repetitions": arguments.repetitions,
                                         "samples": arguments.samples, "binaries": binaries, "runs": runs})


def source_version(root=ROOT):
    contents = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(TaskManagerNative\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)\s", contents)
    if not match:
        raise ValueError("Cannot read the CMake release version.")
    version = match[1]
    expected = version + ".0"
    manifest = production.validate_manifest((root / "app.manifest").read_bytes())
    identities = manifest.findall("{*}assemblyIdentity")
    if len(identities) != 1 or identities[0].get("version") != expected:
        raise ValueError("Application manifest version differs from CMake.")
    resource = (root / "native/app.rc").read_text(encoding="utf-8")
    for name in ("FILEVERSION", "PRODUCTVERSION"):
        matches = re.findall(rf"^\s*{name}\s+(\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+)\s*$", resource, re.M)
        if len(matches) != 1 or re.sub(r"\s", "", matches[0]).replace(",", ".") != expected:
            raise ValueError(f"Resource {name} differs from CMake.")
    for name in ("FileVersion", "ProductVersion"):
        matches = re.findall(rf'VALUE\s+"{name}"\s*,\s*"([^"]+)"', resource)
        if matches != [expected]:
            raise ValueError(f"Resource {name} string differs from CMake.")
    return version


def package(arguments):
    directory = clear_outputs(arguments.artifact_dir, RELEASE_FILES)
    version = source_version()
    executable = directory / "TaskManager.exe"
    production.check_production(executable, runtime=False)
    versions = production.executable_versions(executable)
    if not versions or any(value != version + ".0" for value in versions):
        raise ValueError("Executable version differs from CMake.")
    sha = arguments.sha
    if not sha or not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError("Set GITHUB_SHA or --sha to the full verified Git commit.")
    notes = ROOT / "docs/releases" / f"v{version}.md"
    if not notes.read_text(encoding="utf-8-sig").strip():
        raise ValueError("Release notes must not be empty.")
    shutil.copyfile(notes, directory / "RELEASE_NOTES.md")
    (directory / "SHA256SUMS.txt").write_text(f"{digest(executable)}  TaskManager.exe\n", encoding="ascii")
    # This is the completion marker, written after all package contents are valid.
    write_json(directory / "release.json", {"version": version, "sha": sha})
    print(f"Prepared verified release v{version} for {sha}: {directory}")


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    sub = result.add_subparsers(dest="command", required=True)
    build_parser = sub.add_parser("build", help="build production, run separate developer CTests, verify and copy the executable")
    build_parser.add_argument("--test-configuration", choices=("Debug", "Release"), default="Debug")
    build_parser.set_defaults(function=build)
    benchmark_parser = sub.add_parser("benchmark", help="run all 21 diagnostic scenarios with fresh reports")
    benchmark_parser.add_argument("--seconds", type=bounded_integer(5, 600), default=15)
    benchmark_parser.add_argument("--executable", type=Path, default=DEVELOPER)
    benchmark_parser.add_argument("--output-dir", type=Path, default=ARTIFACTS / "benchmark-suite")
    benchmark_parser.add_argument("--enforce-budgets", action="store_true")
    benchmark_parser.set_defaults(function=benchmark)
    comparison = sub.add_parser("compare", help="run alternating baseline/candidate measurements and summarize medians and ranges")
    comparison.add_argument("--baseline-executable", type=Path, required=True)
    comparison.add_argument("--executable", type=Path, default=DEVELOPER)
    comparison.add_argument("--baseline-production-executable", type=Path)
    comparison.add_argument("--production-executable", type=Path,
                            help="optional production candidate for release-size comparison")
    comparison.add_argument("--output-dir", type=Path, default=ARTIFACTS / "performance-comparison")
    comparison.add_argument("--repetitions", type=bounded_integer(2, 10), default=5)
    comparison.add_argument("--samples", type=bounded_integer(100, 1000), default=500)
    comparison.add_argument("--seconds", type=bounded_integer(5, 600), default=30)
    comparison.add_argument("--warmup-seconds", type=bounded_integer(0, 60), default=20)
    comparison.add_argument("--theme-comparison", action="store_true")
    comparison.set_defaults(function=compare)
    study = sub.add_parser("compiler-study", help="build and measure size/speed, LTO and inlining alternatives")
    study.add_argument("--output-dir", type=Path, default=ARTIFACTS / "compiler-study")
    study.add_argument("--repetitions", type=bounded_integer(2, 10), default=3)
    study.add_argument("--samples", type=bounded_integer(100, 1000), default=500)
    study.set_defaults(function=compiler_study)
    for native_parser in (build_parser, study):
        native_parser.add_argument("--jobs", type=bounded_integer(1, 1024), default=min(8, os.cpu_count() or 1))
        native_parser.add_argument("--generator",
                                   help="optional Visual Studio CMake generator; auto-detect installed Visual Studio by default")
    production_parser = sub.add_parser("check-production", help="check production resources and diagnostic exclusion")
    production_parser.add_argument("--executable", type=Path, default=PRODUCTION)
    production_parser.set_defaults(function=lambda arguments: production.check_production(arguments.executable))
    package_parser = sub.add_parser("package", help="verify versions and prepare release notes, checksum and commit metadata")
    package_parser.add_argument("--artifact-dir", type=Path, default=ARTIFACTS / "app")
    package_parser.add_argument("--sha", default=os.environ.get("GITHUB_SHA"))
    package_parser.set_defaults(function=package)
    return result


def main(argv=None):
    arguments = parser().parse_args(argv)
    try:
        arguments.function(arguments)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Task Manager tooling failed: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("Interrupted; no successful summary was published.", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
