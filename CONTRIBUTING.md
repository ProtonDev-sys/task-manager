# Contributing

Keep changes focused on a reproducible bug, a usability problem or a measured performance improvement. Explain what changes for someone using the app; include a test where the existing diagnostic suite can cover the behaviour.

## Build and test

Use Windows x64 with Visual Studio 2022 or Build Tools, the Desktop development with C++ workload, a Windows SDK, and CMake 3.24 or newer.

```powershell
./tests/benchmark.ps1
./build.ps1
```

The benchmark script regression uses mocked process objects and owned temporary reports; it does not require a build or launch the application. The build script builds production and a separate Debug developer executable, runs all ten CTest cases, and verifies production packaging. The published binary is `artifacts/app/TaskManager.exe`. Build output stays in `artifacts/` and should not be committed.

For optimized developer tests and benchmarks:

```powershell
./build.ps1 -TestConfiguration Release
./benchmark.ps1 -Seconds 15 -EnforceBudgets
```

Benchmark reports are written to `artifacts/benchmark-suite/`. Compare before and after on the same machine with the same workload. Report the Windows build, hardware, update interval and configuration; do not describe Debug timings as production performance.

For repeated, alternating baseline/candidate performance comparisons, retain an optimized developer executable before making changes, then run:

```powershell
./performance.ps1 -BaselineExecutable artifacts/baseline/TaskManager.exe
```

Both developer executables must include the instrumented headless sampler benchmark. The comparison runs five repetitions of saturated sampling, a fixed-tab idle UI and component workloads, alternates execution order, checks exit codes and validation results, and writes raw reports plus median/min/max measurements to `artifacts/performance-comparison/comparison.json`. Use `-BaselineProductionExecutable` to include the original production file size; developer executable sizes include diagnostic tooling and are not release sizes. Do not build or run another benchmark concurrently with a comparison.

Headless sampler reports include warmup, initialization time, process-count range, stage timings, CPU milliseconds per sample, private committed bytes and resident working-set bytes. CPU percent uses **one logical core** as 100%; divide by the machine's logical processor count to compare with whole-machine CPU percentages. Private bytes and working set are different measurements. Repeated ranges matter: process population, drivers, background load and heap retention introduce noise. A lower minimum or a single fast run is not proof of an improvement.

See [performance investigation](docs/PERFORMANCE.md) for the optimization rationale and measured limits.

To rerun just the self-tests or UI verification after an incremental build:

```powershell
cmake --build artifacts/native-tests --config Debug --parallel
ctest --test-dir artifacts/native-tests -C Debug -R "^native-self-test$" --output-on-failure
ctest --test-dir artifacts/native-tests -C Debug -R "^native-ui-test$" --output-on-failure
```

Use `-C Release` and `--config Release` instead if you built the optimized developer configuration.

## Areas to check

Use `./performance.ps1 -BaselineExecutable <before.exe> -Executable <after.exe> -ThemeComparison` for paired light/dark process, performance and tab-burst measurements. `./compiler-study.ps1` builds size/speed, LTO/no-LTO and inlining alternatives and saves raw headless reports. Run these serially on a quiet machine; compare production sizes separately from instrumented binaries. See `docs/PERFORMANCE-ROUND2.md` for methodology and rejected experiments.

- UI changes: light/dark themes, High Contrast, DPI scaling, keyboard navigation, selection and scroll position. Include screenshots when they help show the change.
- Sampling and sorting: identity handling when PIDs are reused, missing counters, paused updates and grouped rows. Keep slow discovery work off the UI thread.
- Process and startup actions: use processes and fixtures you own for tests. Do not terminate unrelated processes or change real startup registrations to demonstrate a fix.
- Windows integration: replacement must remain optional, reversible and unable to overwrite another application's registration. Normal tests must not change the default Task Manager.
- Diagnostics: guard developer-only tooling with `TASKMGR_DIAGNOSTICS`. Production must remain a standalone executable without diagnostic commands or report generation.

The code is under `native/`. `native/diagnostics.cpp` contains self-tests, and `native/verify.cpp` exercises the native UI headlessly. `tests/production.ps1` checks the release manifest and diagnostic exclusion. GitHub Actions runs `tests/benchmark.ps1` before the optimized build and benchmark suite.

## Releases

For a release, update the version in `CMakeLists.txt`, `native/app.rc`, and `app.manifest`, and add `docs/releases/v<version>.md` with the changes and verification limits. Use the next appropriate semantic version; do not move an existing release tag.

After the versioned changes merge into `main`, Windows verification rebuilds that exact commit, runs all tests and benchmark scenarios, checks production packaging and matching executable/manifest versions, and prepares `TaskManager.exe` with `SHA256SUMS.txt`. A separate release job publishes those verified artifacts only after verification succeeds. Pull requests run the validation without publishing. An already-published version is left unchanged; a draft can resume only for the same commit. Release publication guards are tested with `python -m unittest discover -s .github/scripts -p test_release.py -v`.

## Reports and pull requests

For bugs, include reproduction steps, expected and actual behaviour, Windows version/build, and whether you used a release or developer build. Hardware and driver details help with counter or graph issues.

For pull requests, describe the issue, the fix and the checks you ran. Disclose any checks you could not run. Keep unrelated formatting or refactors separate.

Review reports before sharing: process command lines, user names, file paths and startup entries can contain private information. Do not include credentials or real machine inventories as test fixtures.

A project license has not been selected yet.
