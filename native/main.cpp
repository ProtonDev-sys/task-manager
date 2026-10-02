#include "app.hpp"
#ifdef TASKMGR_DIAGNOSTICS
#include "report.hpp"
#endif
#include <commdlg.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <windowsx.h>

namespace taskmgr {
static const wchar_t* tabNames[] = {L"Processes", L"Performance", L"App history", L"Startup", L"Users", L"Details", L"Services"};
#ifdef TASKMGR_DIAGNOSTICS
Application::Application(RunOptions value) : options(std::move(value)) {}
#endif
Application::~Application() {
  stopping = true; wake.notify_all(); if (sampler.joinable()) sampler.join();
#ifdef TASKMGR_DIAGNOSTICS
  if (inputProbe.joinable()) inputProbe.join();
#endif
  if (actionWorker.joinable()) actionWorker.join();
  icons.reset();
  for (auto object : {font, percentFont, headingFont, titleFont, subtitleFont, labelFont, valueFont, sidebarFont, linkFont}) if (object) DeleteObject(object);
  if (rowSpacer) ImageList_Destroy(rowSpacer);
  if (backgroundBrush) DeleteObject(backgroundBrush);
  for (auto icon : {windowIcon, smallIcon, trayIcon}) if (icon) DestroyIcon(icon);
}
int Application::run() {
  const auto instance = GetModuleHandleW(nullptr);
  WNDCLASSEXW klass{sizeof(klass)}; klass.lpfnWndProc = procedure; klass.hInstance = instance; klass.hCursor = LoadCursorW(nullptr, IDC_ARROW); klass.lpszClassName = L"TaskManagerNative"; klass.hbrBackground = GetStockBrush(WHITE_BRUSH);
  const auto taskmgr = windowsDirectory() + L"\\System32\\Taskmgr.exe";
  if (FAILED(SHDefExtractIconW(taskmgr.c_str(), 0, 0, &windowIcon, &smallIcon, MAKELONG(GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CXSMICON))))) { windowIcon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION)); smallIcon = CopyIcon(windowIcon); }
  klass.hIcon = windowIcon; klass.hIconSm = smallIcon; RegisterClassExW(&klass);
  klass.lpfnWndProc = performanceProcedure; klass.lpszClassName = L"TaskManagerPerformance"; klass.hIcon = klass.hIconSm = nullptr; klass.style = CS_DBLCLKS; RegisterClassExW(&klass);
  wchar_t local[MAX_PATH]{}; SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
  const std::filesystem::path directory = std::filesystem::path(local) / L"TaskManagerNative";
  if (persistent()) { std::error_code error; std::filesystem::create_directories(directory, error); preferences = (directory / L"settings.ini").wstring(); historyFile = (directory / L"history.tsv").wstring(); }
  loadSettings();
#ifdef TASKMGR_DIAGNOSTICS
  if (options.theme >= 0) theme = options.theme;
  if (hidden()) groupByType = false;
  if (options.interval >= 0) interval = options.interval;
#endif
  if (persistent()) loadHistory();
  if (historySince == Time{}) historySince = Clock::now();
#ifdef TASKMGR_DIAGNOSTICS
  selectedTab = options.tab >= 0 && !hidden() ? options.tab : defaultTab;
#else
  selectedTab = defaultTab;
#endif
  startupVisible = selectedTab == StartupTab && !compact;
  startupRequested = startupVisible;
#ifdef TASKMGR_DIAGNOSTICS
  const bool screenshots = !options.screenshots.empty();
#else
  constexpr bool screenshots = false;
#endif
  const bool renderOffscreen = screenshots || hidden();
  int width = normalRect.right - normalRect.left, height = normalRect.bottom - normalRect.top, x = normalRect.left, y = normalRect.top;
  if (width <= 0 || height <= 0 || !persistent()) { width = 1010; height = 875; x = y = CW_USEDEFAULT; }
  // Diagnostic runs use a fully transparent, click-through, non-activating window owned by a hidden window (so no taskbar button):
  // Windows still paints and composes it, but nothing appears on the desktop.
  HWND owner = nullptr;
  if (renderOffscreen) { x = GetSystemMetrics(SM_XVIRTUALSCREEN); y = GetSystemMetrics(SM_YVIRTUALSCREEN); owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr); }
  window = CreateWindowExW((topmost ? WS_EX_TOPMOST : 0) | (renderOffscreen ? WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT : 0), L"TaskManagerNative", L"Task Manager", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, width, height, owner, nullptr, instance, this);
  if (renderOffscreen) SetLayeredWindowAttributes(window, 0, 0, LWA_ALPHA);
  if (!window) return 1;
  if (renderOffscreen) { SetWindowPos(window, nullptr, x, y, MulDiv(width, int(GetDpiForWindow(window)), 96), MulDiv(height, int(GetDpiForWindow(window)), 96), SWP_NOZORDER | SWP_NOACTIVATE); ShowWindow(window, SW_SHOWNOACTIVATE); }
  else if (!hidden()) { if (persistent() && x != CW_USEDEFAULT) { WINDOWPLACEMENT placement{sizeof(placement)}; placement.rcNormalPosition = normalRect; placement.showCmd = GetPrivateProfileIntW(L"Window", L"Maximized", 0, preferences.c_str()) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL; SetWindowPlacement(window, &placement); } else ShowWindow(window, SW_SHOW); }
  applyTheme();
  if (compact && !hidden()) setCompact(true);
#ifdef TASKMGR_DIAGNOSTICS
  minimized = options.minimized;
  FILETIME created{}, exited{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &cpuStartKernel, &cpuStartUser);
#endif
  sampler = std::thread([this] { worker(); }); SetTimer(window, 1, hidden() ? 100 : screenshots ? 250 : 1000, nullptr);
  if (persistent()) PostMessageW(window, WM_APP + 11, 0, 0);
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden()) inputProbe = std::thread([this] { while (!stopping) { LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); PostMessageW(window, ProbeMessage, WPARAM(counter.QuadPart), 0); Sleep(50); } });
#endif
  MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (sortQueued && Clock::now() >= sortDue) { rebuild(true); InvalidateRect(header, nullptr, FALSE); }
    const bool control = GetKeyState(VK_CONTROL) & 0x8000, shift = GetKeyState(VK_SHIFT) & 0x8000, alt = GetKeyState(VK_MENU) & 0x8000;
    if (message.message == WM_KEYDOWN && message.wParam == VK_F5) { refresh(); continue; }
    if (message.message == WM_KEYDOWN && control && message.wParam == 'F') { command(SearchId); continue; }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && searchVisible && !compact && !summary && selectedTab != PerformanceTab) { SetFocus(searchBox); toggleSearch(); continue; }
    if (message.message == WM_KEYDOWN && control && message.wParam == VK_TAB && !compact && !summary) { requestTab(((pendingTab >= 0 ? pendingTab : TabCtrl_GetCurSel(tabs)) + (shift ? TabCount - 1 : 1)) % TabCount); continue; }
    if (message.message == WM_SYSKEYDOWN && alt && message.wParam >= '1' && message.wParam <= '7' && !compact && !summary) { requestTab(int(message.wParam - '1')); continue; }
    if (message.message == WM_KEYDOWN && control && shift && message.wParam == 'N') { command(RunId); continue; }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && summary) { setSummary(0); continue; }
    if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
  }
  if (owner) DestroyWindow(owner);
  return exitCode ? exitCode : int(message.wParam);
}
void Application::createFonts() {
  for (auto object : {font, percentFont, headingFont, titleFont, subtitleFont, labelFont, valueFont, sidebarFont, linkFont}) if (object) DeleteObject(object);
  auto make = [&](int size, bool underline = false) { return CreateFontW(-scale(size), 0, 0, 0, FW_NORMAL, FALSE, underline, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"); };
  font = make(12); percentFont = make(16); headingFont = make(16); titleFont = make(28); subtitleFont = make(15); labelFont = make(12); valueFont = make(24); sidebarFont = make(16); linkFont = make(12, true);
  for (const auto control : {tabs, list, end, fewer, link, historyLink, searchBox}) if (control) SendMessageW(control, WM_SETFONT, WPARAM(font), TRUE);
}
void Application::create() {
  createFonts();
  const auto instance = GetModuleHandleW(nullptr);
  auto control = [&](const wchar_t* klass, const wchar_t* title, DWORD style, int id, DWORD extra = 0) { auto created = CreateWindowExW(extra, klass, title, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr); SendMessageW(created, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return created; };
  tabs = control(WC_TABCONTROLW, L"", WS_TABSTOP | TCS_FOCUSNEVER, TabsId);
  SetWindowSubclass(tabs, tabsProcedure, 4, reinterpret_cast<DWORD_PTR>(this));
  for (int index = 0; index < TabCount; ++index) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tabNames[index]); TabCtrl_InsertItem(tabs, index, &item); }
  TabCtrl_SetCurSel(tabs, selectedTab);
  list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, ListId);
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP);
  SetWindowSubclass(list, listProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  header = ListView_GetHeader(list); SetWindowSubclass(header, headerProcedure, 2, reinterpret_cast<DWORD_PTR>(this));
  SetWindowLongPtrW(header, GWL_STYLE, GetWindowLongPtrW(header, GWL_STYLE) | HDS_FULLDRAG);
  performance = CreateWindowExW(0, L"TaskManagerPerformance", L"", WS_CHILD | WS_TABSTOP | WS_CLIPSIBLINGS, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(PerformanceId)), instance, this);
  fewer = control(L"BUTTON", L"Fewer details", BS_OWNERDRAW | WS_TABSTOP, FewerId);
  link = control(L"BUTTON", L"Open Resource Monitor", BS_OWNERDRAW | WS_TABSTOP, LinkId);
  historyLink = control(L"BUTTON", L"Delete usage history", BS_OWNERDRAW | WS_TABSTOP, HistoryLinkId);
  for (const auto button : {fewer, link, historyLink}) SetWindowSubclass(button, linkProcedure, 3, reinterpret_cast<DWORD_PTR>(this));
  end = control(L"BUTTON", L"End task", BS_PUSHBUTTON | WS_TABSTOP, EndId);
  SetWindowSubclass(end, linkProcedure, 3, reinterpret_cast<DWORD_PTR>(this));
  searchBox = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, SearchId, WS_EX_CLIENTEDGE);
  SendMessageW(searchBox, EM_SETLIMITTEXT, 256, 0);
  SendMessageW(searchBox, EM_SETCUEBANNER, TRUE, LPARAM(L"Search name, PID, publisher or command line"));
#ifdef TASKMGR_DIAGNOSTICS
  icons = std::make_unique<Icons>(window, scale(16), hidden());
#else
  icons = std::make_unique<Icons>(window, scale(16));
#endif
  menuBar = CreateMenu(); const auto file = CreatePopupMenu(), options_ = CreatePopupMenu(), view = CreatePopupMenu(), speed = CreatePopupMenu(), defaults = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, RunId, L"&Run new task"); AppendMenuW(file, MF_SEPARATOR, 0, nullptr); AppendMenuW(file, MF_STRING, ExitId, L"E&xit");
  AppendMenuW(options_, MF_STRING, TopmostId, L"&Always on top"); AppendMenuW(options_, MF_STRING, MinimizeOnUseId, L"&Minimize on use"); AppendMenuW(options_, MF_STRING, HideId, L"&Hide when minimized");
  AppendMenuW(options_, MF_STRING, ReplaceDefaultId, L"&Replace default Task Manager");
  const auto themes = CreatePopupMenu();
  AppendMenuW(themes, MF_STRING, ThemeLightId, L"&Light"); AppendMenuW(themes, MF_STRING, ThemeDarkId, L"&Dark"); AppendMenuW(themes, MF_STRING, ThemeSystemId, L"Use &Windows setting");
  AppendMenuW(options_, MF_POPUP, reinterpret_cast<UINT_PTR>(themes), L"&Theme");
  for (int index = 0; index < TabCount; ++index) AppendMenuW(defaults, MF_STRING, UINT_PTR(DefaultTabId + index), tabNames[index]);
  AppendMenuW(options_, MF_POPUP, reinterpret_cast<UINT_PTR>(defaults), L"Set &default tab"); AppendMenuW(options_, MF_SEPARATOR, 0, nullptr); AppendMenuW(options_, MF_STRING, FullNameId, L"Show &full account name"); AppendMenuW(options_, MF_STRING, AllHistoryId, L"Show history for all &processes");
  AppendMenuW(speed, MF_STRING, SpeedHigh, L"&High"); AppendMenuW(speed, MF_STRING, SpeedNormal, L"&Normal"); AppendMenuW(speed, MF_STRING, SpeedLow, L"&Low"); AppendMenuW(speed, MF_STRING, SpeedPause, L"&Paused");
  AppendMenuW(view, MF_STRING, RefreshId, L"&Refresh now\tF5"); AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"&Update speed"); AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(view, MF_STRING, SearchId, L"&Search\tCtrl+F");
  AppendMenuW(view, MF_STRING, GroupId, L"&Group by type"); AppendMenuW(view, MF_STRING, ExpandAllId, L"&Expand all"); AppendMenuW(view, MF_STRING, CollapseAllId, L"&Collapse all");
  AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File"); AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(options_), L"&Options"); AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
  SetMenu(window, menuBar);
  tray.hWnd = window; tray.uID = 1; tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray.uCallbackMessage = TrayMessage; tray.hIcon = smallIcon; wcscpy_s(tray.szTip, L"Task Manager");
  if (persistent()) trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
  applyTheme(); columns(); layout(); updateFooter();
}
void Application::layout() {
  auto show = [](HWND control, bool visible) { if (((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible) ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE); };
  auto place = [&](HWND control, int left, int top, int width, int height) {
    RECT bounds{}; GetWindowRect(control, &bounds); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    if (bounds.left != left || bounds.top != top || bounds.right - bounds.left != width || bounds.bottom - bounds.top != height) {
      MoveWindow(control, left, top, width, height, FALSE); InvalidateRect(control, nullptr, FALSE);
    }
  };
  RECT area{}; GetClientRect(window, &area); const int width = area.right, height = area.bottom;
  const int footer = scale(50);
  if (summary) {
    for (const auto control : {tabs, list, end, fewer, link, historyLink, searchBox}) show(control, false);
    show(performance, true); place(performance, 0, 0, width, height); InvalidateRect(performance, nullptr, FALSE); return;
  }
  updateFooter();
  const bool perf = selectedTab == PerformanceTab && !compact;
  RECT item{}; TabCtrl_GetItemRect(tabs, 0, &item); const int tabBottom = scale(3) + item.bottom + scale(2);
  show(tabs, !compact); place(tabs, scale(2), scale(3), std::max(1, width - scale(4)), item.bottom + scale(2));
  const int gap = compact ? 0 : selectedTab == StartupTab || selectedTab == HistoryTab ? scale(34) : scale(14);
  const bool showSearch = searchVisible && !compact && !perf;
  show(searchBox, showSearch);
  if (showSearch) place(searchBox, scale(10), tabBottom + gap, std::max(scale(100), width - scale(20)), scale(26));
  const int top = compact ? 0 : perf ? tabBottom : tabBottom + gap + (showSearch ? scale(34) : 0), bottom = std::max(top + 1, height - footer);
  show(list, !perf); show(performance, perf);
  place(perf ? performance : list, 0, top, width, bottom - top);
  if (compact) ListView_SetColumnWidth(list, 0, std::max(scale(100), width - GetSystemMetrics(SM_CXVSCROLL) - scale(4)));
  HDC dc = GetDC(window); SelectObject(dc, font); SIZE text{}; auto measure = [&](HWND control) { wchar_t value[128]{}; GetWindowTextW(control, value, 128); GetTextExtentPoint32W(dc, value, int(wcslen(value)), &text); return int(text.cx); };
  const int fewerWidth = measure(fewer) + scale(34), linkWidth = measure(link) + scale(8), historyWidth = measure(historyLink) + scale(4);
  std::wstring since = L"Resource usage since " + [&] { SYSTEMTIME now{}; GetLocalTime(&now); FILETIME file{}; SystemTimeToFileTime(&now, &file); ULARGE_INTEGER value{file.dwLowDateTime, file.dwHighDateTime}; value.QuadPart -= ULONGLONG(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - historySince).count()) * 10000000ULL; file = {value.LowPart, value.HighPart}; SYSTEMTIME local{}; FileTimeToSystemTime(&file, &local); wchar_t date[64]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr); return std::wstring(date); }() + L" for current user account.";
  GetTextExtentPoint32W(dc, since.c_str(), int(since.size()), &text); const int sinceWidth = text.cx; ReleaseDC(window, dc);
  const int middle = height - footer / 2;
  place(fewer, scale(8), middle - scale(12), fewerWidth, scale(24));
  const bool showFewer = compact || selectedTab == ProcessesTab;
  place(link, showFewer ? scale(8) + fewerWidth + scale(12) : scale(10), middle - scale(12), linkWidth, scale(24));
  place(end, width - scale(10) - scale(82), middle - scale(12), scale(82), scale(23));
  if (selectedTab == HistoryTab && !compact) place(historyLink, scale(9) + sinceWidth + scale(8), tabBottom + scale(8), historyWidth, scale(20));
  show(historyLink, selectedTab == HistoryTab && !compact);
  show(fewer, showFewer);
  InvalidateRect(window, nullptr, TRUE);
  if (perf) InvalidateRect(performance, nullptr, FALSE);
}
void Application::updateFooter() {
  if (!end) return;
  const auto row = selectedRow(); std::wstring text = L"End task"; bool visible = true, enabled = row != nullptr;
  switch (compact ? ProcessesTab : selectedTab) {
  case ProcessesTab: enabled = row && (row->kind == RowKind::Process || row->kind == RowKind::Group || row->kind == RowKind::Window); if (row && row->process >= 0 && current && lower(current->processes[size_t(row->process)].name) == L"explorer.exe" && row->kind == RowKind::Process) text = L"Restart"; break;
  case PerformanceTab: case HistoryTab: case ServicesTab: visible = false; break;
  case StartupTab: { const Entry* startup = row && row->entry >= 0 && current && size_t(row->entry) < current->startup->entries.size() ? &current->startup->entries[size_t(row->entry)] : nullptr; text = startup && !startup->enabled ? L"Enable" : L"Disable"; enabled = startup && startup->startupEditable; break; }
  case UsersTab: if (!row || row->kind == RowKind::User) text = L"Disconnect"; break;
  default: break;
  }
  wchar_t existing[64]{}; GetWindowTextW(end, existing, 64); if (text != existing) SetWindowTextW(end, text.c_str());
  ShowWindow(end, visible ? SW_SHOW : SW_HIDE); EnableWindow(end, enabled);
  const wchar_t* linkText = selectedTab == PerformanceTab ? L"Open Resource Monitor" : selectedTab == ServicesTab ? L"Open Services" : selectedTab == StartupTab ? L"Open Startup settings" : nullptr;
  if (linkText && !compact) { wchar_t current_[64]{}; GetWindowTextW(link, current_, 64); if (wcscmp(current_, linkText)) SetWindowTextW(link, linkText); ShowWindow(link, SW_SHOWNA); } else ShowWindow(link, SW_HIDE);
  const auto fewerText = compact ? L"More details" : L"Fewer details";
  GetWindowTextW(fewer, existing, 64); if (wcscmp(existing, fewerText)) SetWindowTextW(fewer, fewerText);
}
void Application::paintWindow(HDC dc) {
  RECT area{}; GetClientRect(window, &area);
  if (summary) return;
  const int footerTop = area.bottom - scale(50);
  RECT line{0, footerTop, area.right, footerTop + 1}; HBRUSH brush = CreateSolidBrush(themeColor(RGB(240, 240, 240))); FillRect(dc, &line, brush); DeleteObject(brush);
  line.top += 1; line.bottom += 1; brush = CreateSolidBrush(themeColor(RGB(160, 160, 160))); FillRect(dc, &line, brush); DeleteObject(brush);
  SetBkMode(dc, TRANSPARENT); SelectObject(dc, font);
  if (IsWindowVisible(link) && IsWindowVisible(fewer)) { RECT bounds{}; GetWindowRect(fewer, &bounds); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2); RECT bar{bounds.right + scale(2), bounds.top, bounds.right + scale(10), bounds.bottom}; SetTextColor(dc, themeColor(RGB(160, 160, 160))); DrawTextW(dc, L"|", 1, &bar, DT_CENTER | DT_VCENTER | DT_SINGLELINE); }
  if (compact) return;
  RECT item{}; TabCtrl_GetItemRect(tabs, 0, &item); const int tabBottom = scale(3) + item.bottom + scale(2);
  if (selectedTab == StartupTab) {
    RECT statusBand{0, tabBottom + scale(2), area.right, tabBottom + scale(34)};
    FillRect(dc, &statusBand, backgroundBrush);
    RECT coverage{scale(12), tabBottom + scale(8), std::max<LONG>(scale(12), area.right - scale(245)), tabBottom + scale(28)};
    const auto message = !current || (current->startup->entries.empty() && current->startup->warnings.empty()) ? L"Scanning startup sources..." : current->startup->warnings.empty() ? L"Extended startup inventory" : L"Partial coverage - see coverage rows";
    SetTextColor(dc, themeColor(RGB(109, 109, 109))); DrawTextW(dc, message, -1, &coverage, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    const double bios = lastBiosSeconds();
    if (bios > 0) {
      const auto value = number(bios, 1) + L" seconds"; RECT bounds{0, tabBottom + scale(8), area.right - scale(12), tabBottom + scale(28)};
      SetTextColor(dc, themeColor(RGB(0, 0, 0))); DrawTextW(dc, value.c_str(), -1, &bounds, DT_RIGHT | DT_SINGLELINE); SIZE size{}; GetTextExtentPoint32W(dc, value.c_str(), int(value.size()), &size);
      bounds.right -= size.cx + scale(6); SetTextColor(dc, themeColor(RGB(109, 109, 109))); DrawTextW(dc, L"Last BIOS time:", -1, &bounds, DT_RIGHT | DT_SINGLELINE);
    }
  }
  if (selectedTab == HistoryTab) {
    SYSTEMTIME now{}; GetLocalTime(&now); FILETIME file{}; SystemTimeToFileTime(&now, &file); ULARGE_INTEGER value{file.dwLowDateTime, file.dwHighDateTime}; value.QuadPart -= ULONGLONG(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - historySince).count()) * 10000000ULL; file = {value.LowPart, value.HighPart};
    SYSTEMTIME local{}; FileTimeToSystemTime(&file, &local); wchar_t date[64]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, 64, nullptr);
    const auto text = L"Resource usage since " + std::wstring(date) + L" for current user account."; RECT bounds{scale(9), tabBottom + scale(10), area.right, tabBottom + scale(30)};
    SetTextColor(dc, themeColor(RGB(0, 0, 0))); DrawTextW(dc, text.c_str(), -1, &bounds, DT_LEFT | DT_SINGLELINE);
  }
}
void Application::selectTab(int tab) {
  if (tab < 0 || tab >= TabCount || tab == selectedTab) return;
  captureColumns();
  KillTimer(window, 3); sortQueued = false;
  selectedTab = tab; startupVisible = tab == StartupTab && !compact; if (TabCtrl_GetCurSel(tabs) != tab) TabCtrl_SetCurSel(tabs, tab); hotRow = -1;
  if (tab != PerformanceTab) { if (columnsTab != tab) columns(); rebuild(); }
  layout();
  if (tab == StartupTab && !startupRequested) { startupRequested = true; if (!current || current->startup->entries.empty()) refresh(false); }
  if (tab == PerformanceTab && !hidden()) SetFocus(performance);
}
void Application::requestTab(int tab) {
  if (tab < 0 || tab >= TabCount) return;
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden() && measured) ++tabRequests;
#endif
  if (!tabQueued && tab == selectedTab) return;
  pendingTab = tab;
  if (!tabQueued) {
    SendMessageW(tabs, WM_SETREDRAW, FALSE, 0);
    tabQueued = PostMessageW(window, WM_APP + 12, 0, 0) != 0;
    if (!tabQueued) { SendMessageW(tabs, WM_SETREDRAW, TRUE, 0); pendingTab = -1; selectTab(tab); InvalidateRect(tabs, nullptr, FALSE); }
  }
}
void Application::setCompact(bool value) {
  pendingTab = -1;
  if (tabQueued) SendMessageW(tabs, WM_SETREDRAW, TRUE, 0);
  if (compact == value && hidden()) return;
  captureColumns(); compact = value; startupVisible = selectedTab == StartupTab && !compact;
  RECT rect{}; GetWindowRect(window, &rect);
  if (compact) { normalRect = rect; SetMenu(window, nullptr); } else { compactRect = rect; SetMenu(window, menuBar); }
  const RECT target = compact ? compactRect : normalRect;
  if (!IsZoomed(window)) {
    int width = target.right - target.left, height = target.bottom - target.top;
    if (width <= 0 || height <= 0) { width = scale(compact ? 330 : 1010); height = scale(compact ? 360 : 875); }
    SetWindowPos(window, nullptr, rect.left, rect.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  }
  columns(); rebuild(); layout();
}
void Application::setSummary(int value) {
  hotResource = -1;
  summary = value; SetMenu(window, summary || compact ? nullptr : menuBar);
  LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
  if (summary) style &= ~LONG_PTR(WS_CAPTION); else style |= WS_CAPTION;
  SetWindowLongPtrW(window, GWL_STYLE, style); SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
  layout();
}
void Application::updateMenu(HMENU popup) {
  auto check = [&](int id, bool value) { CheckMenuItem(popup, UINT(id), MF_BYCOMMAND | (value ? MF_CHECKED : MF_UNCHECKED)); };
  check(TopmostId, topmost); check(MinimizeOnUseId, minimizeOnUse); check(HideId, hideMinimized); check(GroupId, groupByType); check(FullNameId, fullName); check(AllHistoryId, allHistory);
  check(ReplaceDefaultId, replacementEnabled());
  CheckMenuRadioItem(popup, ThemeLightId, ThemeSystemId, UINT(ThemeLightId + theme), MF_BYCOMMAND);
  check(SearchId, searchVisible); EnableMenuItem(popup, SearchId, MF_BYCOMMAND | ((compact || summary || selectedTab == PerformanceTab) ? MF_GRAYED : MF_ENABLED));
  EnableMenuItem(popup, ReplaceDefaultId, MF_BYCOMMAND | (actionBusy ? MF_GRAYED : MF_ENABLED));
  const int speed = interval == 500 ? SpeedHigh : interval == 4000 ? SpeedLow : interval == 0 ? SpeedPause : SpeedNormal;
  CheckMenuRadioItem(popup, SpeedHigh, SpeedPause, UINT(speed), MF_BYCOMMAND);
  CheckMenuRadioItem(popup, DefaultTabId, DefaultTabId + TabCount - 1, UINT(DefaultTabId + defaultTab), MF_BYCOMMAND);
  const bool tree = selectedTab == ProcessesTab || selectedTab == UsersTab;
  EnableMenuItem(popup, GroupId, MF_BYCOMMAND | (selectedTab == ProcessesTab ? MF_ENABLED : MF_GRAYED));
  EnableMenuItem(popup, ExpandAllId, MF_BYCOMMAND | (tree ? MF_ENABLED : MF_GRAYED)); EnableMenuItem(popup, CollapseAllId, MF_BYCOMMAND | (tree ? MF_ENABLED : MF_GRAYED));
}
// The notification-area icon is a CPU meter, like Windows' own Task Manager.
void Application::updateTray() {
  if (!trayAdded || !current) return;
  const int level = int(std::lround(current->cpu / 100 * 14));
  const double memory = current->memory.total ? 100.0 * double(current->memory.total - current->memory.available) / double(current->memory.total) : 0;
  double disk = 0; for (const auto& item : current->disks) disk = std::max(disk, item.active);
  double network = 0; for (const auto& item : current->networks) if (item.speed) network = std::max(network, 800.0 * (item.send + item.receive) / double(item.speed));
  swprintf_s(tray.szTip, L"CPU: %.0f%%\nMemory: %.0f%%\nDisk: %.0f%%\nNetwork: %.0f%%", current->cpu, memory, disk, std::min(100.0, network));
  if (level != trayLevel) {
    trayLevel = level; const int size = GetSystemMetrics(SM_CXSMICON);
    BITMAPINFO information{}; information.bmiHeader = {sizeof(BITMAPINFOHEADER), size, -size, 1, 32, BI_RGB}; void* bits = nullptr;
    HBITMAP color = CreateDIBSection(nullptr, &information, DIB_RGB_COLORS, &bits, nullptr, 0), mask = CreateBitmap(size, size, 1, 1, nullptr);
    auto pixels = static_cast<DWORD*>(bits); const int fill = int(std::lround(double(level) / 14 * (size - 4)));
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
      const bool frame = x >= 1 && x <= size - 2 && y >= 1 && y <= size - 2, edge = frame && (x == 1 || x == size - 2 || y == 1 || y == size - 2);
      DWORD value = 0; if (edge) value = 0xff5a5a5a; else if (frame) value = y >= size - 2 - fill ? 0xff36b04a : 0xff1e1e1e;
      pixels[y * size + x] = value;
    }
    ICONINFO iconInfo{TRUE, 0, 0, mask, color}; HICON next = CreateIconIndirect(&iconInfo); DeleteObject(color); DeleteObject(mask);
    if (next) { tray.hIcon = next; if (trayIcon) DestroyIcon(trayIcon); trayIcon = next; }
  }
  Shell_NotifyIconW(NIM_MODIFY, &tray);
}
void Application::loadSettings() {
  auto read = [&](const wchar_t* section, const wchar_t* key, int fallback) { return persistent() ? int(GetPrivateProfileIntW(section, key, fallback, preferences.c_str())) : fallback; };
  defaultTab = std::clamp(read(L"Window", L"DefaultTab", 0), 0, TabCount - 1);
  theme = std::clamp(read(L"Window", L"Theme", 0), 0, 2);
  interval = read(L"Window", L"Interval", 1000); if (interval != 0 && interval != 500 && interval != 1000 && interval != 4000) interval = 1000;
  topmost = read(L"Window", L"Topmost", 0); minimizeOnUse = read(L"Window", L"MinimizeOnUse", 1); hideMinimized = read(L"Window", L"Hide", 0); groupByType = read(L"Window", L"GroupByType", 1); compact = read(L"Window", L"Compact", 0);
  memoryPercent = read(L"Window", L"MemoryPercent", 0); networkPercent = read(L"Window", L"NetworkPercent", 0); logical = read(L"Window", L"Logical", 0); hideGraphs = read(L"Window", L"HideGraphs", 0); fullName = read(L"Window", L"FullName", 0); allHistory = read(L"Window", L"AllHistory", 1);
  normalRect = {read(L"Window", L"Left", 0), read(L"Window", L"Top", 0), read(L"Window", L"Right", 0), read(L"Window", L"Bottom", 0)};
  compactRect = {read(L"Window", L"CompactLeft", 0), read(L"Window", L"CompactTop", 0), read(L"Window", L"CompactRight", 0), read(L"Window", L"CompactBottom", 0)};
  if (persistent()) { const auto seconds = read(L"History", L"Since", 0); if (seconds > 0) { const auto now = std::chrono::system_clock::now(); historySince = Clock::now() - std::chrono::duration_cast<Clock::duration>(now - std::chrono::system_clock::time_point(std::chrono::seconds(unsigned(seconds)))); } }
  for (int tab = 0; tab < TabCount; ++tab) {
    const auto section = std::to_wstring(tab); const auto definitions_ = definitions(tab);
    sortColumn[size_t(tab)] = std::clamp(read(section.c_str(), L"Sort", 0), 0, int(definitions_.size()) - 1); ascending[size_t(tab)] = read(section.c_str(), L"Ascending", 1) != 0;
    wchar_t text[1024]{}; if (persistent()) GetPrivateProfileStringW(section.c_str(), L"Columns", L"", text, 1024, preferences.c_str());
    std::vector<int> order, sizes;
    for (const auto token : splitFields(text, L',')) { const auto colon = token.find(L':'); if (colon == token.npos) continue; const int id = _wtoi(token.data()); if (id >= 0 && id < int(definitions_.size()) && std::find(order.begin(), order.end(), id) == order.end()) { order.push_back(id); sizes.push_back(std::clamp(_wtoi(token.data() + colon + 1), minimumColumnWidth(tab, id), 2000)); } }
    if (order.empty() || order.front() != 0) { order.clear(); sizes.clear(); for (int id = 0; id < int(definitions_.size()); ++id) if (definitions_[size_t(id)].visible) { order.push_back(id); sizes.push_back(definitions_[size_t(id)].width); } }
    shown[size_t(tab)] = order; widths[size_t(tab)] = sizes;
  }
}
// Reads the current tab's column order and widths from the header into display order. Callers rebuild the columns afterwards.
void Application::captureColumns() {
  if (!list || rebuilding || compact || selectedTab != columnsTab) return;
  const int count = Header_GetItemCount(header); std::vector<int> order(size_t(std::max(0, count)));
  if (count <= 0 || size_t(count) != shown[size_t(selectedTab)].size() || !ListView_GetColumnOrderArray(list, count, order.data())) return;
  std::vector<int> ids, sizes; for (const int index : order) { ids.push_back(shown[size_t(selectedTab)][size_t(index)]); sizes.push_back(MulDiv(ListView_GetColumnWidth(list, index), 96, int(GetDpiForWindow(window)))); }
  if (!ids.empty() && ids.front() == 0) { shown[size_t(selectedTab)] = ids; widths[size_t(selectedTab)] = sizes; }
}
void Application::saveSettings() {
  if (!persistent()) return;
  if (closing || !SetTimer(window, 4, 750, nullptr)) writeSettings();
}
void Application::writeSettings() {
  auto saved = shown; auto savedWidths = widths;
  if (list && !rebuilding && !compact) { const auto ids = shown[size_t(selectedTab)]; const auto sizes = widths[size_t(selectedTab)]; captureColumns(); saved = shown; savedWidths = widths; shown[size_t(selectedTab)] = ids; widths[size_t(selectedTab)] = sizes; for (size_t index = 0; index < ids.size(); ++index) widths[size_t(selectedTab)][index] = MulDiv(ListView_GetColumnWidth(list, int(index)), 96, int(GetDpiForWindow(window))); }
  if (!persistent()) return;
  auto write = [&](const std::wstring& section, const std::wstring& key, int value) { WritePrivateProfileStringW(section.c_str(), key.c_str(), std::to_wstring(value).c_str(), preferences.c_str()); };
  write(L"Window", L"Theme", theme);
  write(L"Window", L"DefaultTab", defaultTab); write(L"Window", L"Interval", interval); write(L"Window", L"Topmost", topmost); write(L"Window", L"MinimizeOnUse", minimizeOnUse); write(L"Window", L"Hide", hideMinimized); write(L"Window", L"GroupByType", groupByType); write(L"Window", L"Compact", compact);
  write(L"Window", L"MemoryPercent", memoryPercent); write(L"Window", L"NetworkPercent", networkPercent); write(L"Window", L"Logical", logical); write(L"Window", L"HideGraphs", hideGraphs); write(L"Window", L"FullName", fullName); write(L"Window", L"AllHistory", allHistory);
  WINDOWPLACEMENT placement{sizeof(placement)};
  if (window && !summary && GetWindowPlacement(window, &placement)) {
    (compact ? compactRect : normalRect) = placement.rcNormalPosition; write(L"Window", L"Maximized", placement.showCmd == SW_SHOWMAXIMIZED);
    write(L"Window", L"Left", normalRect.left); write(L"Window", L"Top", normalRect.top); write(L"Window", L"Right", normalRect.right); write(L"Window", L"Bottom", normalRect.bottom);
    write(L"Window", L"CompactLeft", compactRect.left); write(L"Window", L"CompactTop", compactRect.top); write(L"Window", L"CompactRight", compactRect.right); write(L"Window", L"CompactBottom", compactRect.bottom);
  }
  write(L"History", L"Since", int(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() - std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - historySince).count()));
  for (int tab = 0; tab < TabCount; ++tab) {
    const auto section = std::to_wstring(tab); std::wstring text;
    for (size_t index = 0; index < saved[size_t(tab)].size(); ++index) { if (index) text += L','; text += std::to_wstring(saved[size_t(tab)][index]) + L":" + std::to_wstring(index < savedWidths[size_t(tab)].size() ? savedWidths[size_t(tab)][index] : 100); }
    WritePrivateProfileStringW(section.c_str(), L"Columns", text.c_str(), preferences.c_str()); write(section, L"Sort", sortColumn[size_t(tab)]); write(section, L"Ascending", ascending[size_t(tab)]);
  }
}
void Application::saveHistory() {
  if (!persistent() || historyFile.empty()) return; const auto temporary = historyFile + L".tmp";
  { std::wofstream stream{std::filesystem::path(temporary)}; for (const auto& [key, value] : usage) stream << key << L'\t' << value.ticks << L'\t' << value.name << L'\t' << value.path << L'\t' << (value.app ? 1 : 0) << L'\n'; if (!stream) return; }
  MoveFileExW(temporary.c_str(), historyFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
void Application::loadHistory() {
  std::wifstream stream{std::filesystem::path(historyFile)}; std::wstring line;
  while (std::getline(stream, line) && usage.size() < 8192) {
    const auto fields = splitFields(line, L'\t');
    if (fields.size() < 3) continue;
    try { usage[std::wstring(fields[0])] = {std::wstring(fields[2]), fields.size() > 3 ? std::wstring(fields[3]) : L"", std::stoull(std::wstring(fields[1])), fields.size() > 4 && fields[4] == L"1"}; } catch (...) {}
  }
}
void Application::worker() {
  try {
    const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Sampler source(!hidden());
    while (!stopping) {
      auto sample = std::make_shared<Sample>(source.sample(startupVisible.load()));
      { std::lock_guard lock(workerMutex); pending = std::move(sample);
#ifdef TASKMGR_DIAGNOSTICS
        published = Clock::now();
#endif
      }
      if (!notification.exchange(true)) PostMessageW(window, SampleMessage, 0, 0);
      std::unique_lock lock(workerMutex); const int delay = interval.load();
      if (!delay) {
        while (source.startupPending() && !source.startupReady() && !stopping && !force && interval.load() == 0) wake.wait_for(lock, std::chrono::milliseconds(100));
        if (!source.startupReady()) wake.wait(lock, [&] { return stopping || force || interval.load() != 0; });
      }
      else wake.wait_for(lock, std::chrono::milliseconds(minimized ? std::max(delay, 4000) : delay), [&] { return stopping || force; });
      if (forceInventory) source.invalidateInventory(); force = forceInventory = false;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
  } catch (const std::exception& exception) { std::lock_guard lock(actionMutex); const auto text = exception.what(); actionResults.push_back({std::wstring(text, text + strlen(text)), L""}); PostMessageW(window, ActionMessage, 0, 0); }
}
void Application::consume() {
  if (rebuilding || verifyingSorting) return;
  if (!hidden() && (minimized || (GetKeyState(VK_CONTROL) & 0x8000))) return;
  std::shared_ptr<Sample> sample;
#ifdef TASKMGR_DIAGNOSTICS
  Time publish;
#endif
  { std::lock_guard lock(workerMutex); notification = false; sample.swap(pending);
#ifdef TASKMGR_DIAGNOSTICS
    publish = published;
#endif
  }
  if (!sample) return;
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden()) { if (measured && publish >= measurementStarted) deliveryTimes.push_back(milliseconds(publish)); samplerTimes.push_back(sample->duration); for (size_t index = 0; index < stageTimes.size(); ++index) stageTimes[index].push_back(sample->stages[index]); }
#endif
  const bool startupChanged = !current || current->startup != sample->startup;
  if (indexedInventory != sample->inventory) { indexedServices.clear(); indexedInventory.reset(); }
  current = std::move(sample);
  performanceItemsDirty = true;
#ifdef TASKMGR_DIAGNOSTICS
  ++updates;
  if (firstSample == Time{}) firstSample = Clock::now();
#endif
  const double now = std::chrono::duration<double>(Clock::now() - started).count(); const double elapsed = lastTrend > 0 ? std::clamp(now - lastTrend, 0.0, 10.0) : 0; graphTime = now; lastTrend = now;
  recordHistories(now);
  ++activityGeneration;
  const double weight = elapsed == 0 ? 1 : 1 - std::exp(-elapsed / 120);
  for (const auto& process : current->processes) {
    const Identity identity{process.id, process.created}; const double score = powerScore(process);
    auto [tracked, inserted] = activity.try_emplace(identity, Activity{process.cpuTicks, activityGeneration, score});
    if (process.id > 4) {
      const auto& metadata = icons->get(process); const auto key = lower(process.name); auto found = usage.find(key);
      if (found == usage.end() && usage.size() < 8192) found = usage.emplace(key, Usage{process.name, L"", 0}).first;
      if (found != usage.end()) {
        if (!inserted && process.cpuTicks >= tracked->second.ticks) found->second.ticks += process.cpuTicks - tracked->second.ticks;
        if (!metadata.description.empty()) found->second.name = metadata.description; if (!metadata.path.empty()) found->second.path = metadata.path; if (process.app) found->second.app = true;
      }
    }
    tracked->second.ticks = process.cpuTicks; tracked->second.seen = activityGeneration;
    if (!inserted) tracked->second.trend += (score - tracked->second.trend) * weight;
  }
  std::erase_if(activity, [&](const auto& item) { return item.second.seen != activityGeneration; }); icons->prune(activity);
  if (selectedTab == PerformanceTab && !compact) InvalidateRect(performance, nullptr, FALSE);
  else if (selectedTab != StartupTab || compact || startupChanged) rebuild();
  if (selectedTab == StartupTab && startupChanged) InvalidateRect(window, nullptr, FALSE);
  if (twoLineHeader()) InvalidateRect(header, nullptr, FALSE);
  updateTray();
}
LRESULT CALLBACK Application::linkProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR) {
  if (message == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; }
  if (message == WM_MOUSEMOVE && !GetPropW(target, L"hot")) { SetPropW(target, L"hot", HANDLE(1)); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0}; TrackMouseEvent(&track); InvalidateRect(target, nullptr, FALSE); }
  if (message == WM_MOUSELEAVE) { RemovePropW(target, L"hot"); InvalidateRect(target, nullptr, FALSE); }
  if (message == WM_NCDESTROY) { RemovePropW(target, L"hot"); RemoveWindowSubclass(target, linkProcedure, id); }
  return DefSubclassProc(target, message, word, data);
}
LRESULT CALLBACK Application::procedure(HWND target, UINT message, WPARAM word, LPARAM data) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(target, GWLP_USERDATA));
  if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams); app->window = target; app->dpi = GetDpiForWindow(target); SetWindowLongPtrW(target, GWLP_USERDATA, LONG_PTR(app)); }
  if (!app) return DefWindowProcW(target, message, word, data);
  switch (message) {
  case WM_CREATE: app->create(); return 0;
  case WM_SETTINGCHANGE: case WM_THEMECHANGED: app->indexedServices.clear(); app->indexedInventory.reset(); app->applyTheme(); return 0;
  case WM_ERASEBKGND: { RECT bounds{}; GetClientRect(target, &bounds); FillRect(HDC(word), &bounds, app->backgroundBrush ? app->backgroundBrush : GetStockBrush(WHITE_BRUSH)); return 1; }
  case WM_CTLCOLOREDIT: { const HDC dc = HDC(word); SetTextColor(dc, app->themeColor(RGB(0, 0, 0))); SetBkColor(dc, app->themeColor(RGB(255, 255, 255))); return LRESULT(app->backgroundBrush); }
  case WM_APP + 11:
    if (app->persistent() && !GetPrivateProfileIntW(L"Window", L"ReplacementPrompted", 0, app->preferences.c_str())) {
      const bool enabled = replacementEnabled();
      const bool accepted = !enabled && MessageBoxW(target, L"Replace Windows Task Manager with this app?\n\nCtrl+Shift+Esc and Windows Task Manager launch commands will open this app for all accounts. One protected executable will be installed in Program Files after administrator approval.\n\nTurn off Options > Replace default Task Manager to restore Windows Task Manager.", L"Task Manager setup", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1) == IDYES;
      WritePrivateProfileStringW(L"Window", L"ReplacementPrompted", L"1", app->preferences.c_str());
      if (accepted) app->asynchronous([target] { requestReplacement(target, true); return std::wstring{}; });
    }
    return 0;
  case WM_SIZE: app->minimized = word == SIZE_MINIMIZED; if (app->minimized && app->hideMinimized && app->trayAdded) ShowWindow(target, SW_HIDE); if (!app->minimized) app->layout(); return 0;
  case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(data)->ptMinTrackSize = app->summary ? POINT{app->scale(200), app->scale(150)} : app->compact ? POINT{app->scale(250), app->scale(200)} : POINT{app->scale(420), app->scale(300)}; return 0;
  case WM_DPICHANGED: { app->dpi = HIWORD(word); const auto rect = reinterpret_cast<RECT*>(data); SetWindowPos(target, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); app->createFonts(); app->columns(); app->rebuild(); app->layout(); return 0; }
  case SampleMessage: app->consume(); return 0;
  case IconMessage: app->icons->consume(); SetTimer(target, 2, app->hidden() ? 10 : 120, nullptr); return 0;
  case ActionMessage: { std::deque<ActionResult> results; { std::lock_guard lock(app->actionMutex); results.swap(app->actionResults); } for (const auto& result : results) { if (!app->hidden() && !result.error.empty()) MessageBoxW(target, result.error.c_str(), L"Task Manager", MB_OK | MB_ICONERROR); if (!app->hidden() && !result.report.empty()) DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(103), target, [](HWND dialog, UINT code, WPARAM parameter, LPARAM value) -> INT_PTR { if (code == WM_INITDIALOG) { SetDlgItemTextW(dialog, 1001, reinterpret_cast<const wchar_t*>(value)); return TRUE; } if (code == WM_COMMAND && (LOWORD(parameter) == IDOK || LOWORD(parameter) == IDCANCEL)) { EndDialog(dialog, IDOK); return TRUE; } return FALSE; }, LPARAM(result.report.c_str())); } app->refresh(); return 0; }
  case TrayMessage:
    if (data == WM_LBUTTONUP || data == WM_LBUTTONDBLCLK) app->command(TrayRestore);
    else if (data == WM_RBUTTONUP) { POINT point{}; GetCursorPos(&point); HMENU popup = CreatePopupMenu(); AppendMenuW(popup, MF_STRING, TrayRestore, L"&Restore"); AppendMenuW(popup, MF_STRING | (app->topmost ? MF_CHECKED : 0), TopmostId, L"&Always on top"); AppendMenuW(popup, MF_SEPARATOR, 0, nullptr); AppendMenuW(popup, MF_STRING, ExitId, L"&Close"); SetMenuDefaultItem(popup, TrayRestore, FALSE); SetForegroundWindow(target); TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, target, nullptr); DestroyMenu(popup); }
    return 0;
#ifdef TASKMGR_DIAGNOSTICS
  case ProbeMessage: { if (!app->measured || LONGLONG(word) < app->measurementCounter) return 0; LARGE_INTEGER counter{}, frequency{}; QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency); app->deliveryTimes.push_back(1000.0 * double(counter.QuadPart - LONGLONG(word)) / double(frequency.QuadPart)); return 0; }
#endif
  case WM_TIMER:
    if (word == 5) { app->applySearch(); return 0; }
    if (word == 4) { KillTimer(target, 4); app->writeSettings(); return 0; }
    if (word == 3) { if (app->sortQueued) { app->rebuild(true); InvalidateRect(app->header, nullptr, FALSE); } return 0; }
    if (word == 2) { if (app->verifyingSorting || app->rebuilding) return 0; KillTimer(target, 2); if (!app->minimized && !(app->selectedTab == PerformanceTab && !app->compact)) app->rebuild(); return 0; }
#ifdef TASKMGR_DIAGNOSTICS
    if (app->hidden()) app->benchmarkTick(); else if (!app->options.screenshots.empty()) app->screenshotTick(); else
#endif
    if (app->notification) app->consume(); return 0;
  case WM_COMMAND:
    if (LOWORD(word) == SearchId && HWND(data) == app->searchBox) {
      if (HIWORD(word) == EN_CHANGE && !app->searchQueued) { app->searchQueued = SetTimer(target, 5, 100, nullptr) != 0; if (!app->searchQueued) app->applySearch(); }
      return 0;
    }
    app->command(LOWORD(word)); return 0;
  case WM_INITMENUPOPUP: app->updateMenu(reinterpret_cast<HMENU>(word)); return 0;
  case WM_NOTIFY: {
    const auto notice = reinterpret_cast<NMHDR*>(data);
    if (notice->hwndFrom == app->tabs && notice->code == TCN_SELCHANGE) { app->requestTab(TabCtrl_GetCurSel(app->tabs)); return 0; }
    if (notice->hwndFrom == app->header && (notice->code == HDN_ENDDRAG || notice->code == HDN_ENDTRACKW || notice->code == HDN_ITEMCHANGEDW)) { app->columnGeometryDirty = true; if (!app->rebuilding) app->geometryQueued = true; return 0; }
    if (notice->hwndFrom != app->list) break;
    if (notice->code == NM_CUSTOMDRAW) {
      const auto draw = reinterpret_cast<NMLVCUSTOMDRAW*>(data);
      if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW | (app->rows.empty() && !app->searchText.empty() ? CDRF_NOTIFYPOSTPAINT : 0);
      if (draw->nmcd.dwDrawStage == CDDS_POSTPAINT && app->rows.empty() && !app->searchText.empty()) {
        RECT bounds{}, heading{}; GetClientRect(app->list, &bounds); GetWindowRect(app->header, &heading); bounds.top = heading.bottom - heading.top + app->scale(24); bounds.left += app->scale(12); bounds.right -= app->scale(12);
        const auto previous = SelectObject(draw->nmcd.hdc, app->font); SetBkMode(draw->nmcd.hdc, TRANSPARENT); SetTextColor(draw->nmcd.hdc, app->themeColor(RGB(0, 0, 0)));
        DrawTextW(draw->nmcd.hdc, L"No matching items. Clear the search to show all items.", -1, &bounds, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX); SelectObject(draw->nmcd.hdc, previous); return CDRF_DODEFAULT;
      }
      if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) { RECT bounds{}; ListView_GetItemRect(app->list, int(draw->nmcd.dwItemSpec), &bounds, LVIR_BOUNDS); app->drawRow(draw->nmcd.hdc, int(draw->nmcd.dwItemSpec), bounds); return CDRF_SKIPDEFAULT; }
      return CDRF_DODEFAULT;
    }
    if (notice->code == LVN_GETDISPINFOW) { const auto info = reinterpret_cast<NMLVDISPINFOW*>(data); if (info->item.iItem >= 0 && size_t(info->item.iItem) < app->rows.size()) { const auto& row = app->rows[size_t(info->item.iItem)]; if (info->item.mask & LVIF_TEXT) { const auto& text = app->cell(row, app->columnAt(info->item.iSubItem)); wcsncpy_s(info->item.pszText, size_t(info->item.cchTextMax), text.c_str(), _TRUNCATE); } if (info->item.mask & LVIF_IMAGE) info->item.iImage = 0; } return 0; }
    if (notice->code == LVN_ODFINDITEMW) { const auto info = reinterpret_cast<NMLVFINDITEMW*>(data); return info->lvfi.psz ? app->findRow(info->lvfi.psz, info->iStart) : -1; }
    if (notice->code == LVN_ITEMCHANGED) { const auto change = reinterpret_cast<NMLISTVIEW*>(data); if (!app->rebuilding && (change->uChanged & LVIF_STATE) && ((change->uNewState ^ change->uOldState) & LVIS_SELECTED)) app->updateFooter(); return 0; }
    if (notice->code == LVN_COLUMNCLICK) {
      const int column = app->columnAt(reinterpret_cast<NMLISTVIEW*>(data)->iSubItem); auto& sort = app->sortColumn[size_t(app->selectedTab)]; auto& direction = app->ascending[size_t(app->selectedTab)];
      if (column < 0 || size_t(column) >= definitions(app->selectedTab).size() || app->rebuilding) return 0;
      if (sort == column) direction = !direction; else { sort = column; const auto definition = definitions(app->selectedTab)[size_t(column)]; direction = !(definition.right || definition.heat); }
      if (app->sortQueued && Clock::now() >= app->sortDue) app->rebuild(true);
      else if (!app->sortQueued) { app->sortDue = Clock::now() + std::chrono::milliseconds(32); app->sortQueued = SetTimer(target, 3, 32, nullptr) != 0; if (!app->sortQueued) app->rebuild(true); }
      InvalidateRect(app->header, nullptr, FALSE); return 0;
    }
    break;
  }
  case WM_APP + 10:
    if (!app->geometryQueued || app->columnMutationDepth || app->rebuilding) return 0;
    app->geometryQueued = false; app->updateColumnGeometry();
#ifdef TASKMGR_DIAGNOSTICS
    ++app->columnGeometryCommits;
#endif
    RedrawWindow(app->list, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    return 0;
  case WM_APP + 12: {
    const int tab = app->pendingTab; app->pendingTab = -1; app->tabQueued = false;
    SendMessageW(app->tabs, WM_SETREDRAW, TRUE, 0);
#ifdef TASKMGR_DIAGNOSTICS
    const auto begin = Clock::now();
#endif
    if (!app->closing) app->selectTab(tab);
    InvalidateRect(app->tabs, nullptr, FALSE);
#ifdef TASKMGR_DIAGNOSTICS
    if (app->hidden() && app->measured && !app->verifyingSorting) { UpdateWindow(app->tabs); UpdateWindow(app->selectedTab == PerformanceTab ? app->performance : app->list); app->tabTimes.push_back(milliseconds(begin)); ++app->tabCommits; }
#endif
    return 0;
  }
  case WM_MEASUREITEM: {
    const auto item = reinterpret_cast<MEASUREITEMSTRUCT*>(data);
    if (item->CtlType != ODT_MENU || item->itemData < 1 || item->itemData > 3) break;
    const wchar_t* labels[] = {L"File", L"Options", L"View"}; HDC dc = GetDC(target); auto previous = SelectObject(dc, app->font); SIZE size{};
    GetTextExtentPoint32W(dc, labels[item->itemData - 1], int(wcslen(labels[item->itemData - 1])), &size); SelectObject(dc, previous); ReleaseDC(target, dc);
    item->itemWidth = UINT(size.cx + app->scale(12)); item->itemHeight = UINT(GetSystemMetricsForDpi(SM_CYMENU, app->dpi)); return TRUE;
  }
  case WM_DRAWITEM: {
    const auto item = reinterpret_cast<DRAWITEMSTRUCT*>(data);
    if (item->CtlType == ODT_MENU && item->itemData >= 1 && item->itemData <= 3) {
      const wchar_t* labels[] = {L"&File", L"&Options", L"&View"}; const bool selected = (item->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
      SetDCBrushColor(item->hDC, app->themeColor(selected ? RGB(205, 232, 255) : RGB(255, 255, 255))); FillRect(item->hDC, &item->rcItem, GetStockBrush(DC_BRUSH));
      SetBkMode(item->hDC, TRANSPARENT); SelectObject(item->hDC, app->font);
      SetTextColor(item->hDC, app->highContrast && selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : app->themeColor(RGB(0, 0, 0)));
      RECT text = item->rcItem; DrawTextW(item->hDC, labels[item->itemData - 1], -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | ((item->itemState & ODS_NOACCEL) ? DT_HIDEPREFIX : 0)); return TRUE;
    }
    if (item->CtlType != ODT_BUTTON) break;
    HDC dc = item->hDC; RECT bounds = item->rcItem; FillRect(dc, &bounds, app->backgroundBrush); SetBkMode(dc, TRANSPARENT);
    wchar_t text[128]{}; GetWindowTextW(item->hwndItem, text, 128); const bool hot = GetPropW(item->hwndItem, L"hot") != nullptr;
    if (item->CtlID == EndId) {
      SetDCBrushColor(dc, app->themeColor(hot ? RGB(205, 232, 255) : RGB(229, 243, 255))); FillRect(dc, &bounds, GetStockBrush(DC_BRUSH));
      SetDCBrushColor(dc, app->themeColor(RGB(160, 160, 160))); FrameRect(dc, &bounds, GetStockBrush(DC_BRUSH));
      SelectObject(dc, app->font); SetTextColor(dc, app->themeColor((item->itemState & ODS_DISABLED) ? RGB(109, 109, 109) : RGB(0, 0, 0)));
      DrawTextW(dc, text, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) { InflateRect(&bounds, -3, -3); DrawFocusRect(dc, &bounds); }
      return TRUE;
    }
    if (item->CtlID == FewerId) {
      const int size = app->scale(18), top = bounds.top + (bounds.bottom - bounds.top - size) / 2;
      {
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); Gdiplus::Pen pen(hot ? Gdiplus::Color(255, 0, 102, 204) : Gdiplus::Color(255, 118, 118, 118), 1.0f);
        graphics.DrawEllipse(&pen, Gdiplus::REAL(bounds.left + app->scale(2)), Gdiplus::REAL(top), Gdiplus::REAL(size - 1), Gdiplus::REAL(size - 1));
        const float centerX = float(bounds.left + app->scale(2)) + float(size - 1) / 2, centerY = float(top) + float(size - 1) / 2, unit = float(app->scale(4)); const float direction = app->compact ? 1.0f : -1.0f;
        const Gdiplus::PointF chevron[] = {{centerX - unit, centerY - direction * unit / 2}, {centerX, centerY + direction * unit / 2}, {centerX + unit, centerY - direction * unit / 2}}; graphics.DrawLines(&pen, chevron, 3);
      }
      bounds.left += size + app->scale(9); SelectObject(dc, app->font); SetTextColor(dc, hot ? app->themeColor(RGB(0, 102, 204)) : app->themeColor(RGB(0, 0, 0)));
    } else { SelectObject(dc, hot ? app->linkFont : app->font); SetTextColor(dc, app->themeColor(RGB(0, 102, 204))); if (item->CtlID == LinkId) bounds.left += app->scale(2); }
    DrawTextW(dc, text, -1, &bounds, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if ((item->itemState & ODS_FOCUS) && !(item->itemState & ODS_NOFOCUSRECT)) { RECT focus = item->rcItem; InflateRect(&focus, -1, -1); DrawFocusRect(dc, &focus); }
    return TRUE;
  }
  case WM_CONTEXTMENU: {
    POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
    if (reinterpret_cast<HWND>(word) == app->header) { if (point.x == -1) GetCursorPos(&point); app->headerMenu(point); return 0; }
    if (reinterpret_cast<HWND>(word) == app->list) {
      if (point.x == -1) { RECT bounds{}; const int index = app->selectedIndex(); if (index >= 0 && ListView_GetItemRect(app->list, index, &bounds, LVIR_LABEL)) { point = {bounds.left + app->scale(40), bounds.bottom}; ClientToScreen(app->list, &point); } else GetCursorPos(&point); }
      else { POINT client = point; ScreenToClient(app->list, &client); LVHITTESTINFO hit{}; hit.pt = client; const int index = ListView_SubItemHitTest(app->list, &hit); if (index >= 0) app->select(index); else return 0; }
      app->menu(point); return 0;
    }
    break;
  }
  case WM_PAINT: { PAINTSTRUCT paint{}; HDC dc = BeginPaint(target, &paint); app->paintWindow(dc); EndPaint(target, &paint); return 0; }
  case WM_CLOSE: app->closing = true; app->saveSettings(); app->saveHistory(); app->stopping = true; app->wake.notify_all(); if (app->trayAdded) Shell_NotifyIconW(NIM_DELETE, &app->tray); DestroyWindow(target); return 0;
  case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(target, message, word, data);
}
#ifdef TASKMGR_DIAGNOSTICS
int runApplication(const RunOptions& options) { Application application(options); return application.run(); }
#else
int runApplication() { Application application; return application.run(); }
#endif
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  if (!arguments) return 2;
  const std::wstring_view first = count > 1 ? arguments[1] : L"";
  const bool enableReplacement = count == 2 && first == L"--enable-replacement";
  const bool disableReplacement = count == 2 && first == L"--disable-replacement";
  const bool interactive = count <= 1 || first == L"--replacement-launch";
#ifdef TASKMGR_DIAGNOSTICS
  std::vector<std::wstring> values;
  if (!interactive && !enableReplacement && !disableReplacement) for (int index = 1; index < count; ++index) values.emplace_back(arguments[index]);
#endif
  LocalFree(arguments);
  if (enableReplacement || disableReplacement) return taskmgr::configureReplacement(enableReplacement);
#ifndef TASKMGR_DIAGNOSTICS
  if (!interactive) return 2;
#endif
  taskmgr::Handle instance;
  if (interactive) {
    instance.value = CreateMutexW(nullptr, FALSE, L"Local\\TaskManagerNative.Instance");
    const DWORD error = GetLastError();
    if ((instance.value && error == ERROR_ALREADY_EXISTS) || (!instance.value && error == ERROR_ACCESS_DENIED)) {
      for (int attempt = 0; attempt < 100; ++attempt) {
        if (const auto existing = FindWindowW(L"TaskManagerNative", nullptr)) { ShowWindowAsync(existing, SW_RESTORE); ShowWindowAsync(existing, SW_SHOW); SetForegroundWindow(existing); return 0; }
        Sleep(50);
      }
      return 1;
    }
    if (!instance.value) return 1;
  }
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_LINK_CLASS}; InitCommonControlsEx(&controls);
  const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0; Gdiplus::GdiplusStartup(&token, &input, nullptr);
  WSADATA sockets{}; WSAStartup(MAKEWORD(2, 2), &sockets);
  int result = 0;
  try {
#ifdef TASKMGR_DIAGNOSTICS
    result = interactive ? taskmgr::runApplication() : taskmgr::diagnostics(values);
#else
    result = taskmgr::runApplication();
#endif
  } catch (const std::exception& error) {
    if (interactive) MessageBoxA(nullptr, error.what(), "Task Manager", MB_OK | MB_ICONERROR);
#ifdef TASKMGR_DIAGNOSTICS
    else { auto report = std::find(values.begin(), values.end(), L"--output"); if (report != values.end() && report + 1 != values.end()) { taskmgr::Report stream{*(report + 1)}; stream << "{\"native\":true,\"passed\":false,\"error\":\"" << error.what() << "\"}"; } }
#endif
    result = 1;
  }
  WSACleanup(); Gdiplus::GdiplusShutdown(token); if (SUCCEEDED(initialized)) CoUninitialize(); return result;
}
