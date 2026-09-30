# Task Manager usability coverage

Reviewed September 30, 2026. The visual target is Windows 10 Task Manager; selected modern conveniences are included without claiming full Windows 11 parity. Microsoft's November 10, 2022 announcement documents executable/PID/publisher filtering, cross-page filtering, Alt+F, themes, and Efficiency mode confirmation preferences. Implementation and tests determine status below.

| Area | Implemented | Remaining differences / verification limits |
| --- | --- | --- |
| Navigation/search | Seven tabs, compact view, cross-tab search, Alt+F/Ctrl+F, Escape, Ctrl+Tab, Alt+1..7 | Not every stock accelerator or accessibility behavior compared |
| Refresh/windows | F5, four speeds, Control-to-freeze, topmost, tray hide/restore, saved size/tab | Offscreen tests exclude compositor and physical input latency |
| Tables | Virtual rows, numeric sort, stable selection/scroll, process columns, Ctrl+C, resource values/percentages | Not every optional Details column; inventory search matches displayed cells |
| Processes | App/package/service grouping, expansion, end task/tree, location, properties, online search, go to Details | Normal Windows protected-process restrictions |
| Details actions | Priority, affinity, Efficiency mode, full user-process dump, wait-chain report | No Realtime, UAC virtualization mutation, processor-group affinity or kernel dump; not every lock/deadlock scenario tested |
| Explorer | Confirmed restart of current session's Windows Explorer | Not executed against user's live desktop |
| Performance | CPU/logical processors, memory, disks, network, GPU histories/device data | No graph-only summary, arbitrary GPU-engine selection, temperature/driver date, separate compression/standby breakdown |
| App history | Persistent local CPU/network history, reset, all-process toggle | Not SRUM; no metered/tile-update history |
| Startup | Run keys/folders, enable/disable, publisher/location, BIOS time | Boot impact explicitly Not measured; shortcut metadata may be unavailable |
| Users | Session totals, expandable processes, child actions, confirmed disconnect/sign-out | Live disconnect/sign-out not exercised; no remote-session connection workflow |
| Services | Inventory, go to Details, confirmed start/stop/restart | Live mutations not exercised |
| Appearance | Native Windows 10 light layout/icons | No dark/light/system theme selector; DPI combinations not comprehensively verified |

Efficiency mode verifies creation identity, uses normal permissions, refuses critical/system/self targets and changes power throttling plus priority. Disabling restores Normal priority. Full dumps can contain secrets; creation is explicitly confirmed. Wait-chain inspection runs off the UI thread and reports inaccessible threads. No kernel-driver bypass is used.

## Official research references

- Microsoft, Windows 11 builds 22621.891/22623.891, November 10, 2022: https://blogs.windows.com/windows-insider/2022/11/10/announcing-windows-11-insider-preview-build-22621-891-and-22623-891/
- Power throttling: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_power_throttling_state
- Process information classes: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ne-processthreadsapi-process_information_class
- Process query: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocessinformation
- WCT ABI: https://learn.microsoft.com/en-us/windows/win32/api/wct/ns-wct-waitchain_node_info
- WCT overview: https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal
- Dump API: https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump

This is a candid feature inventory, not a statement that every stock quality-of-life feature has been completed.
