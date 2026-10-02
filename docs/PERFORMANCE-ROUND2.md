# Performance follow-up: themes, compiler choices and generated code

Measured on October 2, 2026, with MSVC 19.44.35228.0 on the same Windows machine as the first pass. This is a measured optimization study, not proof of a global optimum. Live process populations, Windows APIs, graphics drivers and background activity prevent that claim. Source-wide compiler analysis is also not equivalent to manually proving every function correct or optimal.

## Light and dark share the application, not every drawing path

The sampler, process identity checks, inventory, sorting, history and update scheduling are shared. Changing RGB values alone should not materially alter that work. However, Windows 10 does not provide identical native theme support for every control used here:

- Dark tabs use custom painting; light tabs use the native control implementation.
- Dark menus and the end-task button use owner drawing; their light counterparts retain native drawing.
- Dark list scrollbars require additional nonclient painting while preserving native scrolling and accessibility.
- Both themes custom-paint table headers, but the light separator is a per-pixel gradient while the dark separator is a solid fill.

Consequently, bit-identical timings are not expected. Forcing all controls through custom drawing merely to equalize timings could make the faster path slower and introduce accessibility or visual regressions. `performance.ps1 -ThemeComparison` measures both themes on the process and performance tabs and under tab-switch bursts, without changing sampling intervals.

## Accepted changes

Service grouping previously rebuilt a PID-to-services index and locale-sorted each service group on every Processes or Users table rebuild. It now reuses the index for the same immutable inventory. The cache retains the inventory owner so its entry pointers remain valid. A new inventory or Windows settings/theme notification invalidates it; sample consumption releases an obsolete inventory even while another tab is selected. Synthetic UI checks cover sorting, PID zero exclusion, storage reuse and refreshed identities. No service refresh interval is lengthened.

The Run dialog callback had two 32,768-character automatic buffers. They now allocate only in the Browse and Submit branches, rather than requiring a large stack frame on every callback. MSVC analysis initially reported a conservative 131,260-byte frame. Actual optimized no-LTO assembly is more informative: the compiler reused the mutually exclusive buffers, allocating **65,856 bytes (`0x10140`)** with a `__chkstk` call. The revised callback allocates **352 bytes (`0x160`)** and no longer calls `__chkstk`. Saved registers and caller stack space are additional to these adjustments. This reduces callback stack probing; it does not establish a 65 KB whole-application RAM saving. Browse/Submit still need their temporary buffer capacity.

The final balanced production executable is **928,256 bytes**, versus 926,720 bytes at the start of this follow-up and 943,104 bytes before the first pass. The follow-up adds 1,536 bytes; the combined reduction is 14,848 bytes. This tradeoff is disclosed rather than claiming every individual optimization reduces every metric.

## Rejected graph implementation

An alternative reused both graph-window and GDI+ point buffers. Twelve component runs across four compiler configurations alternated fresh and reused drawing order, with 100 measured draws of each implementation per run. Full bitmap comparisons passed for two graph colours. Reusing window storage reduced the isolated approximately 1–3 microsecond window-construction cost, but graph painting cost approximately 3.7–8 milliseconds. Painting changes ranged from about 5.6% faster to 2.4% slower and were not consistent across repetitions/configurations.

The retained-buffer implementation was removed from production and the source API restored. The exploratory reports and patch are preserved under `artifacts/performance-round2/`; the patch includes related first-pass instrumentation and is research evidence, not a patch to apply blindly. Production does not retain new graph scratch buffers.

## Compiler experiments

`compiler-study.ps1` builds production and diagnostic executables for four configurations, then runs self-tests, component tests and saturated sampler benchmarks serially. It records executable hashes, actual byte lengths and raw reports, reversing variant order on alternating repetitions. All configurations keep exceptions, security checks and the static runtime. Diagnostic sizes are not substitutes for production sizes.

- Balanced: `/O2 /Os /Oi`, existing `/Ot` overrides on table/graph code, `/Ob2`, LTO.
- Speed: `/O2 /Ob2`, LTO, without the size preference.
- No LTO: balanced flags without `/GL` or `/LTCG`.
- Aggressive inline: balanced flags with `/Ob3`.

The initial exploratory matrix included the subsequently rejected graph prototype. It produced 36 passing workload reports. Its production sizes were 928,768 / 1,001,984 / 913,920 / 954,368 bytes respectively. Sampler CPU medians were 7.16 / 8.91 / 8.94 / 6.91 milliseconds per sample, with substantial overlapping variation and 578–656 observed processes. These are not final-source results and do not justify choosing the apparently fastest isolated median. The final-source rerun and paired theme measurements are recorded separately.

The final-source matrix produced another 36 passing reports, with three repetitions per configuration/workload:

| Configuration | Production bytes | Sampler CPU median, ms/sample | CPU observed range | 20,000-row sort median, ms |
| --- | ---: | ---: | ---: | ---: |
| Balanced LTO | 928,256 | 6.531 | 6.000–6.750 | 2.545 |
| Speed LTO | 1,001,984 | 8.469 | 8.250–10.125 | 2.747 |
| No LTO | 914,944 | 8.594 | 8.156–9.344 | 2.710 |
| Aggressive inline LTO | 953,856 | 7.688 | 5.406–8.875 | 2.577 |

Live process counts ranged from 504 to 558. The final balanced binary hash matches the binary used in the paired theme measurements. Raw final reports are in `artifacts/performance-round2/compiler/study.json`; earlier graph-prototype reports are preserved in `compiler-exploratory-reports/study.json`. Synthetic component results also varied, so this is a practical tradeoff choice, not proof of compiler superiority for every workload.

The default remains balanced with LTO: speed-first compilation adds 73,728 bytes without a measured win here; no LTO saves 13,312 bytes but has higher sampler CPU cost in this batch; aggressive inlining adds 25,600 bytes and is inconsistent. A smaller no-LTO executable is not automatically a faster executable; more aggressive inlining is not free. `TASKMGR_LTO=OFF` remains an explicit experiment, not a security downgrade.

## Paired theme measurements

The final balanced candidate was compared with the first-pass executable, not the original commit, using two alternating repetitions, a five-second configured warmup and ten seconds of measured UI activity. UI verification occurs before measurement and can extend the premeasurement period. Thirty-two workload reports passed their functional checks. The sampler used 500 measured samples per run. Raw reports, executable hashes, medians and ranges are in `artifacts/performance-round2/theme-comparison/comparison.json`.

| Workload | Baseline CPU, one-core % | Candidate CPU, one-core % | Baseline private bytes | Candidate private bytes |
| --- | ---: | ---: | ---: | ---: |
| Light process idle | 3.489 | 2.489 | 18,511,872 | 18,868,224 |
| Dark process idle | 3.792 | 2.560 | 17,920,000 | 18,296,832 |
| Light performance idle | 2.327 | 1.863 | 18,434,048 | 17,045,504 |
| Dark performance idle | 1.707 | 2.251 | 17,922,048 | 17,049,600 |
| Light tab bursts | 53.340 | 55.891 | 17,946,624 | 18,507,776 |
| Dark tab bursts | 54.859 | 55.302 | 18,380,800 | 17,498,112 |

These short live-system measurements do **not** establish a universal CPU or memory reduction. Results change direction between workloads. Saturated sampler CPU cost increased from 5.48 to 5.97 ms/sample in this batch despite lower CPU utilization; utilization alone is not throughput. Dark/light results also change order between baseline and candidate, so they do not establish an intrinsic penalty for one palette.

Functional success is separate from responsiveness budgets. Light tab-burst queue p95 reached 140.245 ms in one candidate run; dark reached 257.622 ms. Both exceed the unchanged 100 ms budget. Baseline runs also exceeded it (111.234 / 141.577 ms), but that does not excuse candidate failures. Bursty tab switching remains an unresolved responsiveness limit.

## Audit coverage

All C++ translation units are compiled in the optimized diagnostic target; production also verifies diagnostic exclusion. MSVC `/W4 /WX` remains enabled. Release `/OPT:REF` removes unreferenced COMDAT code, and `/OPT:ICF` folds eligible identical functions. Static callbacks, resource-referenced dialogs and error paths are not classified as dead solely because textual call searches find few references.

The additional `/analyze` pass identified the Run dialog stack issue. Its initial build failed because `/WX` made analyzer warnings fatal, preventing complete coverage. A separate successful audit build used `/analyze /analyze:WX-` to retain analyzer findings while normal compiler warnings still remain fatal. It produced analysis XML for **all 17 C++ translation units**, with **25 retained findings** in `artifacts/performance-round2/complete-audit-findings.json` and the full build log alongside it. The Run dialog large-frame finding is gone.

Remaining findings include tray bitmap allocation/buffer handling, optional theme API lookup, ignored Windows/COM results, other large cold-path stack buffers, an entrypoint annotation, diagnostic conditional expressions and four initialization warnings in table cell construction. The latter cells have explicit aggregate initializers and the numeric totals have default member initializers; these warnings are not evidence that the values are actually uninitialized, but remain recorded rather than silently waived. This is not a clean static-analysis audit or proof that every line is optimal. Remaining findings require targeted review; this performance pass does not indiscriminately change startup registration or process-control behaviour.

Reviewed performance-sensitive areas include sample stage timings and inventory ownership; bounded history storage; visible-row and clipping-aware rendering; coalesced sort/tab/icon notifications; sleeping worker queues; bounded metadata caches; and cold startup/action/integration paths. No per-pixel assembly kernel or arithmetic loop was established as the dominant bottleneck. Further possibilities such as batching the light header gradient and representative PGO remain experiments, not proven improvements.

## Final validation and build location

- All nine CTest cases pass on the final balanced diagnostic build (197.78 seconds). Service-index regression checks execute within the UI verification suite.
- All 21 `benchmark.ps1 -Seconds 5` scenarios pass functional checks, including every tab, both themes, sort/tab bursts, paused and minimized behaviour. Applying the unchanged responsiveness thresholds to all saved reports finds one violation: light `tab-spam` message-queue p95 **143.827 ms**, above 100 ms. The suite was not run with its early-aborting `-EnforceBudgets` switch so every scenario could be measured; this is not a strict budget pass. The retained violations are in `final-suite/budget-exceedances.json`.
- The final compiler matrix has 36 passing reports; the theme comparison has 32. All compiler reports were verified newer than their corresponding final diagnostic binary. Failed/stale reports cannot be reused by the compiler script.
- Production administrator-manifest and diagnostic-exclusion checks pass. Windows refuses unelevated launch with error 740. Elevated runtime diagnostic-command rejection was not tested from this unelevated shell.
- Both new PowerShell scripts parse successfully, and `git diff --check` passes.
- Hash-verified final binaries are staged at `artifacts/native-build/Release/TaskManager.exe` and `artifacts/native-tests/Release/TaskManager.exe`; the study copies remain under `artifacts/performance-round2/compiler/`. The running application at `artifacts/app/TaskManager.exe` was not overwritten or stopped.

## References

- Microsoft: [Size/speed preferences](https://learn.microsoft.com/en-us/cpp/build/reference/os-ot-favor-small-code-favor-fast-code?view=msvc-170) and [link-time code generation](https://learn.microsoft.com/en-us/cpp/build/reference/ltcg-link-time-code-generation?view=msvc-170).
- Microsoft: [Inline expansion](https://learn.microsoft.com/en-us/cpp/build/reference/ob-inline-function-expansion?view=msvc-170). `/Ob3` is more aggressive than `/Ob2`, not a universal speed guarantee.
- Microsoft: [Profile-guided optimization](https://learn.microsoft.com/en-us/cpp/build/profile-guided-optimizations?view=msvc-170). Profile observations can override `/Ob`, `/Os` and `/Ot`; a representative training workload matters.
- Microsoft: [Custom drawing](https://learn.microsoft.com/en-us/windows/win32/controls/about-custom-draw). Different notification/default/custom drawing paths can perform different work.
- Microsoft: [Inline assembler](https://learn.microsoft.com/en-us/cpp/assembler/inline/inline-assembler?view=msvc-170). MSVC x64 does not support inline assembly; intrinsics or separate assembly files are alternatives when profiling justifies them.
- Microsoft: [Code analysis options](https://learn.microsoft.com/en-us/cpp/build/reference/analyze-code-analysis?view=msvc-170).

C++ remains appropriate for this small native Win32 application. Changing languages cannot remove the cost of a Windows inventory call or GDI+ draw call. Prefer less work, fewer allocations and better scheduling before handwritten assembly. An assembly experiment should preserve semantics, support the deployed CPUs and beat optimized compiler output reproducibly.
