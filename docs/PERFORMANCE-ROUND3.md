# Sampling, recovery and feature validation — October 2026

## Result

The changes remove repeated counter-buffer allocation, redundant Windows queries, quadratic startup-token replacement, repeated search work and unnecessary filtered-table repainting. They also add scoped search, CSV export and recovery from failed live sampling. The integrated Windows build passes the functional and responsiveness checks below.

The paired experiment shows modest favorable CPU medians, slightly higher diagnostic private memory and a 22,016-byte larger production executable. **The observed timing ranges overlap, so this experiment does not establish a general whole-application speedup.** Keep the adverse and inconclusive results alongside the favorable measurements.

## Reproducible evidence

The [paired Windows study](https://github.com/ProtonDev-sys/task-manager/actions/runs/38207323082) built both versions from immutable source references on the same runner, using the same installed MSVC/CMake toolchain and Release, LTO and size-optimization settings.

| Identity | Value |
| --- | --- |
| Original source | `062797ecd367c638b56d25ca061d287a3d8d5354` |
| Candidate source measured | `d9badedd3a1cd70b541d9b5f42dca2fe9259e88f` |
| Candidate PR head | `874e46d9d2ed4d92677313ace67c6bd000989e56` in [#39](https://github.com/ProtonDev-sys/task-manager/pull/39) |
| Candidate kind | GitHub's tested PR merge with base `bc133c68c2c3d0bda84fca5927a5140c0cbfe828` |
| Host | Windows Server 2025, build 26100, four logical processors |
| Run and attempt | `38207323082`, attempt `1`, October 12, 2026 |
| Evidence artifact | `paired-performance-38207323082-1` |
| Artifact SHA-256 | `7a8ff5b29b4d046a0ef7ccdaa8d5d35bb5d3532382fad860c70bede7a7ea1671` |

There are three repetitions per version for each of three scenarios, alternating baseline/candidate, candidate/baseline, then baseline/candidate. The 18 runs execute serially. The sampler uses 10 warmup samples and 500 measured samples without sleeping. The fixed Processes-tab idle test uses five seconds of warmup, 15 seconds of measurement and its ordinary one-second update interval. Component workloads exercise navigation, sorting and graph drawing. Screenshot capture and the network smoke test run after measurements.

Both headless and UI timings come from optimized diagnostic executables. Production executable size is recorded separately. The sampler's process-count minimum and maximum were exactly 143 in every repetition of both versions. This establishes comparable process population for that scenario; it does not eliminate hosted-runner load, process activity or allocator noise.

The artifact contains exact source identities, executable hashes, every raw report, the comparison JSON, native screenshot captures and the network result. Independent validation matched all 486 recorded metric values to their raw reports and recomputed all 81 summary rows, including medians, ranges and changes.

## Observed measurements

Ranges are the minimum and maximum across the three repetitions, not confidence intervals. CPU percentage uses one logical processor as 100%; on this four-processor VM, divide by four for a whole-machine percentage.

| Measurement | Original median [min, max] | Candidate median [min, max] | Median change |
| --- | ---: | ---: | ---: |
| Sampler CPU, ms/sample | 0.468750 [0.406250, 0.468750] | 0.437500 [0.437500, 0.437500] | -6.67% |
| Sampler wall time, ms/sample | 0.446623 [0.440408, 0.450797] | 0.444023 [0.439998, 0.450180] | -0.58% |
| Sampler p95, ms | 0.4966 [0.4779, 0.5030] | 0.4886 [0.4830, 0.5086] | -1.61% |
| Sampler initialization, ms | 312.387 [310.565, 326.421] | 321.405 [314.544, 328.153] | +2.89% |
| Sampler private bytes | 9,555,968 [9,531,392, 9,715,712] | 9,756,672 [9,367,552, 9,756,672] | +2.10% |
| Sampler working set, bytes | 22,122,496 [22,114,304, 22,126,592] | 22,228,992 [22,155,264, 22,360,064] | +0.48% |
| Idle CPU, % of one processor | 1.65714 [1.13704, 1.65804] | 1.44842 [1.34224, 1.45226] | -12.60% |
| Idle UI update, ms | 1.54700 [1.20235, 1.68352] | 1.35757 [1.32845, 1.40557] | -12.24% |
| Idle message queue p95, ms | 0.1300 [0.1206, 0.1582] | 0.1360 [0.1140, 0.1386] | +4.62% |
| Idle first sample, ms | 390.544 [389.944, 418.060] | 394.499 [386.524, 409.759] | +1.01% |
| Idle private bytes | 16,236,544 [16,199,680, 16,613,376] | 16,850,944 [16,257,024, 16,986,112] | +3.78% |
| Idle working set, bytes | 47,042,560 [46,669,824, 47,247,360] | 47,423,488 [47,415,296, 47,624,192] | +0.81% |
| Navigation, 20,000 names, ms | 0.417813 [0.357600, 0.462463] | 0.366737 [0.357497, 0.402567] | -12.22% |
| Sorting, 20,000 names, ms | 6.91498 [6.00529, 7.06291] | 6.19572 [5.87296, 6.25100] | -10.40% |
| Graph paint, 1,000 × 600, ms | 6.13124 [6.08324, 6.25358] | 6.29375 [6.07358, 6.33689] | +2.65% |

The sampler CPU difference is 0.03125 ms/sample, or 15.625 ms of process CPU over 500 samples. All six CPU totals are drawn from 0.203125, 0.218750 and 0.234375 seconds: one observed accounting increment exactly equals that favorable median difference. This is the granularity observed in these reports, not a precisely established 6.67% improvement. Sampler CPU changes across the three pairs were -6.67%, -6.67% and +7.69%; idle CPU changes were approximately -12.41%, -12.60% and +18.05%.

Each saturated 500-sample window lasted only 0.223–0.229 seconds. Resource sampling normally refreshes at one-second intervals, so this scenario predominantly measures warm process sampling and resource-cache reuse. It does not repeatedly exercise fresh PDH provider queries. The idle runs each recorded 15 resource updates at the ordinary cadence; timed UI-update observations ranged from 15 to 17 and icon extractions from 33 to 36, so their work is not perfectly identical either. Candidate sampling remained healthy in every measured idle run.

Navigation and sorting also move favorably even though these isolated component algorithms were not changed in this round, reinforcing the need to avoid attributing every median movement to the patches. All the main timing ranges overlap. The 2.6-microsecond sampler wall-time median difference and the 0.20872 percentage-point idle CPU difference do not establish repeatable improvements from three repetitions on a hosted runner.

Private bytes measure committed private memory, not resident RAM. The observed median increases are 196 KiB in the sampler and 600 KiB in the idle diagnostic, with overlapping ranges. Working set increased by 104 KiB and 372 KiB respectively, and its ranges **do not overlap**: the candidate working set is higher in every repetition. Diagnostic UI startup runs additional recovery, export and report fixtures before measurement, including a large CSV allocation. Although the objects are destroyed, retained heap commitment and caches can affect the later absolute memory measurement. The direct sampler benchmark does not run those UI fixtures, so that particular confound cannot explain its memory difference. These data do not isolate an equivalent production-memory change.

### Production size

| Build | Bytes | SHA-256 |
| --- | ---: | --- |
| Original, rebuilt on this runner | 970,240 | `50e2192a80a40801959d5a0930b3bbe023ba50213a3496c14a7911fff5429667` |
| Measured candidate | 992,256 | `2f5803aed15184afcf2be70972d80b91d52488691140f6238ac8081d870709f4` |

The measured candidate adds 22,016 bytes (21.5 KiB), or 2.27%, while adding search, export and recovery behavior. Both production executables passed the administrator-manifest and diagnostic-exclusion checks. This table identifies the exact performance-study builds; subsequent persistence and GPU feedback fixes and release version metadata produce a different release binary and checksum.

## Work removed and correctness retained

- **PDH arrays:** retain the bounded array buffer between reads. A stable counter set can be read in one API call, instead of allocating and doing a size probe on every read. Bounded retries handle changing instances; truncated, invalid and non-finite results are rejected.
- **Process snapshots:** validate the actual returned byte extent, process-record links, thread ranges and name pointers. CPU and I/O deltas use the timestamps belonging to their samples.
- **Network accounting:** retain counter-map nodes for live process identities and prune departed entries. PID plus creation time prevents delayed events from an earlier owner from being charged to a reused PID. Trace sessions have unique ownership and use the actual drain interval.
- **Metadata and icons:** prune stale pending requests, results and owned icons. File metadata is resolved through a verified process handle; the common path uses a small buffer with a bounded long-path fallback.
- **Service inventory:** begin with a useful result buffer, consume paginated results and retain the last complete inventory on failure. Errors are exposed to the UI instead of becoming a misleading empty successful result.
- **Startup expansion:** lowercase the input and token once, then append matches. The previous loop repeatedly folded and rebuilt the whole string. A local seven-round alternating stress test with 2,000 matching tokens measured medians of 139.726 ms before and 0.645989 ms after, under GCC/UBSan. This demonstrates the removed quadratic work in that constructed case; it is not a Windows startup-time or whole-app speed claim. Three thousand deterministic compatibility cases and native cases cover the preserved replacement semantics.
- **Filtering and painting:** parse each scoped query once per operation, reuse process matches across group rows and invalidate changed visible rows. Keep independent selection and keyboard focus tied to process identity. Plain text retains its earlier phrase-matching behavior.
- **Sampling recovery:** retain the sampler and last published snapshot after a transient failure. Retry with interruptible backoff instead of permanently ending the worker, and balance COM lifetime on every exit. Diagnostic reports fail if sampling becomes unhealthy during measurement, including a failure followed by recovery.

The process inventory and update settings are preserved. Path and file metadata remain unresolved when a creation-verified process handle and path cannot be obtained; the former PID-only fallback could attach information from a different process after PID reuse. No new CPU-instruction requirement or compiler-speed claim is introduced by this round.

## Integrated validation

The [combined Windows verification](https://github.com/ProtonDev-sys/task-manager/actions/runs/38207323090) passed on the corrected PR head:

- 31 Python tooling tests on Windows; the same suite on Linux, with the two Windows resource-API integrations explicitly skipped there.
- 19 release and performance-workflow guard tests.
- Portable search and CSV regressions, including 44 and 23 checks respectively, compiled with strict warnings on Linux and as native CTest targets on Windows.
- All 12 CTest targets, including 354 named native self-test checks and the owned UI/recovery/export fixtures.
- Owned-child Efficiency mode enable, disable, idempotence, external-priority preservation and restoration on the Windows Server runner, including disabling a policy enabled outside this application's saved state.
- All 21 component, sampler and UI suite cases.
- Production elevation, diagnostic exclusion, packaging, version consistency and checksum checks.

These counts describe overlapping verification layers; they should not be added together as a count of independent tests. The combined report artifact's SHA-256 is `db463af3865f6ab246241af451c5acb8ab927d8191c1d525098c6cdb5f20a76b`.

### Responsiveness budgets

The existing budgets were applied to all 18 retained UI reports from the combined verification suite without rerunning them. These are separate from the 18 paired-study runs. Every report passed. The values below are the largest measured value across those reports, rather than averages chosen from favorable scenarios.

| Metric | Largest observed value | Existing budget |
| --- | ---: | ---: |
| UI update p95 | 3.6634 ms | 50 ms |
| Cached sort p95 | 1.0500 ms | 50 ms |
| Tab switch p95 | 14.5325 ms | 50 ms |
| Message queue p95 | 32.2983 ms | 100 ms |
| Letter navigation p95 | 0.3386 ms | 100 ms |
| First sample | 370.728 ms | 2,000 ms |

### Real network attribution

After timing and screenshots, the candidate sent and received 32 owned UDP datagrams of 1,024 bytes each on `127.0.0.1`. Every payload matched. The live Windows trace attributed 64 UDP events to the test process across 78 samples, with positive reported throughput. Trace and socket statuses were both zero, `passed` was true and `skipped` was false. The native report recorded 2,283.01 ms before socket/session cleanup; this is not complete process wall time. The peak rate while buffered ETW events drain is not a network throughput benchmark.

This establishes working process attribution on this runner for the owned traffic. It does not validate every TCP/UDP workload or every Windows/driver combination. The diagnostic reports narrow pre-traffic permission/session limitations as explicit skips; missing expected events fail instead of being treated as a successful test. Its additional event counters are compiled out of ordinary diagnostic callbacks and all smoke tooling is excluded from production.

### Visual coverage and limits

The artifact includes 19 named native views in each of the light and dark themes: grouped/expanded/ungrouped processes, CPU/logical CPU, memory, disk, network, GPU, history, startup, users, details, services, compact, summary, search results and an empty search. All 38 image hashes and dimensions were checked against their capture manifest, and all 38 actual images were visually reviewed. No clipping, overlap or illegible primary text was observed. Normal column ellipsis was retained. History coverage includes one populated light-theme row and an empty dark-theme view, not a large populated history.

The review found that the software GPU's missing engine counters produced a blank graph region and a misleading 0% utilization. The [follow-up GPU feedback change](https://github.com/ProtonDev-sys/task-manager/pull/41) distinguishes unavailable counters from a measured zero and adds an explicit unavailable message. Its separate native rendering fixtures compare complete dedicated/shared memory plot regions across available-zero and missing-data states in light, dark and High Contrast colors. CI retains their PNGs even if a native test fails. The PR records their verification and visual review; the study's original captures and measured source identity remain intact. File-save dialogs and complete error-state desktop captures are not part of these 38 study views.

This is a hosted Windows Server VM with a software graphics adapter. It does not establish physical multi-GPU behavior, every firmware format, a large real processor-group topology, all DPI/High Contrast desktop configurations or complete Windows 11 parity. Synthetic native fixtures cover several of those edge cases but do not replace physical hardware testing. The broad performance result remains inconclusive; the specific removed work, functional fixes, passing responsiveness budgets and retained evidence are the supported conclusions.

### Persistence follow-up

The [history and export correction](https://github.com/ProtonDev-sys/task-manager/pull/40) passed [Windows verification run 38209554455](https://github.com/ProtonDev-sys/task-manager/actions/runs/38209554455): all 12 CTests, all 21 suite cases, production checks and packaging. Its artifact SHA-256 is `02fd1620c36853d68fa7c95ebdd49068e6c582080668f46b629b9a4881280047`.

All 25 fixture-bearing UI reports (7 CTest and 18 suite) report successful history and CSV checks with no failure label. They exercise a 244-character CSV basename, complete concurrent history snapshots, exact escaped UTF-8 and full-width unsigned timestamps, preservation of locked or directory destinations and an unrelated `.tmp` neighbor, and cleanup of owned staging files after failures. Individual successful assertion counts are not emitted, so source check labels are not counted as extra independent runtime tests. The persistence correction is later than the paired measurements; no new speedup is inferred from these functional checks.

## Run the study again

The optional **Paired performance study** workflow accepts an immutable baseline SHA when manually dispatched and also runs for PRs labeled `performance-study`. For a local comparison with retained optimized diagnostic and production executables:

```text
python tools/taskmgr.py compare --baseline-executable <before-diagnostics.exe> --executable <after-diagnostics.exe> --baseline-production-executable <before-production.exe> --production-executable <after-production.exe> --repetitions 3 --samples 500 --seconds 15 --warmup-seconds 5
```

Run comparisons serially on the same machine, retain all reports, and inspect ranges, source/binary identity and functional results together. See [CONTRIBUTING.md](../CONTRIBUTING.md) for full verification, screenshot and network-smoke commands.
