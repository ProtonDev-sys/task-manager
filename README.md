# Task Manager

A native Windows task manager with the classic Windows 10 layout, live hardware graphs, process controls, search, and light and dark themes. Written in C++20. Runs as a single executable without .NET or additional runtime downloads.

## Install

Requires **64-bit Windows 10 or Windows 11** and administrator approval. ARM64 and 32-bit builds are not provided. Windows 11-specific features have not all been verified on Windows 11.

1. Download `TaskManager.exe` from the [latest release](https://github.com/ProtonDev-sys/task-manager/releases/latest).
2. Put it somewhere you want to keep it, then run it. There is no installer for standalone use.
3. Approve the Windows administrator prompt. The app does not launch if you cancel.
4. On first launch, choose **No** at the replacement prompt if you only want to try it alongside Windows Task Manager.

The executable is currently **unsigned**. Windows may show a security warning. Check that the download came from this repository; each release includes a SHA-256 checksum file. In PowerShell, run `Get-FileHash .\TaskManager.exe -Algorithm SHA256` and compare it with `SHA256SUMS.txt`.

Settings and locally recorded CPU history are stored in `%LOCALAPPDATA%\TaskManagerNative`.

### Replace Windows Task Manager (optional)

Enable **Options > Replace default Task Manager** to redirect Windows Task Manager launches, including **Ctrl+Shift+Esc**, to this app. This affects **all accounts on the computer**. Setup installs a protected copy at `%ProgramFiles%\TaskManagerNative\TaskManager.exe`; moving your downloaded copy does not break the replacement.

Turn the same option off to restore Windows Task Manager. Setup does not overwrite another application's replacement, modify Microsoft's executable, or install a background service.

If you need to restore Windows Task Manager outside the UI, run this from an administrator PowerShell terminal:

```powershell
& "$env:ProgramFiles\TaskManagerNative\TaskManager.exe" --disable-replacement
```

**Update:** for standalone use, close the app and replace your downloaded executable. If replacement is enabled, disable it first, close the installed app, delete its old executable, then run the new download and enable replacement again.

**Uninstall:** disable replacement first if enabled, close the app, and delete the executable and its installed directory. Delete `%LOCALAPPDATA%\TaskManagerNative` too if you want to remove settings and recorded history.

## Features

- **Processes:** grouped apps, background and Windows processes; expandable process groups; CPU, memory, I/O, network and GPU activity; end tasks and restart Explorer.
- **Performance:** CPU and per-logical-processor graphs, memory usage, disk activity, network throughput and GPU engines. Available hardware details depend on Windows and the device's drivers.
- **App history:** locally recorded CPU time, with a reset option.
- **Startup:** Run entries, startup folders, boot/logon scheduled tasks, automatic services and drivers, and packaged startup declarations. Supported Run and folder entries can be enabled or disabled; other sources link to Windows management tools.
- **Users, Details and Services:** session activity, process metadata and controls, service status and start/stop/restart actions. Windows protection and permissions still apply.
- **Appearance:** Light, Dark and Use Windows setting; High Contrast follows system colours. Columns and sorting are saved separately for each tab.
- **Search:** filter table rows by name, PID, cell text or available publisher and command-line metadata. Matching groups retain their context. Search does not request a fresh system sample.

Right-click a column heading to choose visible columns. **Ctrl+F** opens search; **Esc** clears it. **F5** refreshes. **Ctrl+Tab** cycles tabs, and **Alt+1** through **Alt+7** selects one directly. Double-click a Performance graph or its sidebar to enter a summary view.

## Differences from Windows Task Manager

This is an independent implementation, not a complete copy of Microsoft's Task Manager.

| Area | What to expect |
| --- | --- |
| Disk activity per process | Read/write transfer rate includes some non-disk I/O; it is not Windows' disk-only trace. |
| Power usage | An estimate from activity, not Windows' energy estimator. |
| App history | CPU time recorded while this app runs. No Windows SRUM network or tile-update history. |
| Startup | Broader inventory than the ordinary Startup apps list, but not exhaustive Autoruns coverage. Startup impact is **Not measured**. Some sources are read-only. |
| Memory and GPU details | Memory composition uses in-use/cached/free categories. Some stock GPU fields, including DirectX feature level and hardware-reserved memory, are absent. |
| Packaged apps | Grouped by package family; names use available file descriptions rather than resolved manifest display names and logos. |
| Process controls | No UAC virtualization toggle or kernel dumps. Affinity covers one processor group, up to 64 logical processors. Protected processes may refuse actions even with elevation. |
| Efficiency mode | Available only when Windows supports querying the process's policy. Requests EcoQoS and lowers priority; it is not a CPU cap. Disabled on the tested Windows 10 build 19045. Live Windows 11 verification is still outstanding. |
| Search and themes | Added to the classic tab layout. Search is substring-based, not a Boolean query language, and does not filter Performance resources. Popup menus and system dialogs remain OS-dependent. |

See [feature differences](docs/PARITY.md) and [startup coverage](docs/STARTUP.md) for details.

## Build from source

On Windows x64, install Visual Studio 2022 or its Build Tools with **Desktop development with C++**, a Windows SDK, and **CMake 3.24 or newer**. Use PowerShell from the repository directory:

```powershell
./build.ps1
```

The script builds the production executable, runs nine self-test, sampler and UI/stress test cases in a separate developer build, checks the production manifest and diagnostic exclusion, and copies the verified binary to `artifacts/app/TaskManager.exe`.

Production requires elevation and excludes test, benchmark and screenshot tooling. Developer builds run without an elevation requirement and must not be distributed as releases. When the build script runs unelevated, it verifies Windows' refusal to launch production without elevation; runtime command-rejection checks require an elevated test shell.

## Contributing

Bug fixes, usability improvements and performance work are welcome. Open an [issue](https://github.com/ProtonDev-sys/task-manager/issues) with your Windows build, reproduction steps and expected behaviour, or send a focused pull request.

For UI changes, include a screenshot and check light, dark and High Contrast modes. For performance changes, include before/after measurements from the same machine. Avoid posting process command lines, user names or startup reports without reviewing them for private information.

Read [CONTRIBUTING.md](CONTRIBUTING.md) for build, test and benchmark commands. A project license has not been selected yet.
