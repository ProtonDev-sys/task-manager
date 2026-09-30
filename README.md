# Task Manager — Native C++

Windows x64 task manager using C++20, Win32 owner-data controls, GDI+ graphs and native system APIs. The default build is a statically linked, optimized executable: **no CLR, WinForms, .NET Desktop Runtime, NuGet or ETW companion DLLs are required to run it**. The original managed implementation remains in source control as a reference, not part of the native executable.

## Build And Run

Install Visual Studio Build Tools with Desktop development with C++, the Windows SDK, and CMake 3.24+. Run `./build.ps1` in PowerShell. It compiles an x64 Release build, runs native tests headlessly, and publishes `artifacts/app/TaskManager.exe`. Copy that executable alone to another Windows x64 machine. The application does not replace Windows Task Manager and does not automatically elevate.

## Responsiveness And Navigation

- Visible process rows get priority icon requests. Two icon workers extract executable resources and share a bounded path cache. A separate enrichment worker looks up publishers, descriptions and users; that work cannot hold up the icon queue. Placeholders appear immediately. Protected/inaccessible or iconless executables retain a default icon.
- Click the process list and type a letter to select the next matching name, including wraparound. Repeated letters cycle; successive characters within one second form a prefix. This works on virtual rows without loading the full table into a Windows control.
- Search matches process name, PID, publisher and description. Ctrl+F / Alt+F focus search, Escape clears it, F5 refreshes, Ctrl+C copies the selected row, Delete requests ending a task, Ctrl+Shift+N opens Run new task, Ctrl+Tab and Alt+1…7 navigate tabs. Hold Ctrl to freeze visible sample refresh while selecting.
- Graph histories retain and interpolate the predecessor at the left boundary. Startup graphs show only observed history, stretched across the graph; they do not invent 60 seconds of measurements. Double-buffered antialiased lines, fills and grids avoid the clipped start and repaint flicker.
- A sleeping sampler publishes only the newest snapshot through a coalesced message. Normal/High/Low speeds are 1000/500/4000 ms; paused sampling sleeps until F5. Minimized sampling is throttled. Only the active table rebuilds. Resource graphs never call system APIs during painting.

## Native Features

Seven tabs: Processes, Performance, App history, Startup, Users, Details and Services. Includes stable numeric sorting, remembered column widths and optional column hiding, grouping of application child processes, expandable user processes, tray restoration, topmost/hide-on-minimize, compact view, and persistent local CPU history.

Process context menus expose end task, Go to details, priority, efficiency mode, affinity, memory dumps, wait-chain reports, file location, Properties, web search and copy. Startup entries can be toggled; services can start/stop/restart; sessions can disconnect/sign out. Mutations run off the UI thread, verify process creation identity, respect normal Windows permissions and refuse critical/self process actions. Destructive commands require confirmation. Dumps may contain secrets: the UI warns before saving them. Tests mutate only an application-owned disposable child.

## Benchmarks

Run `./benchmark.ps1 -Seconds 15 -EnforceBudgets`. It launches hidden native instances and writes JSON plus offscreen performance PNGs to `artifacts/benchmark-suite/`. Includes pure/native action tests, filter/navigation/sort workloads at 1k/5k/20k rows, graph rendering, live sampling, seven tab cases, pause/high/low/minimized sampling and interaction stress. UI cases include a two-second warmup; direct CLI `--warmup 20` allows a longer warmup. CPU/UI metrics exclude warmup, while initial icon and first-sample measurements remain startup measurements. There is no desktop input automation. Reports distinguish request-to-icon extraction/display, message-queue delay, sampling stages, table updates, search and letter handling.

See `docs/NATIVE_PERFORMANCE.md` for measured results and methodology, and `docs/NATIVE_PARITY.md` for feature coverage. No hand-written assembly is included: the release uses `/O2`, whole-program optimization and link-time code generation; assembly would need a measured advantage over the compiler, not just a lower-level label.

## Honest Limits

This native migration is **not complete feature parity** with Microsoft Task Manager or every feature of the archived managed release. Per-process physical disk/network ETW attribution, SRUM history, package/service-host grouping, per-logical-processor graphs, hardware-detail panels, Windows energy estimates, dark themes and every optional Details column are not implemented yet. Process I/O includes non-disk transfers; unavailable fields are not fabricated. Startup impact is not measured. GPU counters require driver support and use an engine aggregation heuristic. Affinity is one processor group / at most 64 processors. Some metadata stays inaccessible without elevation. The parity document tracks these gaps explicitly.

Preferences/history are local in `%LOCALAPPDATA%\TaskManagerNative`. There is no telemetry or backend. Diagnostic output is ignored by Git and may contain local metadata. The prior managed documentation and numbers are archived in `docs/MANAGED_RELEASE.md`; they are not measurements of this native build.
