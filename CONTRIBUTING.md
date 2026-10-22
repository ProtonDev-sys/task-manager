# Contributing

Keep changes focused on a reproducible bug, a usability problem or a measured performance improvement. Explain what changes for someone using the app; include a test where the existing diagnostic suite can cover the behaviour.

## Build and test

Use Windows x64 with Visual Studio 2022 or newer (or Build Tools), the Desktop development with C++ workload, a Windows SDK, CMake 3.24 or newer with support for your Visual Studio version, and Python 3.10 or newer. The developer CLI uses only Python's standard library and works from Command Prompt, Git Bash, or another terminal. CMake selects the installed Visual Studio generator automatically; use `--generator "Visual Studio 18 2026"` with build or compiler-study to select a specific installed version.

```text
python -m unittest discover -s tests -p "test_*.py" -v
python tools/taskmgr.py build
```

The tooling regressions use mocked native process requests, owned temporary reports, and two short Python child-process fixtures to verify argument handling and timeout cleanup. They run on Linux and Windows without building or launching the app; Windows also exercises the resource APIs against the running Python executable. The build command builds production and a separate Debug developer executable, runs all registered CTest cases, and verifies production packaging. The published binary is `artifacts/app/TaskManager.exe`. Build output stays in `artifacts/` and should not be committed.

For optimized developer tests and benchmarks:

```text
python tools/taskmgr.py build --test-configuration Release
python tools/taskmgr.py benchmark --seconds 15 --enforce-budgets
```

Benchmark reports are written to `artifacts/benchmark-suite/`. All 21 scenarios must finish successfully before `suite.json` is published. Failed attempts clear old summaries and scenario reports, reject invalid JSON, non-boolean success values, duplicate fields and non-finite numbers, and terminate only their own timed-out child. Compare before and after on the same machine with the same workload. Report the Windows build, hardware, update interval and configuration; do not describe Debug timings as production performance.

For repeated, alternating baseline/candidate performance comparisons, retain an optimized developer executable before making changes, then run:

```text
python tools/taskmgr.py compare --baseline-executable artifacts/baseline/TaskManager.exe
```

Both developer executables must include the instrumented headless sampler benchmark. The comparison runs five repetitions of saturated sampling, a fixed-tab idle UI and component workloads, alternates execution order, checks exit codes and strict validation results, and writes raw reports plus median/min/max measurements to `artifacts/performance-comparison/comparison.json`. Missing or inconsistent metric sets fail the comparison instead of being treated as zero. Supply `--baseline-production-executable` and `--production-executable` to include release file sizes; those paths are optional so diagnostic-only baselines can be compared independently. Developer executable sizes include diagnostic tooling and are not release sizes. Do not build or run another benchmark concurrently with a comparison.

For a short paired CI investigation, use `--repetitions 3 --samples 500 --seconds 15 --warmup-seconds 5`. Preserve both executables and run them serially on the same runner. The report includes executable SHA-256 hashes, sizes, operating system and logical processor count. `--output-dir` selects a report directory; the command removes only its explicitly named report files there.

The optional **Paired performance study** workflow runs when a pull request has the `performance-study` label, or when manually dispatched. Its default baseline is the exact commit `062797ecd367c638b56d25ca061d287a3d8d5354`; manual runs accept another full commit SHA. Pull-request studies measure the workflow's merge commit, and the artifact records both exact source commits. The workflow builds baseline and candidate production/developer executables on the same Windows runner, checks both self-tests and production exclusions, then runs three serial alternating sampler, fixed-tab idle UI and component pairs. It retains every raw report, executable hash, production size and median/min/max summary. Ordinary pull requests continue to run the full verification suite without this extra study.

After timing finishes, the study captures the actual native views in light and dark themes for visual review. Run `python tools/taskmgr.py screenshots` locally to reproduce those captures from an optimized developer build. The command verifies that all 19 views per theme have fresh PNG headers and valid dimensions, and records their hashes in `artifacts/screenshots/screenshots.json`. Inspect the images themselves before claiming visual correctness; automated capture does not establish High Contrast, multiple-DPI or physical-monitor coverage.

For shareable UI previews, add `--demo-data` to the screenshots command. This developer-only mode supplies synthetic process, account, startup and hardware data, disables the live sampler and icon workers, and labels the window **Task Manager - Demo data**. The report records `dataSource: synthetic`. It uses the same native controls and renderer; its displayed metrics are examples and must not be used as performance measurements. The flag, fixture and capture machinery are excluded from the production executable.

After measurements and screenshots, the study runs `python tools/taskmgr.py network-smoke`. This diagnostic sends 32 known UDP datagrams to an owned socket on `127.0.0.1`, verifies their contents and byte counts, and requires positive trace attribution to its own process. It uses no external endpoint. The Python command bounds the native process to 30 seconds by default (`--timeout` accepts 5–120 seconds), clears earlier output, and retains the native report plus a `verified`, `skipped`, or `failed` result in `artifacts/network-smoke/` (or `--output-dir`).

Only an explicit pre-traffic trace limitation can be skipped: native exit 77 with `trace_permission` and status 5 or 1314, or `trace_session_unavailable` and status 50 or 1450. Skips must report zero sent/received traffic, samples and own UDP events. The Python command accepts these documented environment skips without failing CI, but the summary states that live attribution remains unverified. Unknown skip reasons, mismatched statuses, missing events, malformed JSON and incomplete transfers fail. The smoke command and its diagnostic markers are excluded from production builds.

Headless sampler reports include warmup, initialization time, process-count range, stage timings, CPU milliseconds per sample, private committed bytes and resident working-set bytes. CPU percent uses **one logical core** as 100%; divide by the machine's logical processor count to compare with whole-machine CPU percentages. Private bytes and working set are different measurements. Repeated ranges matter: process population, drivers, background load and heap retention introduce noise. A lower minimum or a single fast run is not proof of an improvement.

See [performance investigation](docs/PERFORMANCE.md) for the optimization rationale and measured limits.

To rerun just the self-tests or UI verification after an incremental build:

```text
cmake --build artifacts/native-tests --config Debug --parallel
ctest --test-dir artifacts/native-tests -C Debug -R "^native-self-test$" --output-on-failure
ctest --test-dir artifacts/native-tests -C Debug -R "^native-ui-test$" --output-on-failure
```

Use `-C Release` and `--config Release` instead if you built the optimized developer configuration.

## Areas to check

Use `python tools/taskmgr.py compare --baseline-executable <before.exe> --executable <after.exe> --theme-comparison` for paired light/dark process, performance and tab-burst measurements. `python tools/taskmgr.py compiler-study` builds size/speed, LTO/no-LTO and inlining alternatives and saves raw headless reports, compiler logs and executable identities. It checks diagnostic exclusion for each production variant. Run these serially on a quiet machine; compare production sizes separately from instrumented binaries. See `docs/PERFORMANCE-ROUND2.md` for methodology and rejected experiments.

- UI changes: light/dark themes, High Contrast, DPI scaling, keyboard navigation, selection and scroll position. Include screenshots when they help show the change.
- Sampling and sorting: identity handling when PIDs are reused, missing counters, paused updates and grouped rows. Keep slow discovery work off the UI thread.
- Process and startup actions: use processes and fixtures you own for tests. Do not terminate unrelated processes or change real startup registrations to demonstrate a fix.
- Windows integration: replacement must remain optional, reversible and unable to overwrite another application's registration. Normal tests must not change the default Task Manager.
- Diagnostics: guard developer-only tooling with `TASKMGR_DIAGNOSTICS`. Production must remain a standalone executable without diagnostic commands or report generation.

The code is under `native/`. `native/diagnostics.cpp` contains self-tests, and `native/verify.cpp` exercises the native UI headlessly. `python tools/taskmgr.py check-production --executable <file.exe>` checks the embedded release manifest, diagnostic exclusion and command rejection. It reads Windows resources directly, without locating or invoking the SDK manifest utility. GitHub Actions runs the tooling and release-guard regressions on Linux and Windows before the optimized native build and full benchmark suite.

## Releases

For a release, update the version in `CMakeLists.txt`, `native/app.rc`, and `app.manifest`, and add `docs/releases/v<version>.md` with the changes and verification limits. Use the next appropriate semantic version; do not move an existing release tag.

After the versioned changes merge into `main`, Windows verification rebuilds that exact commit, runs all tests and benchmark scenarios, checks production packaging and matching executable/manifest versions, and prepares `TaskManager.exe` with `SHA256SUMS.txt`. A separate release job publishes those verified artifacts only after verification succeeds. Pull requests run the validation without publishing. An already-published version is left unchanged; a draft can resume only for the same commit. Release publication guards are tested with `python -m unittest discover -s .github/scripts -p test_release.py -v`.

`python tools/taskmgr.py package --sha <full-commit-sha>` prepares the release package locally after a verified build. CI supplies `GITHUB_SHA` automatically. Packaging checks CMake, manifest, numeric resource versions and displayed version strings, then writes release notes, the executable checksum and commit metadata. It never publishes to GitHub; publication stays in the separately guarded release job.

## Reports and pull requests

For bugs, include reproduction steps, expected and actual behaviour, Windows version/build, and whether you used a release or developer build. Hardware and driver details help with counter or graph issues.

For pull requests, describe the issue, the fix and the checks you ran. Disclose any checks you could not run. Keep unrelated formatting or refactors separate.

Review reports before sharing: process command lines, user names, file paths and startup entries can contain private information. Do not include credentials or real machine inventories as test fixtures.

A project license has not been selected yet.
