#include "app.hpp"
#include <commdlg.h>
#include <shlobj.h>
#include <windowsx.h>

namespace taskmgr {
struct RunInput { std::wstring text; bool elevated = false; };
static INT_PTR CALLBACK runProcedure(HWND dialog, UINT message, WPARAM word, LPARAM data) {
  auto input = reinterpret_cast<RunInput*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) { SetWindowLongPtrW(dialog, DWLP_USER, data); SendDlgItemMessageW(dialog, 1004, STM_SETICON, WPARAM(LoadIconW(nullptr, IDI_APPLICATION)), 0); if (IsUserAnAdmin()) { EnableWindow(GetDlgItem(dialog, 1003), FALSE); CheckDlgButton(dialog, 1003, BST_CHECKED); } return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == 1002) { std::vector<wchar_t> path(32768); OPENFILENAMEW file{sizeof(file)}; file.hwndOwner = dialog; file.lpstrFile = path.data(); file.nMaxFile = DWORD(path.size()); file.lpstrFilter = L"Programs\0*.exe;*.pif;*.com;*.bat;*.cmd\0All Files\0*.*\0"; file.lpstrTitle = L"Browse"; file.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR; if (GetOpenFileNameW(&file)) SetDlgItemTextW(dialog, 1001, (L"\"" + std::wstring(path.data()) + L"\"").c_str()); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == 1001 && HIWORD(word) == EN_CHANGE) { EnableWindow(GetDlgItem(dialog, IDOK), GetWindowTextLengthW(GetDlgItem(dialog, 1001)) > 0); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == IDOK) { std::vector<wchar_t> text(32768); GetDlgItemTextW(dialog, 1001, text.data(), int(text.size())); input->text = text.data(); input->elevated = IsDlgButtonChecked(dialog, 1003) == BST_CHECKED && !IsUserAnAdmin(); EndDialog(dialog, IDOK); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
  return FALSE;
}
struct AffinityInput { std::wstring name; DWORD_PTR mask = 0, system = 0; bool syncing = false; };
static INT_PTR CALLBACK affinityProcedure(HWND dialog, UINT message, WPARAM word, LPARAM data) {
  auto input = reinterpret_cast<AffinityInput*>(GetWindowLongPtrW(dialog, DWLP_USER)); const HWND processors = GetDlgItem(dialog, 1001);
  if (message == WM_INITDIALOG) {
    input = reinterpret_cast<AffinityInput*>(data); SetWindowLongPtrW(dialog, DWLP_USER, data); input->syncing = true;
    SetDlgItemTextW(dialog, 1002, (L"Which processors are allowed to run “" + input->name + L"”?").c_str());
    ListView_SetExtendedListViewStyle(processors, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT); RECT client{}; GetClientRect(processors, &client);
    LVCOLUMNW column{}; column.mask = LVCF_WIDTH; column.cx = client.right - GetSystemMetrics(SM_CXVSCROLL); ListView_InsertColumn(processors, 0, &column);
    auto add = [&](const std::wstring& text, LPARAM bit, bool checked) { LVITEMW item{}; item.mask = LVIF_TEXT | LVIF_PARAM; item.iItem = ListView_GetItemCount(processors); item.pszText = const_cast<wchar_t*>(text.c_str()); item.lParam = bit; const int index = ListView_InsertItem(processors, &item); ListView_SetCheckState(processors, index, checked); };
    add(L"<All Processors>", -1, (input->mask & input->system) == input->system);
    for (int bit = 0; bit < int(sizeof(DWORD_PTR) * 8); ++bit) if (input->system & (DWORD_PTR(1) << bit)) add(L"CPU " + std::to_wstring(bit), bit, (input->mask & (DWORD_PTR(1) << bit)) != 0);
    input->syncing = false; return TRUE;
  }
  if (message == WM_NOTIFY && reinterpret_cast<NMHDR*>(data)->idFrom == 1001 && reinterpret_cast<NMHDR*>(data)->code == LVN_ITEMCHANGED && input && !input->syncing) {
    const auto change = reinterpret_cast<NMLISTVIEW*>(data); if (!(change->uChanged & LVIF_STATE) || !((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK)) return FALSE;
    input->syncing = true; const int count = ListView_GetItemCount(processors);
    if (change->iItem == 0) { const bool all = ListView_GetCheckState(processors, 0); for (int index = 1; index < count; ++index) ListView_SetCheckState(processors, index, all); }
    else { bool all = true; for (int index = 1; index < count; ++index) all = all && ListView_GetCheckState(processors, index); ListView_SetCheckState(processors, 0, all); }
    input->syncing = false; return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(word) == IDOK) {
    DWORD_PTR mask = 0; for (int index = 1; index < ListView_GetItemCount(processors); ++index) if (ListView_GetCheckState(processors, index)) { LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = index; ListView_GetItem(processors, &item); mask |= DWORD_PTR(1) << item.lParam; }
    if (!mask) { MessageBoxW(dialog, L"The process must have affinity with at least one processor.", L"Task Manager", MB_OK | MB_ICONERROR); return TRUE; }
    input->mask = mask; EndDialog(dialog, IDOK); return TRUE;
  }
  if (message == WM_COMMAND && LOWORD(word) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
  return FALSE;
}
bool Application::confirm(const std::wstring& title, const std::wstring& text, const std::wstring& button) {
  if (!persistent()) return false;
  const TASKDIALOG_BUTTON buttons[] = {{IDOK, button.c_str()}, {IDCANCEL, L"Cancel"}};
  TASKDIALOGCONFIG config{sizeof(config)}; config.hwndParent = window; config.pszWindowTitle = L"Task Manager"; config.pszMainInstruction = title.c_str(); config.pszContent = text.c_str(); config.pButtons = buttons; config.cButtons = 2; config.nDefaultButton = IDCANCEL; config.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW;
  int pressed = 0; return SUCCEEDED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr)) && pressed == IDOK;
}
void Application::asynchronous(std::function<std::wstring()> operation) {
  if (!persistent()) return;
  if (actionBusy.exchange(true)) { MessageBoxW(window, L"An action is already in progress. Try again when it finishes.", L"Task Manager", MB_OK); return; }
  if (actionWorker.joinable()) actionWorker.join();
  actionWorker = std::thread([this, operation = std::move(operation)] {
    ActionResult result; const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try { result.report = operation(); } catch (const std::exception& error) { const auto text = error.what(); const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0); if (needed > 0) { result.error.resize(size_t(needed)); MultiByteToWideChar(CP_UTF8, 0, text, -1, result.error.data(), needed); result.error.resize(size_t(needed - 1)); } }
    if (SUCCEEDED(initialized)) CoUninitialize();
    { std::lock_guard lock(actionMutex); actionResults.push_back(std::move(result)); } actionBusy = false; if (!stopping) PostMessageW(window, ActionMessage, 0, 0);
  });
}
HWND Application::appWindow(const Process& process) const {
  struct Search { DWORD pid; HWND found = nullptr; } search{process.id};
  EnumWindows([](HWND candidate, LPARAM context) -> BOOL { auto& state = *reinterpret_cast<Search*>(context); DWORD pid = 0; GetWindowThreadProcessId(candidate, &pid); if (pid == state.pid && IsWindowVisible(candidate) && !GetWindow(candidate, GW_OWNER) && GetWindowTextLengthW(candidate) > 0) { state.found = candidate; return FALSE; } return TRUE; }, reinterpret_cast<LPARAM>(&search));
  return search.found;
}
void Application::runTask() {
  RunInput input;
  if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), window, runProcedure, LPARAM(&input)) != IDOK || input.text.empty()) return;
  std::wstring file, parameters;
  if (input.text.front() == L'"') { const auto close = input.text.find(L'"', 1); file = input.text.substr(1, close == input.text.npos ? input.text.npos : close - 1); if (close != input.text.npos) parameters = input.text.substr(close + 1); }
  else { const auto executable = commandExecutable(input.text); file = executable.empty() ? input.text : executable; parameters = input.text.size() > file.size() && lower(input.text).starts_with(lower(file)) ? input.text.substr(file.size()) : input.text.substr(std::min(input.text.size(), input.text.find(L' ') == input.text.npos ? input.text.size() : input.text.find(L' '))); }
  while (!parameters.empty() && parameters.front() == L' ') parameters.erase(parameters.begin());
  SHELLEXECUTEINFOW execute{sizeof(execute)}; execute.fMask = SEE_MASK_FLAG_NO_UI; execute.hwnd = window; execute.lpVerb = input.elevated ? L"runas" : nullptr; execute.lpFile = file.c_str(); execute.lpParameters = parameters.empty() ? nullptr : parameters.c_str(); execute.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&execute)) { const DWORD error = GetLastError(); if (error != ERROR_CANCELLED) MessageBoxW(window, (L"Windows cannot find '" + file + L"'. Make sure you typed the name correctly, and then try again.\n\n" + winerror(error)).c_str(), L"Create new task", MB_OK | MB_ICONERROR); }
}
void Application::endSelected(bool tree) {
  const auto row = selectedRow(); if (!row || !current) return;
  const auto members = selectedMembers(); if (members.empty()) return;
  const auto name = cell(*row, 0);
  if (selectedTab == DetailsTab) {
    if (tree ? !confirm(L"Do you want to end the process tree of “" + name + L"”?", L"If open programs or processes are associated with this process tree, they will close and you will lose any unsaved data. If you end a system process, it might result in system instability. Are you sure you want to continue?", L"End process tree")
             : !confirm(L"Do you want to end “" + name + L"”?", L"If an open program is associated with this process, it will close and you will lose any unsaved data. If you end a system process, it might result in system instability. Are you sure you want to continue?", L"End process")) return;
  } else if (row->category == 2 && !confirm(L"Do you want to end the system process “" + name + L"”?", L"Ending this process will cause Windows to become unusable or shut down, causing you to lose any unsaved data. Are you sure you want to continue?", L"Shut down")) return;
  std::vector<Process> targets;
  for (const auto member : members) {
    if (!tree) { targets.push_back(*member); continue; }
    for (const auto& identity : processTree(current->processes, *member)) for (const auto& process : current->processes) if (process.id == identity.pid && process.created == identity.created) targets.push_back(process);
  }
  asynchronous([targets] { std::wstring failures; for (const auto& process : targets) { try { processAction(process, Action::End); } catch (const std::exception& error) { if (failures.empty() || targets.size() == 1) failures = std::wstring(error.what(), error.what() + strlen(error.what())); } } if (!failures.empty()) throw std::runtime_error(utf8(L"Unable to terminate process.\n\n" + failures)); return std::wstring{}; });
}
void Application::command(int id) {
  if (tabQueued && pendingTab >= 0) { const int tab = pendingTab; pendingTab = -1; SendMessageW(tabs, WM_SETREDRAW, TRUE, 0); selectTab(tab); InvalidateRect(tabs, nullptr, FALSE); }
  const auto row = selectedRow(); const auto process = selectedProcess();
  const Entry* entry = nullptr;
  if (row && current && row->entry >= 0) { const auto& entries = selectedTab == StartupTab ? current->startup->entries : selectedTab == UsersTab ? current->sessions : current->services; if (size_t(row->entry) < entries.size()) entry = &entries[size_t(row->entry)]; }
  if (id >= ColumnId && id < ColumnId + 64) {
    const int column = id - ColumnId; const auto defs = definitions(selectedTab);
    if (column == 0 || size_t(column) >= defs.size() || compact || selectedTab == PerformanceTab) return;
    captureColumns(); auto& ids = shown[size_t(selectedTab)]; auto& sizes = widths[size_t(selectedTab)];
    const auto found = std::find(ids.begin(), ids.end(), column);
    if (found != ids.end()) { sizes.erase(sizes.begin() + (found - ids.begin())); ids.erase(found); } else { ids.push_back(column); sizes.push_back(defs[size_t(column)].width); }
    columns(true); saveSettings(); return;
  }
  if (id >= DefaultTabId && id < DefaultTabId + TabCount) { defaultTab = id - DefaultTabId; saveSettings(); return; }
  if (id >= ViewResource && id < ViewResource + 64) { const auto& items = perfItems(); if (size_t(id - ViewResource) < items.size() && selectedResource != items[size_t(id - ViewResource)].key) { selectedResource = items[size_t(id - ViewResource)].key; InvalidateRect(performance, nullptr, FALSE); } return; }
  switch (id) {
  case ThemeLightId: case ThemeDarkId: case ThemeSystemId: if (theme == id - ThemeLightId) return; theme = id - ThemeLightId; applyTheme(); saveSettings(); return;
  case SearchId: toggleSearch(); return;
  case ReplaceDefaultId: {
    const bool enable = !replacementEnabled();
    if (enable && !confirm(L"Replace Windows Task Manager?", L"Ctrl+Shift+Esc and Windows Task Manager launch commands will open this app for all Windows accounts. Windows will ask for administrator approval and install one protected executable in Program Files. Turn this option off to restore Windows Task Manager.", L"Replace Task Manager")) return;
    asynchronous([owner = window, enable] { requestReplacement(owner, enable); return std::wstring{}; }); return;
  }
  case ExitId: SendMessageW(window, WM_CLOSE, 0, 0); return;
  case RefreshId: refresh(); return;
  case TrayRestore: ShowWindow(window, IsIconic(window) ? SW_RESTORE : SW_SHOW); SetForegroundWindow(window); return;
  case FewerId: setCompact(!compact); return;
  case LinkId: ShellExecuteW(window, nullptr, selectedTab == StartupTab ? L"ms-settings:startupapps" : selectedTab == ServicesTab ? L"services.msc" : L"resmon.exe", nullptr, nullptr, SW_SHOWNORMAL); return;
  case HistoryLinkId: case DeleteHistory: usage.clear(); historySince = Clock::now(); saveHistory(); saveSettings(); rebuild(); layout(); return;
  case TopmostId: topmost = !topmost; SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); saveSettings(); return;
  case MinimizeOnUseId: minimizeOnUse = !minimizeOnUse; saveSettings(); return;
  case HideId: hideMinimized = !hideMinimized; saveSettings(); return;
  case GroupId: groupByType = !groupByType; rebuild(); saveSettings(); return;
  case FullNameId: fullName = !fullName; rebuild(); saveSettings(); return;
  case AllHistoryId: allHistory = !allHistory; rebuild(); saveSettings(); return;
  case ExpandAllId: { bool changed = false; for (const auto& item : rows) if (item.expandable) changed = expanded.insert(item.key).second || changed; if (changed) rebuild(); return; }
  case CollapseAllId: if (!expanded.empty()) { expanded.clear(); rebuild(); } return;
  case SpeedHigh: case SpeedNormal: case SpeedLow: case SpeedPause: { const int requested = id == SpeedHigh ? 500 : id == SpeedNormal ? 1000 : id == SpeedLow ? 4000 : 0; if (interval == requested) return; interval = requested; refresh(false); saveSettings(); return; }
  case MemoryValues: case MemoryPercents: if (memoryPercent == (id == MemoryPercents)) return; memoryPercent = id == MemoryPercents; rebuild(); saveSettings(); return;
  case NetworkValues: case NetworkPercents: if (networkPercent == (id == NetworkPercents)) return; networkPercent = id == NetworkPercents; rebuild(); saveSettings(); return;
  case SummaryView: setSummary(summary ? 0 : 1); return;
  case SidebarSummary: setSummary(summary ? 0 : 2); return;
  case OverallView: case LogicalView: if (logical == (id == LogicalView)) return; logical = id == LogicalView; InvalidateRect(performance, nullptr, FALSE); saveSettings(); return;
  case HideGraphs: hideGraphs = !hideGraphs; hotResource = -1; InvalidateRect(performance, nullptr, FALSE); saveSettings(); return;
  case CopyPerformance: case CopyId: copy(); return;
  case RunId: if (!hidden()) runTask(); return;
  case Toggle: toggle(selectedIndex()); return;
  default: break;
  }
  if (!persistent()) return;
  if (id == EndId) {
    if (compact || selectedTab == ProcessesTab) { if (row && row->kind == RowKind::Process && process && lower(process->name) == L"explorer.exe") id = RestartExplorer; else id = EndTask; }
    else if (selectedTab == StartupTab) id = StartupToggle;
    else if (selectedTab == UsersTab) id = row && row->kind == RowKind::User ? Disconnect : EndTask;
    else id = EndTask;
  }
  switch (id) {
  case EndTask: endSelected(false); return;
  case EfficiencyId: {
    if (!process || process->id <= 4 || process->id == GetCurrentProcessId() || !row || row->kind != RowKind::Process) return;
    try {
      const auto policy = powerPolicy(*process); const bool enabled = !(policy.control & policy.state & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
      if (enabled && !confirm(L"Turn on Efficiency mode?", L"This lowers the selected process priority and requests power-efficient scheduling. It can make that application less responsive. It does not limit CPU usage. Child processes are not changed.", L"Turn on Efficiency mode")) return;
      const auto target = *process; asynchronous([target, enabled] { efficiencyAction(target, enabled); return std::wstring{}; });
    } catch (const std::exception& error) {
      const auto message = error.what(); const int length = MultiByteToWideChar(CP_UTF8, 0, message, -1, nullptr, 0); std::wstring text(size_t(std::max(1, length)), L'\0');
      if (length > 0) MultiByteToWideChar(CP_UTF8, 0, message, -1, text.data(), length);
      MessageBoxW(window, text.c_str(), L"Efficiency mode unavailable", MB_OK | MB_ICONERROR);
    }
    return;
  }
  case EndTree: endSelected(true); return;
  case RestartExplorer: {
    if (!process) return; const auto target = *process;
    asynchronous([target] { processAction(target, Action::End); Sleep(1000); bool running = false; HWND shell = FindWindowW(L"Shell_TrayWnd", nullptr); running = shell != nullptr; if (!running) ShellExecuteW(nullptr, nullptr, (windowsDirectory() + L"\\explorer.exe").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return std::wstring{}; });
    return;
  }
  case SwitchTo: case BringToFront: case MinimizeWindow: case MaximizeWindow: {
    HWND target = row && row->kind == RowKind::Window && IsWindow(row->window) ? row->window : nullptr;
    if (!target) for (const auto member : selectedMembers()) if ((target = appWindow(*member)) != nullptr) break;
    if (!target) return;
    if (id == MinimizeWindow) { ShowWindow(target, SW_MINIMIZE); return; }
    if (id == MaximizeWindow) { ShowWindow(target, SW_MAXIMIZE); return; }
    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SwitchToThisWindow(target, TRUE); SetForegroundWindow(target);
    if (id == SwitchTo && minimizeOnUse && !compact) ShowWindow(window, SW_MINIMIZE);
    return;
  }
  case GoToDetails: {
    DWORD pid = process ? process->id : entry ? entry->pid : 0; if (!pid) return;
    SetWindowTextW(searchBox, L""); applySearch();
    selectTab(DetailsTab); for (size_t index = 0; index < rows.size(); ++index) if (rows[index].process >= 0 && current->processes[size_t(rows[index].process)].id == pid) { select(int(index)); break; }
    SetFocus(list); return;
  }
  case GoToServices: {
    if (!process) return; const DWORD pid = process->id; SetWindowTextW(searchBox, L""); applySearch(); selectTab(ServicesTab);
    for (size_t index = 0; index < rows.size(); ++index) if (rows[index].entry >= 0 && current->services[size_t(rows[index].entry)].pid == pid) { select(int(index)); break; }
    SetFocus(list); return;
  }
  case OpenLocation: case Properties: {
    std::wstring path = process ? icons->get(*process).path : entry && selectedTab == StartupTab ? entry->path : L"";
    if (selectedTab == HistoryTab && row) { const auto found = usage.find(row->key); if (found != usage.end()) path = found->second.path; }
    if (path.empty()) return;
    if (id == OpenLocation) { ShellExecuteW(window, nullptr, L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL); return; }
    SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_INVOKEIDLIST; info.hwnd = window; info.lpVerb = L"properties"; info.lpFile = path.c_str(); info.nShow = SW_SHOWNORMAL; ShellExecuteExW(&info); return;
  }
  case SearchOnline: {
    std::wstring query;
    if (row && row->kind == RowKind::Service && selectedTab == ProcessesTab) query = row->cells[0].text;
    else if (process) { const auto& metadata = icons->get(*process); query = process->name + (metadata.description.empty() ? L"" : L" " + metadata.description); }
    else if (entry) query = selectedTab == ServicesTab ? entry->key + L" " + entry->description : entry->cells[0];
    else if (row) query = cell(*row, 0);
    std::wstring escaped; for (const auto character : utf8(query)) { if (isalnum(static_cast<unsigned char>(character)) || character == '.' || character == '-') escaped += wchar_t(character); else { wchar_t code[8]{}; swprintf_s(code, L"%%%02X", unsigned(static_cast<unsigned char>(character))); escaped += code; } }
    ShellExecuteW(window, nullptr, (L"https://www.bing.com/search?q=" + escaped).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
  }
  case CreateDump: {
    if (!process) return; const auto target = *process;
    wchar_t temporary[MAX_PATH]{}; GetTempPathW(MAX_PATH, temporary); std::wstring stem = target.name; if (lower(stem).ends_with(L".exe")) stem.resize(stem.size() - 4);
    const auto path = std::wstring(temporary) + stem + L".DMP";
    asynchronous([target, path] { processAction(target, Action::Dump, path); return L"The file has been successfully created.\r\n\r\n" + path + L"\r\n\r\nMemory dumps may contain passwords and private data; delete it when you are finished."; });
    return;
  }
  case WaitChainId: { if (!process) return; const auto target = *process; asynchronous([target] { return waitChain(target); }); return; }
  case AffinityId: {
    if (!process) return;
    Handle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process->id)); AffinityInput input{process->name};
    if (!handle.value || !GetProcessAffinityMask(handle.value, &input.mask, &input.system)) { MessageBoxW(window, (L"Unable to access or set process affinity.\n\n" + winerror()).c_str(), L"Task Manager", MB_OK | MB_ICONERROR); return; }
    if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), window, affinityProcedure, LPARAM(&input)) != IDOK) return;
    const auto target = *process; const auto argument = hexadecimal(input.mask);
    asynchronous([target, argument] { processAction(target, Action::Affinity, argument); return std::wstring{}; }); return;
  }
  case PriorityRealtime: case PriorityHigh: case PriorityAbove: case PriorityNormal: case PriorityBelow: case PriorityLow: {
    if (!process) return;
    if (!confirm(L"Do you want to change the priority of “" + process->name + L"”?", L"Changing the priority of certain processes could cause system instability.", L"Change priority")) return;
    static constexpr Action choices[] = {Action::Realtime, Action::High, Action::AboveNormal, Action::Normal, Action::BelowNormal, Action::Idle};
    const auto target = *process; const auto action = choices[id - PriorityRealtime]; asynchronous([target, action] { processAction(target, action); return std::wstring{}; }); return;
  }
  case ServiceStart: case ServiceStop: case ServiceRestart: {
    const Entry* service = entry; if (!service && row && row->kind == RowKind::Service && current) { const auto key = row->key.substr(2); for (const auto& item : current->services) if (item.key == key) service = &item; }
    if (!service) return; const auto target = *service; const int action = id - ServiceStart; asynchronous([target, action] { serviceAction(target, action); return std::wstring{}; }); return;
  }
  case OpenServices: ShellExecuteW(window, nullptr, L"services.msc", nullptr, nullptr, SW_SHOWNORMAL); return;
  case StartupToggle: { if (!entry || selectedTab != StartupTab || !entry->startupEditable) return; const auto target = *entry; asynchronous([target] { startupAction(target); return std::wstring{}; }); return; }
  case StartupManageId: {
    if (!entry || selectedTab != StartupTab || entry->cells.size() <= 4) return;
    const auto& kind = entry->cells[4];
    const wchar_t* target = kind == L"Packaged app" ? L"ms-settings:startupapps" : kind == L"Scheduled task" || kind == L"COM task" ? L"taskschd.msc" : kind == L"Service" ? L"services.msc" : nullptr;
    if (target) ShellExecuteW(window, nullptr, target, nullptr, nullptr, SW_SHOWNORMAL);
    return;
  }
  case Disconnect: case SignOff: {
    if (!entry || selectedTab != UsersTab) return;
    if (!confirm(id == SignOff ? L"Are you sure you want to sign off " + entry->cells[0] + L"?" : L"Are you sure you want to disconnect " + entry->cells[0] + L"?", id == SignOff ? L"Any unsaved data from this user's open programs might be lost." : L"The user's session will remain signed in, and their programs will keep running.", id == SignOff ? L"Sign off user" : L"Disconnect user")) return;
    const auto target = *entry; const bool logoff = id == SignOff; asynchronous([target, logoff] { sessionAction(target, logoff); return std::wstring{}; }); return;
  }
  case ManageAccounts: ShellExecuteW(window, nullptr, L"control.exe", L"/name Microsoft.UserAccounts", nullptr, SW_SHOWNORMAL); return;
  default: return;
  }
}
void Application::menu(POINT point) {
  if (hidden()) return;
  const auto row = selectedRow(); if (!row || row->kind == RowKind::Heading) return;
  HMENU popup = CreatePopupMenu(); std::vector<HMENU> children;
  auto add = [&](HMENU target, int id, const std::wstring& text, bool enabled = true, bool checked = false) { AppendMenuW(target, MF_STRING | (enabled ? 0 : MF_GRAYED) | (checked ? MF_CHECKED : 0), UINT_PTR(id), text.c_str()); };
  auto separator = [&](HMENU target) { AppendMenuW(target, MF_SEPARATOR, 0, nullptr); };
  auto submenu = [&](HMENU target, const std::wstring& text) { HMENU child = CreatePopupMenu(); children.push_back(child); AppendMenuW(target, MF_POPUP, reinterpret_cast<UINT_PTR>(child), text.c_str()); return child; };
  const auto process = selectedProcess(); const auto members = selectedMembers();
  HWND appTarget = row->kind == RowKind::Window ? row->window : nullptr; if (!appTarget) for (const auto member : members) if (member->app && (appTarget = appWindow(*member)) != nullptr) break;
  const bool path = process && !icons->get(*process).path.empty();
  if ((selectedTab == ProcessesTab || selectedTab == DetailsTab) && row->kind == RowKind::Process && process && process->id > 4 && process->id != GetCurrentProcessId()) {
    try { const auto policy = powerPolicy(*process); add(popup, EfficiencyId, L"&Efficiency mode", true, (policy.control & policy.state & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0); }
    catch (...) { add(popup, EfficiencyId, L"&Efficiency mode", false); }
    separator(popup);
  }
  int defaultItem = -1;
  if (compact) {
    add(popup, SwitchTo, L"&Switch to", appTarget != nullptr); add(popup, EndTask, L"&End task"); separator(popup); add(popup, RunId, L"&Run new task"); add(popup, TopmostId, L"&Always on top", true, topmost); separator(popup);
    add(popup, OpenLocation, L"&Open file location", path); add(popup, SearchOnline, L"S&earch online"); add(popup, Properties, L"Propert&ies", path); defaultItem = SwitchTo;
  } else if (selectedTab == ProcessesTab && row->kind == RowKind::Service) {
    add(popup, ServiceStart, L"&Start"); add(popup, ServiceStop, L"S&top"); add(popup, ServiceRestart, L"&Restart"); separator(popup); add(popup, OpenServices, L"Open Ser&vices"); add(popup, SearchOnline, L"S&earch online"); add(popup, GoToDetails, L"&Go to details");
  } else if (selectedTab == ProcessesTab || (selectedTab == UsersTab && row->kind != RowKind::User)) {
    if (row->expandable) { add(popup, Toggle, row->expanded ? L"Colla&pse" : L"Ex&pand"); defaultItem = Toggle; }
    if (appTarget) { add(popup, SwitchTo, L"&Switch to"); add(popup, BringToFront, L"&Bring to front"); add(popup, MinimizeWindow, L"&Minimize"); add(popup, MaximizeWindow, L"Ma&ximize"); if (defaultItem < 0) defaultItem = SwitchTo; }
    if (row->kind == RowKind::Process && process && lower(process->name) == L"explorer.exe") add(popup, RestartExplorer, L"&Restart");
    add(popup, EndTask, L"&End task", row->kind != RowKind::Interrupts);
    if (selectedTab == ProcessesTab) {
      separator(popup); const auto values = submenu(popup, L"Resource &values"); const auto memoryMenu = submenu(values, L"&Memory"), networkMenu = submenu(values, L"&Network");
      add(memoryMenu, MemoryValues, L"&Values"); add(memoryMenu, MemoryPercents, L"&Percents"); CheckMenuRadioItem(memoryMenu, MemoryValues, MemoryPercents, memoryPercent ? MemoryPercents : MemoryValues, MF_BYCOMMAND);
      add(networkMenu, NetworkValues, L"&Values"); add(networkMenu, NetworkPercents, L"&Percents"); CheckMenuRadioItem(networkMenu, NetworkValues, NetworkPercents, networkPercent ? NetworkPercents : NetworkValues, MF_BYCOMMAND);
    }
    separator(popup); add(popup, CreateDump, L"Create &dump file", process && row->kind != RowKind::Interrupts); separator(popup);
    add(popup, GoToDetails, L"&Go to details", process != nullptr); add(popup, OpenLocation, L"&Open file location", path); add(popup, SearchOnline, L"S&earch online"); add(popup, Properties, L"Propert&ies", path);
  } else if (selectedTab == UsersTab) {
    add(popup, Toggle, row->expanded ? L"Colla&pse" : L"Ex&pand"); add(popup, Disconnect, L"&Disconnect"); add(popup, SignOff, L"Sign o&ff"); separator(popup); add(popup, ManageAccounts, L"&Manage user accounts"); defaultItem = Toggle;
  } else if (selectedTab == DetailsTab) {
    add(popup, EndTask, L"&End task"); add(popup, EndTree, L"End process &tree"); separator(popup);
    const auto priority = submenu(popup, L"Set &priority"); add(priority, PriorityRealtime, L"&Realtime"); add(priority, PriorityHigh, L"&High"); add(priority, PriorityAbove, L"&Above normal"); add(priority, PriorityNormal, L"&Normal"); add(priority, PriorityBelow, L"&Below normal"); add(priority, PriorityLow, L"&Low");
    if (process) { const LONG base = process->priority; const int current_ = base >= 24 ? PriorityRealtime : base >= 13 ? PriorityHigh : base >= 10 ? PriorityAbove : base >= 8 ? PriorityNormal : base >= 6 ? PriorityBelow : PriorityLow; CheckMenuRadioItem(priority, PriorityRealtime, PriorityLow, UINT(current_), MF_BYCOMMAND); }
    add(popup, AffinityId, L"Set a&ffinity"); separator(popup); add(popup, WaitChainId, L"Analyze &wait chain"); add(popup, CreateDump, L"Create &dump file"); separator(popup);
    add(popup, OpenLocation, L"&Open file location", path); add(popup, SearchOnline, L"S&earch online"); add(popup, Properties, L"Propert&ies", path);
    const bool hosts = process && current && std::any_of(current->services.begin(), current->services.end(), [&](const Entry& service) { return service.pid == process->id; });
    add(popup, GoToServices, L"&Go to service(s)", hosts);
  } else if (selectedTab == ServicesTab) {
    const auto& service = current->services[size_t(row->entry)]; const bool running = service.cells[3] == L"Running", stopped = service.cells[3] == L"Stopped";
    add(popup, ServiceStart, L"&Start", stopped); add(popup, ServiceStop, L"S&top", running); add(popup, ServiceRestart, L"&Restart", running); separator(popup);
    add(popup, OpenServices, L"Open Ser&vices"); add(popup, SearchOnline, L"S&earch online"); add(popup, GoToDetails, L"&Go to details", service.pid != 0);
  } else if (selectedTab == StartupTab) {
    const auto& item = current->startup->entries[size_t(row->entry)]; add(popup, StartupToggle, item.enabled ? L"&Disable" : L"&Enable", item.startupEditable);
    if (item.cells.size() > 4 && (item.cells[4] == L"Packaged app" || item.cells[4] == L"Scheduled task" || item.cells[4] == L"COM task" || item.cells[4] == L"Service")) add(popup, StartupManageId, L"Manage in Windows");
    separator(popup);
    add(popup, OpenLocation, L"&Open file location", !item.path.empty()); add(popup, SearchOnline, L"S&earch online", item.cells[4] != L"Coverage"); add(popup, Properties, L"Propert&ies", !item.path.empty()); separator(popup); add(popup, CopyId, L"&Copy");
  } else if (selectedTab == HistoryTab) {
    const auto found = usage.find(row->key); const bool known = found != usage.end() && !found->second.path.empty();
    add(popup, SearchOnline, L"S&earch online"); add(popup, OpenLocation, L"&Open file location", known); add(popup, Properties, L"Propert&ies", known); separator(popup); add(popup, DeleteHistory, L"&Delete usage history");
  }
  if (defaultItem >= 0) SetMenuDefaultItem(popup, UINT(defaultItem), FALSE);
  if (GetMenuItemCount(popup) > 0) TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
  DestroyMenu(popup);
}
HMENU Application::createHeaderMenu() const {
  HMENU popup = CreatePopupMenu(); const auto defs = definitions(selectedTab); const auto& ids = shown[size_t(selectedTab)];
  if (!popup) return nullptr;
  for (size_t column = 0; column < defs.size(); ++column) AppendMenuW(popup, MF_STRING | (std::find(ids.begin(), ids.end(), int(column)) != ids.end() ? MF_CHECKED : 0) | (column == 0 ? MF_GRAYED : 0), UINT_PTR(ColumnId + column), defs[column].title);
  HMENU values = nullptr, memoryMenu = nullptr, networkMenu = nullptr;
  if (selectedTab == ProcessesTab) {
    AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); values = CreatePopupMenu(); memoryMenu = CreatePopupMenu(); networkMenu = CreatePopupMenu();
    AppendMenuW(memoryMenu, MF_STRING, MemoryValues, L"&Values"); AppendMenuW(memoryMenu, MF_STRING, MemoryPercents, L"&Percents"); CheckMenuRadioItem(memoryMenu, MemoryValues, MemoryPercents, memoryPercent ? MemoryPercents : MemoryValues, MF_BYCOMMAND);
    AppendMenuW(networkMenu, MF_STRING, NetworkValues, L"&Values"); AppendMenuW(networkMenu, MF_STRING, NetworkPercents, L"&Percents"); CheckMenuRadioItem(networkMenu, NetworkValues, NetworkPercents, networkPercent ? NetworkPercents : NetworkValues, MF_BYCOMMAND);
    AppendMenuW(values, MF_POPUP, reinterpret_cast<UINT_PTR>(memoryMenu), L"&Memory"); AppendMenuW(values, MF_POPUP, reinterpret_cast<UINT_PTR>(networkMenu), L"&Network"); AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(values), L"Resource &values");
  }
  return popup;
}
void Application::headerMenu(POINT point) {
#ifdef TASKMGR_DIAGNOSTICS
  ++headerMenuRequests;
#endif
  if (hidden() || compact || selectedTab == PerformanceTab) return;
  HMENU popup = createHeaderMenu();
  if (!popup) return;
  const int selected = int(TrackPopupMenu(popup, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window, nullptr));
  DestroyMenu(popup);
  if (selected) command(selected);
}
void Application::performanceMenu(POINT point, bool sidebar) {
  if (hidden()) return;
  HMENU popup = CreatePopupMenu(), view = nullptr, change = nullptr; const auto& items = perfItems();
  if (sidebar) { AppendMenuW(popup, MF_STRING, HideGraphs, hideGraphs ? L"&Show graphs" : L"&Hide graphs"); AppendMenuW(popup, MF_STRING | (summary == 2 ? MF_CHECKED : 0), SidebarSummary, L"S&ummary view"); }
  else {
    AppendMenuW(popup, MF_STRING | (summary == 1 ? MF_CHECKED : 0), SummaryView, L"Graph &summary view");
    view = CreatePopupMenu(); int selected = -1; for (size_t index = 0; index < items.size() && index < 64; ++index) { AppendMenuW(view, MF_STRING, UINT_PTR(ViewResource + index), items[index].title.c_str()); if (items[index].key == selectedResource) selected = int(index); }
    if (selected >= 0) CheckMenuRadioItem(view, ViewResource, UINT(ViewResource + items.size() - 1), UINT(ViewResource + selected), MF_BYCOMMAND);
    AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    if (selectedResource == L"cpu") { change = CreatePopupMenu(); AppendMenuW(change, MF_STRING, OverallView, L"&Overall utilization"); AppendMenuW(change, MF_STRING, LogicalView, L"&Logical processors"); CheckMenuRadioItem(change, OverallView, LogicalView, logical ? LogicalView : OverallView, MF_BYCOMMAND); AppendMenuW(popup, MF_POPUP, reinterpret_cast<UINT_PTR>(change), L"&Change graph to"); }
  }
  AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); AppendMenuW(popup, MF_STRING, CopyPerformance, L"&Copy\tCtrl+C");
  TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr); DestroyMenu(popup);
}
}
