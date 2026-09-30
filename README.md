# Task Manager

A Windows x64 task manager with a Windows 10-style native interface, real system counters, and a low-overhead background sampler. This is an independent implementation, not a patched or redistributed Microsoft executable. It does not replace the installed Windows Task Manager.

## Run

The local release is `artifacts/app/TaskManager.exe`. Keep the accompanying `amd64/` directory beside it; the native ETW support libraries are not embedded. It requires the **.NET 8 Desktop Runtime, x64**. Start normally for process, CPU, memory, GPU, disk, network, startup, service, and user-session information. Run as administrator when you explicitly want per-process disk/network attribution or operations requiring elevation. The application never elevates itself silently or bypasses Windows access checks.

## Implemented

- **Processes:** native virtual rows laid out to the measured Windows 10 geometry (43px header, 28px rows, the stock heat-map shades and selection blend). Grouping follows Windows: an app (a process with a visible window) collects its descendant tree, processes from one package are grouped under the package's display name, service-hosting processes expand to their services, and other background processes stand alone. Includes optional Apps/Background/Windows categories, file-description names and icons (also for services and protected processes, via the kernel's process-ID image query), service-host captions with gear icons, suspended states, CPU/memory/disk/network/GPU/GPU engine, estimated power usage and trend, PID, expansion, sorting, and selection preservation.
- **Performance:** rolling 60-second CPU, logical-processor, memory, physical-disk, network, and GPU graphs. Includes CPU frequency, cores/sockets/caches/virtualization, uptime, memory commitment/pools/cache, SMBIOS memory speed/slots/form factor, disk response time and hardware type, link speed, GPU engines and dedicated/shared memory.
- **App history:** persistent CPU and network usage recorded by this application; packaged apps by default, with an option to include all observed processes. Delete usage history clears only this application's records.
- **Startup:** Run-key and Startup-folder inventory with file-description names and icons, publisher where readable, last BIOS time, enable/disable using StartupApproved, and file location.
- **Users:** named Windows sessions, expandable per-session process lists and aggregate resource readings; explicit, confirmed disconnect and sign-out.
- **Details:** Windows 10 default columns (Name, PID, Status, User name, CPU, Memory, UAC virtualization, Description) plus optional Session ID, threads, handles, I/O and CPU time; native process identities including System Idle Process; account names for every process (SYSTEM, LOCAL SERVICE, …) from the terminal services process list; threads, handles, working set and I/O transfer counters. Process actions verify creation time before acting.
- **Services:** live Service Control Manager inventory, PID, description, status, svchost group, go to Details, and explicit confirmed start/stop/restart.
- **Window controls:** native Win32 menus, compact view, refresh/update speeds, topmost, tray hide/restore, run-new-task dialog, asynchronous end task/tree, priority, affinity, Efficiency mode, full user-process dumps, wait-chain analysis, and confirmed restart of your session's Explorer.
- **Search and keyboard:** executable/PID/publisher search follows tabs; Alt+F/Ctrl+F focuses search, Escape clears, Ctrl+C copies rows, Ctrl+Shift+N opens Run new task, Alt+1..7 switches tabs, and holding Control freezes visible refresh. Numeric inventory sorting persists across refreshes. Memory/network values can switch to percentages.
- **Preferences:** window size, selected tab, update speed, topmost, grouping, history filter, and Processes/Details column widths/order/visibility/sort. Right-click a column header to choose columns; drag headers to reorder.

## Deliberate limits

This release is **not a complete replacement** for every Windows 10 Task Manager function. Fonts, theme rendering, DPI, and protected-system metadata depend on the host. On September 30, 2026 the Processes tab was compared pixel-for-pixel against the installed Windows 10 (19045) Task Manager at 100% scaling, and its column widths, header/row heights and grouping were taken from the stock window's UI Automation tree; the other tabs were matched by eye. Other DPI settings have not been compared.

- App history is not Windows SRUM history. Metered-network and tile-update history are not collected. Startup boot-impact measurement is not implemented and is displayed as `Not measured`.
- Exact per-process disk and network readings require running as administrator (an application-owned ETW session). Without it, Disk falls back to each process's file I/O transfer rate (which also counts non-disk I/O), and Network is shown as `—` rather than a fabricated zero. Power usage and its trend are a heuristic from CPU, GPU, disk and network activity; Windows' own energy estimator is not a public API. I/O columns in Details are total process transfer rates, not physical-disk attribution.
- GPU reporting requires compatible Windows counters/drivers. Grouped utilization is a capped sum heuristic, not a synchronized per-engine aggregate. GPU driver/date/temperature fields and arbitrary engine selection are not implemented.
- CPU frequency is performance-counter-based relative to the registry's nominal frequency, not a hardware-specific instantaneous clock measurement. Memory compression and detailed standby-list composition are not exposed separately.
- UAC virtualization mutation, every optional Details column, kernel dumps, graph-only summary views, and dark/light/system themes are not implemented. Realtime priority is intentionally disabled. Affinity supports one mask of up to 64 processors. See [the researched parity matrix](docs/TASK_MANAGER_PARITY.md).
- Startup-folder shortcut publishers/targets may be unavailable. Protected processes and services retain normal Windows access restrictions. Service/session/startup mutations and elevated ETW were not exercised against the user's live system during verification.

## Performance architecture

The UI is WinForms/native Windows controls. One worker samples the system. A latest-snapshot slot and coalesced notification replace 250 ms UI polling without queuing obsolete updates. Only the active view filters search input. Stable identities prevent selection churn. Hardware metadata initializes in the background. Tables are virtualized; the process row cache is bounded to 512 entries. Histories, caches and native buffers are bounded. Fully paused sampling sleeps until refresh or cancellation.

The main process inventory is one reused `NtQuerySystemInformation` buffer. CPU uses monotonic elapsed time and process creation identity, avoiding a performance counter or open handle per process on every tick. `GetSystemTimes`, `GetPerformanceInfo`, native processor information, DXGI, PDH English counters, and interface counters supply system data. Executable names, icons, and account lookups are cached; device topology and service/session inventories refresh less frequently. Normal sampling is one second; High is 500 ms, Low is four seconds, and minimized windows sample no faster than four seconds without UI painting. Paused mode permits explicit F5 refresh.

The only direct external package is Microsoft's `Microsoft.Diagnostics.Tracing.TraceEvent`, pinned with a dependency lockfile. It supplies optional ETW consumption. The application has no analytics, telemetry, remote backend, or automatic data upload. Settings and history are local in `%LOCALAPPDATA%\TaskManagerClone`. Diagnostic files can contain local process/device/account metadata and are excluded from Git.

### Earlier measurements

September 30, 2026, Windows x64, Ryzen 7 9800X3D / 16 logical processors, RTX 5070, roughly 340 processes; one-second sampling, normal non-elevated execution. Both runs include a 20-second warmup and a **real visible window**, native paints, GPU counters, and metadata. Per-process ETW was unavailable in these runs.

| Measurement | Processes, 120 seconds | Performance, 120 seconds |
| --- | ---: | ---: |
| Own CPU, percent of entire machine | 0.207% | 0.076% |
| Own CPU, percent of one logical core | 3.304% | 1.210% |
| Average sampling latency | 8.31 ms | 7.98 ms |
| Working set at end | 126.54 MiB | 128.77 MiB |
| Private bytes at end | 76.25 MiB | 74.96 MiB |
| Managed allocation during measured interval | 61.34 MiB | 37.21 MiB |
| Gen 0 / 1 / 2 collections | 1 / 0 / 0 | 0 / 0 / 0 |

Raw local reports: `artifacts/verified-live-processes.json` and `artifacts/verified-live-performance.json`. CPU values exclude the desktop compositor, initial startup and final shutdown; the final shutdown-only cleanup change follows these measurements. Results are machine/workload-specific, not guarantees. Native handle/GDI counts grew slightly with process/icon churn; short runs do not prove leak freedom. No controlled apples-to-apples benchmark against Microsoft's Task Manager was performed, so this project does **not** claim to be faster, smaller, or maximally optimized compared with the stock application.

The current optimization pass and matched headless baseline are in [PERFORMANCE.md](docs/PERFORMANCE.md). These older visible-window measurements are historical context, not evidence for the current changes.

## Build and verify

Use Windows x64 with the .NET 8 SDK and network access for the initial NuGet restore. SDK 8.0.425 was used for the verified build. A different SDK patch can change the build-tool dependencies in the lockfile; intentionally updating SDK/tool dependencies requires `dotnet restore --force-evaluate` and a new verification run:

```powershell
.\build.ps1
```

The script restores locked dependencies, builds Release, publishes a runtime-dependent single-file executable, and runs the published executable's self-test. Build output, runtime packages, logs, benchmarks, and screenshots are under ignored `artifacts/`.

Individual commands:

```powershell
dotnet restore --locked-mode
dotnet build -c Release --no-restore
dotnet publish -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o artifacts/app
$test = Start-Process artifacts/app/TaskManager.exe -ArgumentList '--self-test --output artifacts/release-self-test.json' -WindowStyle Hidden -Wait -PassThru
$test.ExitCode
Get-Content artifacts/release-self-test.json
```

The published build passes **78 checks** covering native ABI, metrics, inventories, owned-child identity/priority/affinity/Efficiency mode/termination/dumps, wait-chain reports, grouping/PID reuse, history, preferences, selection, search/sorting, rendering, graph timing and pause/F5/cancellation. Tests do not terminate unrelated processes, restart your Explorer, mutate startup/services, or disconnect/sign out sessions. Check exit code and report together; an old report does not prove a new run passed.

```powershell
.\benchmark.ps1 -EnforceBudgets
Start-Process artifacts/app/TaskManager.exe -ArgumentList '--component-benchmark --output artifacts/components.json' -WindowStyle Hidden -Wait
dotnet list package --vulnerable --include-transitive
```

`--screenshots --output <dir> [--tabs 0,1,5] [--resources] [--compact] [--group] [--expand]` renders the real window into PNGs without disturbing the desktop (an invisible, click-through, never-activated window captured with PrintWindow); it was used for the visual comparison. `--benchmark` isolates the sampler. `--ui-benchmark` without `--live` cycles tabs and uses offscreen DrawToBitmap; with `--live` it opens and closes its own visible test window. `--tab` selects one of the seven tabs (0 through 6). Diagnostics do not overwrite normal user preferences/history. No computer-use automation is needed for these checks.

## Repository

Private GitHub repository: `ProtonDev-sys/taskmanager`. Windows CI builds/tests the published app and benchmarks native components. Machine-specific diagnostics and `artifacts/` stay ignored; sanitized results and reproduction instructions are committed instead.

## Native references

- https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntquerysysteminformation
- https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getsystemtimes
- https://learn.microsoft.com/en-us/windows/win32/api/psapi/nf-psapi-getperformanceinfo
- https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhgetformattedcounterarrayw
- https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getlogicalprocessorinformationex
- https://learn.microsoft.com/en-us/windows/win32/api/winsvc/nf-winsvc-enumservicesstatusexw
