# Native Task Manager Coverage

Reviewed September 30, 2026 against Microsoft's documented Task Manager conveniences and the repository's archived managed release. This describes the **default C++ executable**, not the previous .NET implementation. Full stock parity is not claimed.

| Area | Native implementation | Remaining work / limits |
| --- | --- | --- |
| Process icons | Immediate placeholders; visible rows first; two extraction workers; bounded executable-path cache; independent enrichment worker; PID + creation identity; measured request-to-display | Protected processes, iconless executables and UNC paths retain placeholders; packaged-app logos and high-DPI icon variants need further work |
| Keyboard | Letter-to-next with wraparound; repeated-letter cycling; one-second prefix search; Ctrl+F / Alt+F; Escape; F5; Ctrl+C; Delete; Ctrl+Shift+N; Alt+1…7; Ctrl+Tab; Ctrl-to-freeze sampling | No exhaustive accelerator/accessibility comparison; hidden tests invoke control messages, not physical keyboard events |
| Tables | Virtual owner-data rows; stable identity/selection; numeric resource sorting; header resizing/reordering; optional column hiding via header context menu; remembered widths/sort | Column order is not persisted; not every stock Details column, resource-percent mode or heat-map implemented |
| Processes | Native inventory; visible-window app detection; direct-child app grouping; expansion; search by name/PID/publisher/description; Go to details | Full descendant/package/service-host grouping, end-tree and special Explorer restart are not ported |
| Process actions | End; Idle/Normal/Above-normal/High priority; affinity; Efficiency on/off; full process dump; off-UI wait-chain report; location; Properties; online search; copy | Normal access restrictions; no Realtime, UAC virtualization changes, kernel dump or processor-group affinity. No live destructive action was performed during verification |
| Performance | CPU/memory/disk/network/GPU counters; resource selector; double-buffered antialiased history; exact left-edge interpolation; observed startup duration; frozen paused history | CPU per-logical-processor view, arbitrary GPU engine selection, temperature, hardware/BIOS detail panels, summary/graph-only modes and stock energy estimator not ported |
| App history | Persistent local CPU deltas by executable name | Not SRUM; per-process network attribution/reset/all-process preference still missing |
| Startup | HKCU/HKLM Run keys; startup folders; enable/disable with current-value checks | No measured boot impact; publisher/shortcut resolution and 32-bit Run-key coverage incomplete |
| Users | WTS sessions; CPU/memory totals; expandable per-session processes; confirmed disconnect/sign-out; child process actions | No remote-session connection flow; live disconnect/sign-out not tested |
| Services | Native inventory; PID/display/status; off-UI start/stop/restart; Windows Services shortcut | Go-to-process context command not ported; live service mutations not tested |
| Window/QOL | Compact view; four refresh speeds; explicit F5; pause sleeps; minimized throttling; topmost; hide-to-tray/restore; saved size/tab; DPI-scaled controls/fonts | Full dark/light/system themes, per-monitor icon resampling and every stock dialog not implemented |
| Responsiveness | Coalesced latest-sample slot; no sampling during painting; bounded caches/histories; active-view updates; separate metadata/icon work; reusable native inventory buffer; static release CRT | Measurement excludes compositor, physical input devices and adversarially stalled local filesystem/account services |

## Action Safety

All process mutations reopen the selected PID and verify its creation timestamp. Critical/system/self mutations are refused. Windows permissions remain intact; there is no driver or privilege bypass. Affinity masks must be nonzero and inside the process's system mask. Efficiency changes use the OS power-throttling API and normal priority policy, with rollback if the priority change fails. Full dumps use a temporary file and an atomic replacement; the user is warned about private memory contents. Service stop/restart polling has a ten-second deadline and runs outside the UI thread. Session identity and startup value are checked again before mutation. Only one action worker runs at a time, avoiding retained completed-thread handles.

## Official Research

The web-search connector returned no content in this session. The following official pages were then fetched directly over HTTPS and returned HTTP 200; API documentation was used to validate virtual-list type search and executable icon extraction. They are reference evidence, not a claim of exhaustive Microsoft feature parity.

- Microsoft Task Manager filtering, Alt+F, theme and Efficiency-mode improvements, November 10, 2022: https://blogs.windows.com/windows-insider/2022/11/10/announcing-windows-11-insider-preview-build-22621-891-and-22623-891/
- Virtual-list search notification: https://learn.microsoft.com/en-us/windows/win32/controls/lvn-odfinditem
- Owner-data list controls: https://learn.microsoft.com/en-us/windows/win32/controls/list-view-controls-overview
- Executable icon extraction: https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-extracticonexw
- Power throttling: https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-process_power_throttling_state
- Wait-chain traversal: https://learn.microsoft.com/en-us/windows/win32/debug/wait-chain-traversal
- Memory dumps: https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump
