# Differences from Windows 10 Task Manager

Reviewed September 30, 2026 against the stock Windows 10 (19045) Task Manager, using side-by-side captures for layout, colours and column geometry.

Updated October 1, 2026: added Light/Dark/System appearance, cached table search and capability-gated Efficiency mode without replacing the classic tab layout. These additions do not imply complete Windows 11 parity.

| Area | Difference |
| --- | --- |
| Elevation | Production requires administrator approval at launch. Developer builds run as the invoking user for unattended tests; without elevation, per-process Network may show "—" and metadata for other users' processes may be unavailable. Elevation does not bypass protected-process restrictions. |
| Disk column | Per-process disk is the process's read + write transfer rate, so it includes some non-disk I/O. Windows uses a disk-only kernel trace. |
| Power usage | Estimated from CPU, GPU, disk and network activity. Windows' energy estimator is not public. |
| App history | CPU time is tracked locally while the app runs. Metered/non-metered network and tile updates (from SRUM) are not shown. |
| Startup impact | Shown as "Not measured". Windows derives it from boot traces. |
| Startup coverage | Extended inventory includes boot/logon tasks, automatic services/drivers, packaged declarations and accessible other profiles. Permission failures and unsupported mechanisms are visible; this is not an exhaustive Autoruns replacement. Packaged enabled state remains Windows-managed. See `docs/STARTUP.md` for the researched source matrix and feature gaps. |
| Packaged apps | Grouped by package family and named by file description. The manifest display name and logo are not resolved. |
| Performance | No DirectX feature-level or GPU hardware-reserved rows. Memory composition shows in use / cached / free rather than Windows' modified and standby lists, which need elevation. |
| Details | No "UAC virtualization" toggle, "Provide feedback" or kernel dumps. Affinity covers one processor group (up to 64 logical processors). |
| Efficiency mode | Individual-process priority + EcoQoS control is capability-gated. The test machine's Windows 10 19045 rejects the power-policy query, so live mutation tests skip explicitly and the command is disabled. Windows 11 live verification is outstanding. Group-wide/child propagation and an Efficiency status column are not implemented. |
| Dark appearance | Table rows/headers, charts, tabs, menu bar, footer and native title bar support dark colours. Native horizontal/vertical scrollbars receive dark painting without replacing their Windows input/accessibility behaviour. Popup menus and system dialogs remain OS-dependent. High Contrast uses system accessibility colours. Windows 10 title-bar compatibility is version-gated; unsupported theme exports are skipped. |
| Search | Cached substring search across table text and available process metadata; no multi-term Boolean query syntax or Performance-resource filtering. Group headings retain their unfiltered totals. |

Efficiency implementation references: Microsoft's Quality of Service guide (`https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service`) and GetProcessInformation contract (`https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocessinformation`). Only documented Win32 APIs are used; unsupported states are not inferred.
