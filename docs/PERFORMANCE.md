# Performance investigation

## Scope and language

The application is already C++20, using native Windows controls, GDI/GDI+, native process inventories, PDH and Windows networking APIs. Release builds already use whole-program optimization, intrinsic expansion, function/data elimination and identical-code folding, with speed-biased compilation for table and graph code. Rewriting it in another language is not an established optimization.

For this implementation, retain C++. Assembly is not an appropriate first response to time spent in Windows queries, heap allocation, inventory copying or painting. An instruction-level optimization only helps the portion it actually accelerates; it cannot remove time inside an OS query. Identify a CPU-bound hot loop, inspect the generated code and benchmark an intrinsic-based alternative before considering handwritten assembly. Keep CPU feature compatibility and correctness checks. No assembly or new CPU instruction requirement was added in this investigation.

There is no defensible proof that a complete desktop application cannot run any faster on every workload, processor and Windows version. The stopping criterion is narrower: measurable improvements, preserved behaviour, successful automated checks and explicit remaining limits. Do not trade away security checks, process identity validation, graph quality, update frequency or features just to improve a benchmark.

## Changes

- Services and sessions now live in an immutable shared inventory. Samples retain its ownership and expose read-only spans. Previously every sample deep-copied the cached vectors, cell strings and descriptions even when the inventory was unchanged. Refresh still replaces the inventory every five seconds or on invalidation; earlier samples keep their original storage alive.
- The process query buffer starts at 64 KiB instead of eagerly allocating 1 MiB. Windows' required-size response grows it with headroom under the existing 64 MiB bound, and subsequent samples reuse the allocation. This changes storage sizing, not process/thread coverage or sampling frequency; initial collection may need an extra query.
- GPU process IDs and adapter LUID components use a bounded decimal/hexadecimal parser instead of general-purpose wide `scanf`. It performs no allocation, checks overflow and rejects malformed fields. Removing both production `swscanf_s` callers also lets the linker discard the wide scanning implementation from the statically linked runtime.
- Diagnostic sampling now reports initialization and warmup separately, CPU milliseconds per sample, process-count range, committed private memory, working-set memory and stage timings. Hidden UI reports additionally expose working-set and peak working-set memory.
- Regression checks cover integer boundaries, malformed counter names and shared-inventory reuse, refresh and ownership after sampler destruction. CTest includes a genuinely UI-free sampler test.

## Rejected experiment

Replacing the floating-point display formatter with `std::to_chars` passed numeric edge-case comparisons but increased the production executable from 943,104 to 1,063,424 bytes in the combined experimental build. That tradeoff failed the size objective; the original formatter was restored. The final changes do not include that formatter replacement.

## Measurement

Run `performance.ps1` using two optimized diagnostic executables with the same instrumented sampler. It alternates baseline/candidate order, retains individual JSON reports and publishes medians and observed ranges. Production executable sizes are measured separately, not inferred from developer binaries. Measurements are sequential; do not compile or run other benchmark suites concurrently.

The saturated sampler workload measures throughput and CPU cost without sleeping. The fixed-tab idle workload retains the ordinary one-second update interval. Component tests and the full `benchmark.ps1` suite exercise navigation, sorting, graph rendering, every tab, rapid interactions, update rates, paused and minimized behaviour. Hidden rendering is automated, not a guarantee of zero desktop/compositor involvement; the sampler case does not create a UI.

CPU percent is relative to one logical core. Private bytes are committed private memory, not resident RAM; working set includes resident shared pages as well. Peak working set includes initialization. Heap retention, process churn, driver behaviour and background work can change memory and timing between runs. Headless diagnostic runs without elevation also do not measure administrator-only network attribution. Runtime production command rejection must be verified separately in an elevated test shell.

## Results on this machine

These are first-pass results. See [the follow-up study](PERFORMANCE-ROUND2.md) for theme paths, compiler comparisons, generated-code inspection and the newer production size.

Measured on Windows 10 build 19045.6466, an AMD Ryzen 7 9800X3D and 16 logical processors. Baseline source was commit `3bed058`; the baseline diagnostic executable was rebuilt with the same headless CPU measurement instrumentation before applying the optimizations. Production and diagnostic binaries are measured separately.

The first-pass production executable decreased from **943,104 to 926,720 bytes**, a reduction of **16,384 bytes (1.74%)**. Its administrator manifest and diagnostic exclusion checks passed. The executable in `artifacts/native-build/Release/TaskManager.exe` has since been replaced by the verified follow-up build described in `PERFORMANCE-ROUND2.md`; publishing over `artifacts/app/TaskManager.exe` was blocked because a running application holds that file open. No user process was stopped to replace it.

Two alternating A/B batches expose why whole-application results need caution:

| Measurement (median) | Initial batch: baseline -> candidate | Longer-warmup batch: baseline -> candidate |
| --- | --- | --- |
| Sampler CPU milliseconds/sample | 4.625 -> 4.063 (-12.2%) | 4.969 -> 5.156 (+3.8%) |
| Sampler private committed bytes | 9,719,808 -> 9,437,184 (-2.9%) | 9,883,648 -> 9,994,240 (+1.1%) |
| Cached services/sessions/startup stage milliseconds/sample | 0.06348 -> 0.0000952 | 0.06425 -> 0.0001158 |
| Idle CPU, percent of one logical core | 2.599 -> 3.529 (+35.8%) | 4.830 -> 3.790 (-21.5%) |
| Idle private committed bytes | 17,575,936 -> 17,862,656 (+1.6%) | 20,086,784 -> 19,255,296 (-4.1%) |

The initial batch uses five repetitions, 500 saturated samples, five seconds of UI warmup and 15 seconds of idle measurement. The second uses three repetitions, the same sample count, 20 seconds of UI warmup and 30 seconds of idle measurement. Live process populations also changed substantially between batches. Within each batch the executables alternate order; the batches are not directly interchangeable experiments.

The cached inventory stage consistently becomes more than 99.8% cheaper in these short sampling runs that avoid inventory refreshes. This is a local result, not a 99.8% application speedup: the original stage only consumes about 0.06 milliseconds/sample. The executable size reduction is also reproducible. **A stable end-to-end CPU or memory percentage improvement is not established**: signs reverse between batches and observed ranges overlap. Retain the adverse results rather than selecting the more favourable run. The smaller query buffer avoids eager allocation but may grow on a busy machine; it does not promise a fixed RAM reduction.

Raw reports, executable hashes, individual runs and median/min/max summaries are retained in `artifacts/performance-study/paired/comparison.json` and `artifacts/performance-study/steady/comparison.json`. Working-set measurements are available for the headless sampler and new hidden UI executable; the historical UI baseline lacks that field, so no baseline UI working-set comparison is claimed.

## Validation

The optimized build passes all **nine CTest cases** and production manifest/diagnostic-exclusion checks. The complete **21-case functional benchmark suite** passes; reports are in `artifacts/performance-study/after-functional/suite.json`.

The budget-enforcing suite is **not green**. It stops at dark-tab spam: message-queue p95 is **223.458 milliseconds**, above its 100-millisecond limit, while functional assertions pass. A subsequent functional run records 112.177 milliseconds for the same metric, still above that budget. The first baseline budget-enforcing attempt also hit a tab-spam responsiveness failure. These observations do not establish whether the missed budget is an optimization regression, an existing stress bottleneck or background contention. Do not change the threshold, hide the failure or claim every performance target passes. The original failed reports remain in `artifacts/performance-study/after/`.

The new PowerShell comparison runner passes syntax validation and executes both alternating A/B batches successfully. `git diff --check` passes. Elevated production runtime rejection and administrator-only telemetry remain untested in this unelevated shell. Final diagnostic and production binary snapshots are retained alongside the baseline files in `artifacts/performance-study/`.

## Remaining limits

Live process/CPU/memory queries remain a substantial part of sampling time. They execute Windows and driver code outside this application's optimizer. Any future proposal to collect less data or reduce refresh frequency is a feature/freshness tradeoff, not a free optimization. Further investigation can use representative elevated workloads, Windows 11, machines with multiple GPUs, large process populations and controlled CPU-load scenarios. GPU parser tests do not replace live validation of every vendor's PDH instance format.
