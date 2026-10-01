# Startup coverage and Task Manager research

Reviewed September 30, 2026. The UI retains its Windows 10 layout. This inventory deliberately includes more startup sources than Task Manager's ordinary Startup apps list, without pretending every Windows execution mechanism is a startup app.

## Source matrix

| Source | Coverage | Changes |
| --- | --- | --- |
| Run registrations | HKLM 64/32-bit, current user, accessible loaded/offline user profiles | Supported current-user/machine approval entries only |
| RunOnce and policy Run | Machine and accessible user registrations; separate identities | Read-only |
| Startup folders | Common folder and each accessible profile; shell-folder redirection when available | Supported current-user/common approval entries only |
| Task Scheduler | Recursive folders, hidden/disabled tasks with boot or logon triggers; separate executable/COM actions | Read-only; Manage in Windows opens Task Scheduler |
| Services and drivers | Automatic services, delayed automatic services, boot/system/automatic drivers | Read-only; services can open Services |
| Packaged apps | StartupTask declarations from accessible package manifests; all-user enumeration where permitted | Windows-managed; opens Startup settings |

User and Source location are optional columns. Identical commands are not duplicates when they belong to different users, registry views, sources or task actions. Coverage rows explain incomplete scans and can be copied.

## Limits of entire-system coverage

Windows permissions still apply. An unelevated scan may not read other profiles, package registrations or protected tasks. Elevation can expand coverage but does not guarantee access to locked/encrypted hives, disconnected or inaccessible redirected folders, or protected registrations. Failures are reported rather than silently shown as an empty complete inventory. Offline profiles use a private read-only application hive that is released after the scan; the app does not mount them globally or alter their permissions.

This is not exhaustive Sysinternals Autoruns coverage. Shell extensions, WMI subscriptions, Winlogon components/scripts, RunOnceEx, Active Setup, Group Policy scripts and provider/plugin registrations are not enumerated. Tasks triggered only by other events are outside this boot/logon inventory. Unattached Windows installations, remote machines and default-profile templates are not existing local users. Invalid profile backup keys and SID Classes hives are not separate users.

Packaged manifest Enabled is not the user's current enabled state. User choice and policy can override it, so these rows say Windows-managed instead of fabricating Enabled. Startup impact, boot CPU/disk cost, running-now state and disabled timestamps are not measured here. A registration or missing target does not prove that an app successfully starts.

## Edge cases handled

- Long registry value names, both machine registry views, unsupported value types, unknown approval formats and missing targets.
- Disabled entries and broken shortcuts remain visible. Shortcut targets containing spaces are not split as command lines.
- Other-user environment variables resolve against that profile where known; unresolved variables remain visible instead of resolving to the current user incorrectly.
- Shared folder paths retain distinct owner identities. Redirected startup folders use user shell-folder configuration when readable.
- RunOnce entries stay read-only: prefix characters and administrator-dependent execution have semantics that are not ordinary enable/disable approval.
- Hidden/disabled boot/logon tasks, multiple actions, COM handlers and packaged desktop applications with multiple StartupTask declarations.
- Approval changes re-read the existing state before writing and refuse unknown or stale state. Unsupported sources cannot invoke the Run/folder toggle.

## Resource behavior

Discovery starts only when Startup is requested, on a separate worker with at most one scan in flight. While visible, its inventory is cached for 60 seconds; explicit refresh/actions invalidate it. Normal process updates share an immutable inventory instead of copying it. An unchanged inventory does not rebuild Startup rows. Sorting uses cached rows and measurements, never enumerates startup sources, and retains the existing click coalescing. Paused sampling waits for an in-flight startup scan without repeatedly sampling processes.

There is no external scanner, background service, persistent inventory database or runtime startup log. The release remains one executable using Windows-provided APIs. Explicit diagnostic exports are optional developer artifacts. Network folders and Windows services can still delay a scan; the UI does not wait synchronously for that discovery.

## Stock Task Manager features still missing or limited

These are documented gaps, not claims of complete parity or promises to add costly background collectors:

- Windows 11 process search/filtering, theme choices and Efficiency mode. Idle priority is not equivalent to EcoQoS power throttling.
- Live kernel dumps, distinct from existing user-process dumps. These depend on OS support/elevation and can write large files; any future implementation should be an explicit action only.
- Additional optional Details fields such as cycle time, GDI/USER objects, pool usage, I/O operation counts, platform, elevation and power throttling. Collect expensive fields only when requested, not for every row on every tick.
- Affinity across multiple processor groups on systems with more than 64 logical processors; current affinity covers one group.
- Stock App history's SRUM-backed metered/non-metered network and tile data; this app tracks CPU history locally.
- Boot-derived Startup impact and exact packaged task state. Disk-only per-process tracing and Windows energy estimates also differ, as described in PARITY.md.

## Primary research sources

- Microsoft Sysinternals [Autoruns](https://learn.microsoft.com/en-us/sysinternals/downloads/autoruns): broader autostart categories and other-account coverage.
- Microsoft [Run and RunOnce registry keys](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys): prefix, ordering and administrator caveats.
- Microsoft [StartupTask](https://learn.microsoft.com/en-us/uwp/api/windows.applicationmodel.startuptask): package declarations and user/policy-controlled states.
- Microsoft [RegLoadAppKeyW](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regloadappkeyw): private application hives.
- Microsoft [TaskFolder.GetTasks](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskfolder-gettasks) and [trigger types](https://learn.microsoft.com/en-us/windows/win32/taskschd/trigger-types): hidden tasks and boot/logon triggers.
- Microsoft [Task Manager live dumps](https://learn.microsoft.com/en-us/windows-hardware/drivers/debugger/task-manager-live-dump): supported kernel-dump behavior and restrictions.
- Microsoft [Windows 11 Insider announcement, November 10, 2022](https://blogs.windows.com/windows-insider/2022/11/10/announcing-windows-11-insider-preview-build-22621-891-and-22623-891/): historical introduction of search, themes and Efficiency mode options, not a claim about the latest build.
- Microsoft [Quality of Service](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service): EcoQoS behavior.

Validation uses isolated temporary registry fixtures, native sorting stress tests and read-only comparison against this PC's Windows task/service inventory. Those checks do not establish coverage of every Windows configuration or inaccessible account.
