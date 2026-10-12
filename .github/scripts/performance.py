"""Validate performance-study source identities and summarize retained evidence."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import taskmgr


KEY_METRICS = (
    ("sampler", "cpuMillisecondsPerSample"), ("sampler", "sampler.meanMilliseconds"),
    ("sampler", "privateBytes"), ("sampler", "minimumProcesses"), ("sampler", "maximumProcesses"),
    ("idle", "processCpuPercentOneCore"), ("idle", "uiUpdate.meanMilliseconds"),
    ("idle", "messageQueue.p95Milliseconds"), ("idle", "privateBytes"),
    ("components", "navigation20000.meanMilliseconds"), ("components", "sort20000.meanMilliseconds"),
    ("components", "graphPaint1000x600.meanMilliseconds"),
)


def exact_sha(value):
    if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{40}", value):
        raise ValueError("Baseline and candidate must be full 40-character hexadecimal commit SHAs.")
    return value.lower()


def validate_baseline():
    sha = exact_sha(os.environ.get("TASKMGR_BASELINE_SHA"))
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(f"sha={sha}\n")
    print(f"Validated immutable baseline {sha}.")


def git_sha(directory):
    result = subprocess.run(["git", "-C", str(Path(directory).resolve()), "rev-parse", "HEAD"],
                            capture_output=True, text=True, check=True)
    return exact_sha(result.stdout.strip())


def record(arguments):
    directory = taskmgr.clear_outputs(arguments.output_dir, ["sources.json", "summary.md"])
    taskmgr.clear_outputs(directory / "reports", ["comparison.json"])
    taskmgr.clear_outputs(directory / "screenshots", ["screenshots.json"])
    taskmgr.clear_outputs(directory / "network-smoke", ["network-smoke.json", "result.json"])
    baseline, candidate = git_sha(arguments.baseline_source), git_sha(ROOT)
    if baseline != exact_sha(os.environ.get("TASKMGR_BASELINE_SHA")):
        raise ValueError("Baseline checkout differs from the validated commit.")
    if candidate != exact_sha(os.environ.get("GITHUB_SHA")):
        raise ValueError("Candidate checkout differs from the workflow commit.")
    taskmgr.write_json(directory / "sources.json", {
        "baselineSha": baseline, "candidateSha": candidate,
        "candidateKind": "pull-request merge" if os.environ.get("GITHUB_EVENT_NAME") == "pull_request" else "selected commit",
        "runId": os.environ.get("GITHUB_RUN_ID"), "runAttempt": os.environ.get("GITHUB_RUN_ATTEMPT"),
    })


def load_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"), object_pairs_hook=taskmgr.unique_object,
                      parse_constant=taskmgr.reject_constant, parse_float=taskmgr.finite_float)


def measurement(value):
    value = taskmgr.number(value, "summary")
    return f"{value:,}" if type(value) is int else f"{value:,.6g}"


def network_summary(directory):
    directory = Path(directory) / "network-smoke"
    heading = "\n## Live network attribution\n\n"
    if not (directory / "result.json").is_file():
        return heading + "The network smoke test did not run or did not produce a validated result. Live attribution remains unverified.\n"
    result = load_json(directory / "result.json")
    if type(result) is not dict or result.get("status") not in ("verified", "skipped", "failed"):
        raise ValueError("Malformed network smoke outcome.")
    if result["status"] == "failed":
        return heading + "**Failed.** Live attribution was not verified. Inspect the failed step and retained native report and result before retrying.\n"
    report = load_json(directory / "network-smoke.json")
    status = taskmgr.validate_network_smoke(report, result.get("exitCode"))
    if result["status"] != status:
        raise ValueError("Network smoke outcome differs from the native evidence.")
    if status == "skipped":
        return heading + (f"**Explicit environment skip:** `{report['reason']}` (trace status {report['traceStatus']}). "
                          "The limitation occurred before sending traffic. Live process network attribution remains unverified; "
                          "this is not a successful traffic test.\n")
    return heading + (f"**Verified owned traffic:** {report['receivedDatagrams']} UDP loopback datagrams and "
                      f"{report['receivedBytes']:,} bytes were received with matching packet contents. "
                      f"The trace attributed {report['ownUdpEvents']} UDP events to this process across "
                      f"{report['samples']} samples, with a peak of {measurement(report['peakBytesPerSecond'])} bytes/s. "
                      "This diagnostic ran after the timing comparisons and screenshots.\n")


def render_summary(directory):
    directory = Path(directory)
    heading = "# Paired Task Manager performance study\n\n"
    if not (directory / "reports/comparison.json").is_file():
        return heading + ("The comparison did not complete. No successful performance conclusion is available. "
                          "Inspect the failed job step and retained raw reports before retrying.\n") + network_summary(directory)
    sources = load_json(directory / "sources.json")
    report = load_json(directory / "reports/comparison.json")
    if type(sources) is not dict or type(report) is not dict:
        raise ValueError("Performance evidence must contain JSON objects.")
    baseline, candidate = exact_sha(sources.get("baselineSha")), exact_sha(sources.get("candidateSha"))
    lines = [heading, f"Baseline: `{baseline}`  \nCandidate: `{candidate}`\n\n",
             "Both optimized builds ran serially on the same Windows runner. "
             "Three repetitions alternate baseline/candidate order after warmup. "
             "Ranges below are the observed minimum and maximum across repetitions.\n\n",
             "| Scenario | Metric | Baseline median [min, max] | Candidate median [min, max] | Change |\n",
             "| --- | --- | ---: | ---: | ---: |\n"]
    if type(report.get("repetitions")) is not int or report["repetitions"] != 3 or type(report.get("summary")) is not list:
        raise ValueError("The CI study requires three complete repetitions and a metric summary.")
    rows = {}
    for row in report["summary"]:
        if type(row) is not dict or not isinstance(row.get("scenario"), str) or not isinstance(row.get("metric"), str):
            raise ValueError("Malformed performance summary row.")
        key = row["scenario"], row["metric"]
        if key in rows:
            raise ValueError("Duplicate performance summary metric.")
        rows[key] = row
    for scenario, metric in KEY_METRICS:
        if (scenario, metric) not in rows:
            raise ValueError(f"Missing required summary metric: {scenario}/{metric}")
        row = rows[scenario, metric]
        before, after = taskmgr.number(row.get("baselineMedian"), metric), taskmgr.number(row.get("candidateMedian"), metric)
        change = f"{(after - before) * 100 / before:+.2f}%" if before else "n/a (zero baseline)"
        lines.append(f"| {scenario} | `{metric}` | {measurement(before)} "
                     f"[{measurement(row.get('baselineMin'))}, {measurement(row.get('baselineMax'))}] | "
                     f"{measurement(after)} [{measurement(row.get('candidateMin'))}, "
                     f"{measurement(row.get('candidateMax'))}] | {change} |\n")
    production = report.get("productionExecutables")
    if type(production) is not list or len(production) != 2:
        raise ValueError("Expected both production executable identities.")
    variants = {item.get("variant"): item for item in production if type(item) is dict}
    if set(variants) != {"baseline", "candidate"}:
        raise ValueError("Production identities must include baseline and candidate.")
    lines.append("\n## Production executable sizes\n\n| Build | Bytes | SHA-256 |\n| --- | ---: | --- |\n")
    for variant in ("baseline", "candidate"):
        binary = variants[variant]
        sha = binary.get("sha256")
        if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha):
            raise ValueError("Production identity requires a SHA-256 digest.")
        lines.append(f"| {variant} | {measurement(binary.get('bytes'))} | `{sha}` |\n")
    images = directory / "screenshots/screenshots.json"
    lines.append("\n## Interpretation and visual review\n\n"
                 "CPU percent uses one logical core as 100%. Process-count rows describe the workload, "
                 "not an improvement target. Private bytes measure committed memory and differ from resident working set. "
                 "Hosted-runner load and process population introduce noise; overlapping ranges and small median shifts "
                 "are not evidence of a reliable speedup. The artifact retains every report and executable identity.\n\n")
    lines.append("Light and dark native screenshot captures are included for visual review; PNG header and completeness "
                 "checks do not replace reviewing the actual images.\n" if images.is_file() else
                 "Screenshot capture did not complete; visual review remains outstanding.\n")
    lines.append(network_summary(directory))
    return "".join(lines)


def summary(arguments):
    directory = Path(arguments.output_dir)
    directory.mkdir(parents=True, exist_ok=True)
    content = render_summary(directory)
    (directory / "summary.md").write_text(content, encoding="utf-8")
    output = os.environ.get("GITHUB_STEP_SUMMARY")
    if output:
        with Path(output).open("a", encoding="utf-8") as stream:
            stream.write(content)
    print(content)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("validate-baseline").set_defaults(function=lambda arguments: validate_baseline())
    recording = commands.add_parser("record")
    recording.add_argument("--baseline-source", type=Path, required=True)
    recording.add_argument("--output-dir", type=Path, required=True)
    recording.set_defaults(function=record)
    reporting = commands.add_parser("summary")
    reporting.add_argument("--output-dir", type=Path, required=True)
    reporting.set_defaults(function=summary)
    arguments = parser.parse_args(argv)
    try:
        arguments.function(arguments)
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        print(f"Performance study failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
