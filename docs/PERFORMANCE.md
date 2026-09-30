# Headless performance verification

September 30, 2026; Windows 10 x64 build 19045, Ryzen 7 9800X3D (16 logical processors), RTX 5070, .NET SDK 8.0.425. Non-elevated: per-process ETW disk/network attribution unavailable. Results are workload-specific, not evidence of superiority over Microsoft's Task Manager.

## Matched all-tab comparison

Both baseline/final use 20 seconds warmup, approximately 15 seconds measurement, one-second sampling, native controls and offscreen DrawToBitmap. Process counts vary approximately 379 to 385. CPU includes benchmark rendering, not composition. One paired observation is not repeated statistical evidence.

| Metric | Baseline | Final |
| --- | ---: | ---: |
| Own CPU, whole machine | 0.410% | 0.336% |
| Own CPU, one logical core | 6.555% | 5.382% |
| Managed allocation | 9,604,152 bytes | 8,396,960 bytes |
| Average sampler | 11.129 ms | 11.948 ms |
| Median offscreen render | 11.865 ms | 10.569 ms |
| p95 offscreen render | 25.380 ms | 25.413 ms |

Observed CPU is about 18% lower and allocation about 13% lower. Sampler time increases slightly and p95 render is essentially unchanged: neither is claimed as improved. Responsiveness removes the old 250 ms polling gate, coalesces notification delivery and avoids hidden-view search refreshes.

## Final interaction measurements

| Operation | p95 |
| --- | ---: |
| All-tab sample delivery | 0.059 ms |
| All-tab UI sample application | 5.286 ms |
| Exercised search | 4.611 ms |
| Exercised tab navigation | 11.204 ms |
| Exercised posted-input callback latency | 31.115 ms |

Interaction form shown in 79 ms; first sample observed at 747 ms. First-sample observation uses a 100 ms timer, not an exact startup profiler. Posted callbacks measure queue responsiveness, not keyboard device-to-photon latency. Search/navigation compete with offscreen paint. Short per-tab samples limit percentile confidence.

## Stress and resource coverage

Components use deterministic 1,000/5,000-process fixtures, ten warmups and thirty measurements per operation. At 5,000 processes, p95 grouped refresh is 5.343 ms, process search 16.757 ms, Details refresh 1.434 ms, inventory sort 5.648 ms, Processes paint 13.522 ms and Details paint 23.529 ms. Unchanged-history update allocates approximately 41 bytes/operation; this is not busy/changing history. CPU, memory, three disks, network and GPU paints each measure below 3.3 ms p95 on this host.

`benchmark.ps1` runs 17 sequential headless cases: self-test, components, bare/enriched samplers, all-tabs, interaction, High/Low/Paused, minimized and each of seven fixed tabs. Every case checks exit/report status. `-EnforceBudgets` checks sampled UI cases for p95 UI update <=50 ms, posted-input <=100 ms, navigation <=100 ms and first sample <=2 seconds. Local guardrails are not universal hardware guarantees. Components report timings without enforced budgets. Pause/minimize intentionally have limited/absent UI timing.

Published self-tests contain 78 checks, including owned-child actions, search/sort/selection and refresh/shutdown. No unrelated termination, Explorer restart, service/startup mutation or session sign-out occurs. Raw reports/screenshots stay local under ignored `artifacts/final-benchmark-suite/`; baseline is `artifacts/baseline-all-tabs.json`. Raw diagnostics may contain device/process/account information and are not committed.

## Reproduce

```powershell
.\build.ps1
.\benchmark.ps1 -EnforceBudgets
```

Test executables run hidden. UI tests exclude composition. Components report current-thread allocation; UI-suite allocation covers the process. Stage distributions separate process inventory, devices, sessions/services, attribution, GPU and metadata. Short handle/GDI deltas and zero collections in one run do not prove leak freedom. Privileged ETW and long soak are separate verification work.
