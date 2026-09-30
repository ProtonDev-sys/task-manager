# Native Performance Verification

Measured September 30, 2026. This documents the C++ executable, not the archived managed release. Machine-readable evidence is in `benchmarks/native-2026-09-30.json`.

## Build And Environment

- Windows 10 Home, build 19045, x64; Ryzen 7 9800X3D, 8 cores / 16 logical processors.
- MSVC 19.44; C++20 Release `/O2 /GL /LTCG`, static CRT `/MT`, warnings-as-errors.
- Standalone executable: 642,560 bytes. SHA-256: `5DAA8FDBDB32ACA139C0CA51F94363C84B2C9764C36770726C894AA6ACE4F113`.
- `dumpbin /headers` confirms AMD64 and a zero COM descriptor; `/dependents` lists only Windows system DLLs. No CLR or external VC runtime DLL is imported.
- Normal developer desktop workload, not a dedicated isolated benchmarking machine. Runs were serial. Results are host-specific observations, not guarantees or a controlled managed/native speedup comparison.

## Validation

`./build.ps1` passes 39 native checks and both CTest cases. Tests cover letter wrapping/repeat/prefix/case, search, graph boundary interpolation, invalid points, bounded histories, randomized graph properties, native inventory/resources, and priority/affinity/wait-chain/stale-identity/end actions against an application-owned disposable child.

`./benchmark.ps1 -Seconds 5 -EnforceBudgets` passes all 17 cases: self-test, component workloads, sampler, all-tabs, a second fresh interaction instance, idle, high/low/paused/minimized modes, and each of seven tabs. Each diagnostic has a process deadline. Live destructive service/startup/session operations, efficiency changes and memory dumps were not exercised against user targets.

## UI Matrix

Each UI case has a two-second warmup followed by five measured seconds. CPU is percent of **one logical processor**, not overall machine CPU. Private MiB is private committed memory, not working set. UI/queue figures are p95 milliseconds. Icon latency includes startup requests despite the CPU/UI warmup.

| Case | CPU % one core | Private MiB | UI update p95 ms | Queue p95 ms | Icon display p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| all-tabs | 3.415 | 14.711 | 2.175 | 0.212 | 77.364 |
| interaction | 4.957 | 15.691 | 4.126 | 0.383 | 76.874 |
| idle | 1.858 | 14.555 | 1.610 | 0.224 | 74.333 |
| high-speed | 4.032 | 15.535 | 2.024 | 0.203 | 79.691 |
| low-speed | 1.865 | 14.293 | 2.576 | 0.260 | 81.113 |
| paused | 0.310 | 14.090 | 0.000 | 0.235 | 91.720 |
| minimized | 0.311 | 15.348 | 1.471 | 0.207 | 74.790 |
| tab-0 | 5.892 | 14.625 | 2.739 | 0.319 | 81.436 |
| tab-1 | 4.624 | 15.527 | 0.530 | 0.246 | 70.768 |
| tab-2 | 3.714 | 14.523 | 2.665 | 0.284 | 75.477 |
| tab-3 | 2.770 | 15.547 | 1.603 | 0.208 | 206.914 |
| tab-4 | 2.782 | 14.551 | 0.767 | 0.250 | 76.415 |
| tab-5 | 3.417 | 15.660 | 2.319 | 0.189 | 73.227 |
| tab-6 | 2.791 | 15.797 | 1.730 | 0.208 | 79.455 |

All measured table updates satisfy the local 50 ms p95 budget; queue and letter handlers satisfy 100 ms p95, and first-sample time satisfies 2 seconds. A zero metric with zero observations means no work occurred, not instantaneous work. Paused mode retains the initial snapshot and performs no measured sampling. Minimized mode tests its throttled sampler; it does not claim to measure a visible minimized window's compositor.

## Sustained Interaction

A separate run uses `--ui-benchmark --seconds 120 --warmup 20`. It exercises view changes, search, letter cycling, native sampling and offscreen graph painting. The final performance PNG was visually inspected after the 60-second rolling boundary: the line/fill meet the left edge without the previous cutoff.

- First native sample: 225.309 ms.
- UI updates: p95 2.514 ms; max 14.407 ms.
- Message dispatch: p95 0.253 ms; max 15.807 ms.
- Letter navigation: p95 0.193 ms, 314 observations.
- Icon request-to-display: p95 75.116 ms; max 78.790 ms.
- Graph painting: p95 3.655 ms.
- Process CPU: 3.801% of one core; private memory: 15.930 MiB.

The process handle count increases by two during the measured soak. GDI and USER object counts show zero growth. This bounded observation is **not proof that every handle/resource leak is impossible**, and longer lifecycle tests remain valuable.

## Sampler Stages

These measurements are from the sustained run. PDH collection also collects GPU counters, so the PDH stage is not exclusively disk work. Services/session/startup data refresh approximately every five seconds and most iterations reuse it.

| Stage | Mean ms | p95 ms |
| --- | ---: | ---: |
| processInventory | 6.673 | 8.030 |
| cpuAndMemory | 5.696 | 7.906 |
| pdhAndDisk | 0.640 | 0.823 |
| network | 0.472 | 0.596 |
| gpuAggregation | 0.722 | 1.103 |
| servicesSessionsStartup | 0.520 | 2.421 |

## Component Workloads

20,000-row p95: filtering 3.725 ms, prefix navigation 2.319 ms, name sorting 2.900 ms. 1000 x 600 graph painting p95: 4.991 ms. History-window interpolation p95: 0.004 ms.

Component loops use 5 warmup iterations and 30 measured iterations for 1k/5k/20k synthetic process names, filtering, prefix navigation and name sorting. Graph work uses 5 warmups and 100 measured iterations. Synthetic sorting measures name sorting; live UI cases also exercise resource sorting. These figures do not include physical keyboard devices or OS scheduling before a message is posted.

## Measurement Boundaries

Hidden native windows receive real Win32 messages and run the same event handlers as the application. The independent 50 ms message probe measures dispatch latency; it is not hardware-to-photon latency. UI-update timing covers data preparation and virtual-row updates, not every ListView paint or the desktop compositor. Graph-paint timing covers the offscreen GDI+ chart; Performance-only cases mostly repaint on new samples. The all-tabs stress case forces additional chart repaint/navigation work.

Icon request-to-display ends when the UI consumes the result, not at the next displayed frame. Successful extraction and all first deliveries are reported separately; unavailable/protected/iconless/UNC targets retain immediate placeholders. Filesystem caches were not flushed, so startup requests are not a guaranteed cold-disk benchmark. Local icon/version/token operations run off the UI thread, but shutdown can still wait for a blocked worker under an abnormal storage/account-service stall.

Hidden diagnostics do not save preferences/history or invoke user process actions. Consequently these measurements do not benchmark persistence filesystem latency or every action dialog. Full stock Task Manager feature parity remains incomplete; see `NATIVE_PARITY.md`. No handwritten assembly is added without a measured benefit over optimized compiler output.

## Reproduce Headlessly

```powershell
./build.ps1
./benchmark.ps1 -Seconds 15 -EnforceBudgets
$test = Start-Process artifacts/app/TaskManager.exe -ArgumentList '--ui-benchmark --seconds 120 --warmup 20 --output artifacts/soak.json' -WindowStyle Hidden -PassThru
if (-not $test.WaitForExit(180000)) { $test.Kill(); throw 'Owned diagnostic timed out.' }
if ($test.ExitCode -ne 0) { throw 'Native soak failed.' }
```

JSON reports and offscreen PNGs are under ignored `artifacts/`. Private-repository CI compiles/tests on a second Windows host, uploads the standalone `native-task-manager` executable artifact and retains its verification reports. CI checks functional success, not host-specific timing budgets.
