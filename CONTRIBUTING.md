# Contributing

Keep changes focused on a reproducible bug, a usability problem or a measured performance improvement. Explain what changes for someone using the app; include a test where the existing diagnostic suite can cover the behaviour.

## Build and test

Use Windows x64 with Visual Studio 2022 or Build Tools, the Desktop development with C++ workload, a Windows SDK, and CMake 3.24 or newer.

```powershell
./build.ps1
```

This builds production and a separate Debug developer executable, runs all eight CTest cases, and verifies production packaging. The published binary is `artifacts/app/TaskManager.exe`. Build output stays in `artifacts/` and should not be committed.

For optimized developer tests and benchmarks:

```powershell
./build.ps1 -TestConfiguration Release
./benchmark.ps1 -Seconds 15 -EnforceBudgets
```

Benchmark reports are written to `artifacts/benchmark-suite/`. Compare before and after on the same machine with the same workload. Report the Windows build, hardware, update interval and configuration; do not describe Debug timings as production performance.

To rerun just the self-tests or UI verification after an incremental build:

```powershell
cmake --build artifacts/native-tests --config Debug --parallel
ctest --test-dir artifacts/native-tests -C Debug -R "^native-self-test$" --output-on-failure
ctest --test-dir artifacts/native-tests -C Debug -R "^native-ui-test$" --output-on-failure
```

Use `-C Release` and `--config Release` instead if you built the optimized developer configuration.

## Areas to check

- UI changes: light/dark themes, High Contrast, DPI scaling, keyboard navigation, selection and scroll position. Include screenshots when they help show the change.
- Sampling and sorting: identity handling when PIDs are reused, missing counters, paused updates and grouped rows. Keep slow discovery work off the UI thread.
- Process and startup actions: use processes and fixtures you own for tests. Do not terminate unrelated processes or change real startup registrations to demonstrate a fix.
- Windows integration: replacement must remain optional, reversible and unable to overwrite another application's registration. Normal tests must not change the default Task Manager.
- Diagnostics: guard developer-only tooling with `TASKMGR_DIAGNOSTICS`. Production must remain a standalone executable without diagnostic commands or report generation.

The code is under `native/`. `native/diagnostics.cpp` contains self-tests, and `native/verify.cpp` exercises the native UI headlessly. `tests/production.ps1` checks the release manifest and diagnostic exclusion. GitHub Actions runs the optimized build and benchmark suite.

## Reports and pull requests

For bugs, include reproduction steps, expected and actual behaviour, Windows version/build, and whether you used a release or developer build. Hardware and driver details help with counter or graph issues.

For pull requests, describe the issue, the fix and the checks you ran. Disclose any checks you could not run. Keep unrelated formatting or refactors separate.

Review reports before sharing: process command lines, user names, file paths and startup entries can contain private information. Do not include credentials or real machine inventories as test fixtures.

A project license has not been selected yet.
