#include "core.hpp"
#include <commdlg.h>
#include <uxtheme.h>
#include <shlobj.h>
#include <iomanip>
#include <windowsx.h>

namespace taskmgr {
constexpr int SearchId = 10, TabsId = 11, ListId = 12, ResourcesId = 13, EndId = 14, CompactId = 15;
constexpr int RunId = 100, ExitId = 101, RefreshId = 102, TopmostId = 103, HideId = 104, GroupId = 105, SpeedHigh = 106, SpeedNormal = 107, SpeedLow = 108, SpeedPause = 109;
static const wchar_t* tabNames[] = {L"Processes", L"Performance", L"App history", L"Startup", L"Users", L"Details", L"Services"};
struct Input { std::wstring text; bool elevated = false; };
static INT_PTR CALLBACK inputProcedure(HWND dialog, UINT message, WPARAM word, LPARAM data) {
  auto input = reinterpret_cast<Input*>(GetWindowLongPtrW(dialog, DWLP_USER));
  if (message == WM_INITDIALOG) { input = reinterpret_cast<Input*>(data); SetWindowLongPtrW(dialog, DWLP_USER, data); SetDlgItemTextW(dialog, 1001, input->text.c_str()); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == 1002) { wchar_t path[32768]{}; OPENFILENAMEW file{sizeof(file)}; file.hwndOwner = dialog; file.lpstrFile = path; file.nMaxFile = DWORD(std::size(path)); file.lpstrFilter = L"Programs\0*.exe\0All files\0*.*\0"; file.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR; if (GetOpenFileNameW(&file)) SetDlgItemTextW(dialog, 1001, (L"\"" + std::wstring(path) + L"\"").c_str()); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == IDOK) { wchar_t text[32768]{}; GetDlgItemTextW(dialog, 1001, text, int(std::size(text))); input->text = text; input->elevated = IsDlgButtonChecked(dialog, 1003) == BST_CHECKED; EndDialog(dialog, IDOK); return TRUE; }
  if (message == WM_COMMAND && LOWORD(word) == IDCANCEL) { EndDialog(dialog, IDCANCEL); return TRUE; }
  return FALSE;
}
static INT_PTR CALLBACK reportProcedure(HWND dialog, UINT message, WPARAM word, LPARAM data) { if (message == WM_INITDIALOG) { SetDlgItemTextW(dialog, 1001, reinterpret_cast<const wchar_t*>(data)); return TRUE; } if (message == WM_COMMAND && (LOWORD(word) == IDOK || LOWORD(word) == IDCANCEL)) { EndDialog(dialog, IDOK); return TRUE; } return FALSE; }
struct Row { int process = -1; Entry entry; bool heading = false; int indent = 0; unsigned children = 0; std::wstring key; };
struct ActionResult { std::wstring error, report; };
class Application {
  HWND window = nullptr, search = nullptr, tabs = nullptr, list = nullptr, resources = nullptr, graph = nullptr, end = nullptr, compactButton = nullptr, statusLabel = nullptr;
  HFONT font = nullptr, titleFont = nullptr;
  std::unique_ptr<Icons> icons;
  std::shared_ptr<Sample> current;
  std::vector<Row> rows;
  std::unordered_map<std::wstring, History> histories;
  struct Usage { std::wstring name; uint64_t ticks = 0; };
  std::unordered_map<std::wstring, Usage> usage;
  std::unordered_map<Identity, uint64_t, IdentityHash> priorCpu;
  std::set<std::wstring> expanded;
  std::array<int, 7> sortColumn{};
  std::array<bool, 7> ascending{true, true, true, true, true, true, true};
  std::array<std::vector<int>, 7> columnWidths;
  std::wstring filter, prefix, preferences, historyFile;
  Time prefixAt{}, shown = Clock::now(), firstSample{}, lastSave = Clock::now();
  int selectedTab = 0, selectedResource = 0;
  int exitCode = 0;
  double graphTime = 0;
  bool topmost = false, hideMinimized = false, grouped = false, compact = false, hidden = false, closing = false, rebuilding = false;
  std::atomic<int> interval{1000};
  std::atomic<bool> minimized{false}, stopping{false}, notification{false};
  std::mutex workerMutex;
  std::condition_variable wake;
  bool force = false;
  std::shared_ptr<Sample> pending;
  Time published{};
  std::thread sampler;
  std::thread actionWorker;
  std::atomic<bool> actionBusy{false};
  std::mutex actionMutex;
  std::deque<ActionResult> actionResults;
  NOTIFYICONDATAW tray{sizeof(tray)};
  int benchmarkSeconds = 0;
  int warmupSeconds = 0;
  bool measured = false;
  DWORD baselineHandles = 0, baselineGdi = 0, baselineUser = 0;
  int benchmarkInterval = -1, benchmarkTab = -1;
  bool benchmarkMinimized = false;
  bool benchmarkIdle = false;
  std::thread inputProbe;
  Time lastInteraction{};
  std::wstring output;
  std::vector<double> deliveryTimes, updateTimes, paintTimes, searchTimes, samplerTimes, inputTimes;
  std::array<std::vector<double>, 6> stageTimes;
  unsigned updates = 0, tests = 0;
  bool keyboardPassed = false;
  FILETIME cpuStartKernel{}, cpuStartUser{};
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM word, LPARAM data);
  static LRESULT CALLBACK graphProcedure(HWND window, UINT message, WPARAM word, LPARAM data);
  static LRESULT CALLBACK listProcedure(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context);
  int scale(int value) const { return MulDiv(value, int(GetDpiForWindow(window)), 96); }
  void create();
  void updateFonts();
  void layout();
  void columns();
  void rebuild();
  void consume();
  void paint(HDC dc, RECT bounds);
  void command(int id);
  void menu(POINT point);
  void saveSettings();
  void saveHistory();
  void loadHistory();
  void worker();
  void refresh() { { std::lock_guard lock(workerMutex); force = true; } wake.notify_one(); }
  void asynchronous(std::function<std::wstring()> operation);
  const Process* selectedProcess() const;
  std::wstring cell(const Row& row, int column) const;
  std::wstring rowKey(int index) const { return index >= 0 && size_t(index) < rows.size() ? rows[size_t(index)].key : L""; }
  int selectedIndex() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
  void select(int index) { if (index < 0 || size_t(index) >= rows.size() || rows[size_t(index)].heading) return; ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(list, index, FALSE); }
  void letter(wchar_t character);
  void copy();
  void resourceSelection();
  void benchmarkTick();
  void writeBenchmark();
public:
  Application(bool invisible, int seconds, std::wstring report, int refreshInterval, int tab, bool startMinimized, bool idle, int warmup) : hidden(invisible), benchmarkSeconds(seconds), warmupSeconds(warmup), benchmarkInterval(refreshInterval), benchmarkTab(tab), benchmarkMinimized(startMinimized), benchmarkIdle(idle), output(std::move(report)) {}
  ~Application();
  int run();
};
Application::~Application() {
  stopping = true; wake.notify_all(); if (sampler.joinable()) sampler.join();
  if (inputProbe.joinable()) inputProbe.join();
  if (actionWorker.joinable()) actionWorker.join();
  icons.reset(); if (font) DeleteObject(font); if (titleFont) DeleteObject(titleFont);
}
int Application::run() {
  WNDCLASSEXW klass{sizeof(klass)}; klass.lpfnWndProc = procedure; klass.hInstance = GetModuleHandleW(nullptr); klass.hCursor = LoadCursorW(nullptr, IDC_ARROW); klass.hIcon = LoadIconW(nullptr, IDI_APPLICATION); klass.lpszClassName = L"TaskManagerNative"; klass.hbrBackground = GetSysColorBrush(COLOR_WINDOW); RegisterClassExW(&klass);
  klass.lpfnWndProc = graphProcedure; klass.lpszClassName = L"TaskManagerGraph"; RegisterClassExW(&klass);
  wchar_t local[MAX_PATH]{}; SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local);
  std::filesystem::path directory = std::filesystem::path(local) / L"TaskManagerNative";
  if (!hidden) { std::filesystem::create_directories(directory); preferences = (directory / L"settings.ini").wstring(); historyFile = (directory / L"history.tsv").wstring(); loadHistory(); }
  selectedTab = hidden ? 0 : std::clamp(int(GetPrivateProfileIntW(L"Window", L"Tab", 0, preferences.c_str())), 0, 6);
  interval = hidden ? 1000 : int(GetPrivateProfileIntW(L"Window", L"Interval", 1000, preferences.c_str()));
  if (interval != 0 && interval != 500 && interval != 1000 && interval != 4000) interval = 1000;
  if (benchmarkInterval >= 0) interval = benchmarkInterval;
  topmost = !hidden && GetPrivateProfileIntW(L"Window", L"Topmost", 0, preferences.c_str()); hideMinimized = !hidden && GetPrivateProfileIntW(L"Window", L"Hide", 0, preferences.c_str());
  grouped = !hidden && GetPrivateProfileIntW(L"Window", L"Group", 0, preferences.c_str());
  const int width = hidden ? 1040 : std::clamp(int(GetPrivateProfileIntW(L"Window", L"Width", 1040, preferences.c_str())), 640, 2400);
  const int height = hidden ? 820 : std::clamp(int(GetPrivateProfileIntW(L"Window", L"Height", 820, preferences.c_str())), 400, 1600);
  window = CreateWindowExW(topmost ? WS_EX_TOPMOST : 0, L"TaskManagerNative", L"Task Manager", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height, nullptr, nullptr, klass.hInstance, this);
  if (!window) return 1;
  if (!hidden) ShowWindow(window, SW_SHOW);
  minimized = benchmarkMinimized;
  FILETIME created{}, exited{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &cpuStartKernel, &cpuStartUser);
  sampler = std::thread([this] { worker(); }); SetTimer(window, 1, hidden ? 100 : 1000, nullptr);
  if (hidden) inputProbe = std::thread([this] { while (!stopping) { LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); PostMessageW(window, WM_APP + 5, WPARAM(counter.QuadPart), 0); Sleep(50); } });
  MSG message{}; while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && message.wParam == 'F') { SetFocus(search); SendMessageW(search, EM_SETSEL, 0, -1); continue; }
    if (message.message == WM_KEYDOWN && message.wParam == VK_F5) { refresh(); continue; }
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { SetWindowTextW(search, L""); continue; }
    if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && (GetKeyState(VK_MENU) & 0x8000) && message.wParam == 'F') { SetFocus(search); continue; }
    if ((message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) && (GetKeyState(VK_MENU) & 0x8000) && message.wParam >= '1' && message.wParam <= '7') { saveSettings(); selectedTab = int(message.wParam - '1'); TabCtrl_SetCurSel(tabs, selectedTab); columns(); rebuild(); layout(); continue; }
    if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && message.wParam == VK_TAB) { saveSettings(); selectedTab = (selectedTab + ((GetKeyState(VK_SHIFT) & 0x8000) ? 6 : 1)) % 7; TabCtrl_SetCurSel(tabs, selectedTab); columns(); rebuild(); layout(); continue; }
    if (message.message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (GetKeyState(VK_SHIFT) & 0x8000) && message.wParam == 'N') { command(RunId); continue; }
    TranslateMessage(&message); DispatchMessageW(&message);
  }
  return exitCode ? exitCode : int(message.wParam);
}
void Application::create() {
  font = CreateFontW(-scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  titleFont = CreateFontW(-scale(27), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  auto control = [&](const wchar_t* klass, const wchar_t* title, DWORD style, int id, DWORD extra = 0) { auto created = CreateWindowExW(extra, klass, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr); SendMessageW(created, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); return created; };
  search = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, SearchId, WS_EX_CLIENTEDGE);
  SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search name, PID or publisher (Alt+F)"));
  tabs = control(WC_TABCONTROLW, L"", WS_TABSTOP, TabsId);
  for (int index = 0; index < 7; ++index) { TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tabNames[index]); TabCtrl_InsertItem(tabs, index, &item); }
  TabCtrl_SetCurSel(tabs, selectedTab);
  list = control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_TABSTOP, ListId);
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_HEADERDRAGDROP | LVS_EX_LABELTIP);
  SetWindowTheme(list, L"Explorer", nullptr); SetWindowSubclass(list, listProcedure, 1, reinterpret_cast<DWORD_PTR>(this));
  resources = control(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_TABSTOP, ResourcesId);
  SendMessageW(resources, LB_SETITEMHEIGHT, 0, scale(74));
  graph = control(L"TaskManagerGraph", L"", 0, 16); SetWindowLongPtrW(graph, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  compactButton = control(L"BUTTON", L"Fewer details", BS_PUSHBUTTON | WS_TABSTOP, CompactId);
  end = control(L"BUTTON", L"End task", BS_PUSHBUTTON | WS_TABSTOP, EndId);
  statusLabel = control(L"STATIC", L"Starting native sampler…", SS_LEFT, 17);
  icons = std::make_unique<Icons>(window); ListView_SetImageList(list, icons->images, LVSIL_SMALL);
  auto bar = CreateMenu(), file = CreatePopupMenu(), options = CreatePopupMenu(), view = CreatePopupMenu(), speed = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, RunId, L"Run new task\tCtrl+Shift+N"); AppendMenuW(file, MF_STRING, ExitId, L"Exit");
  AppendMenuW(options, MF_STRING | (topmost ? MF_CHECKED : 0), TopmostId, L"Always on top"); AppendMenuW(options, MF_STRING | (hideMinimized ? MF_CHECKED : 0), HideId, L"Hide when minimized");
  AppendMenuW(view, MF_STRING, RefreshId, L"Refresh now\tF5"); AppendMenuW(view, MF_STRING | (grouped ? MF_CHECKED : 0), GroupId, L"Group processes by type");
  AppendMenuW(speed, MF_STRING, SpeedHigh, L"High (500 ms)"); AppendMenuW(speed, MF_STRING, SpeedNormal, L"Normal (1 second)"); AppendMenuW(speed, MF_STRING, SpeedLow, L"Low (4 seconds)"); AppendMenuW(speed, MF_STRING, SpeedPause, L"Paused");
  AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"Update speed"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(options), L"Options"); AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View"); SetMenu(window, bar);
  tray.hWnd = window; tray.uID = 1; tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray.uCallbackMessage = WM_APP + 4; tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION); wcscpy_s(tray.szTip, L"Task Manager");
  if (!hidden) Shell_NotifyIconW(NIM_ADD, &tray);
  for (int tab = 0; tab < 7; ++tab) { const auto section = std::to_wstring(tab); if (!hidden) { sortColumn[size_t(tab)] = std::clamp(int(GetPrivateProfileIntW(section.c_str(), L"Sort", 0, preferences.c_str())), 0, 15); ascending[size_t(tab)] = GetPrivateProfileIntW(section.c_str(), L"Ascending", 1, preferences.c_str()) != 0; } }
  columns(); layout();
}
void Application::layout() {
  RECT area{}; GetClientRect(window, &area); const int width = area.right, height = area.bottom;
  ShowWindow(search, compact ? SW_HIDE : SW_SHOW); ShowWindow(tabs, compact ? SW_HIDE : SW_SHOW);
  MoveWindow(search, scale(12), scale(8), std::max(1, width - scale(24)), scale(25), TRUE);
  MoveWindow(tabs, scale(5), scale(42), width - scale(10), scale(28), TRUE);
  const int top = compact ? scale(4) : scale(83), bottom = std::max(top + 1, height - scale(48));
  const bool performance = selectedTab == 1 && !compact;
  ShowWindow(list, performance ? SW_HIDE : SW_SHOW); ShowWindow(resources, performance ? SW_SHOW : SW_HIDE); ShowWindow(graph, performance ? SW_SHOW : SW_HIDE);
  MoveWindow(list, scale(8), top, std::max(1, width - scale(16)), bottom - top, TRUE);
  MoveWindow(resources, scale(8), top, scale(215), bottom - top, TRUE);
  MoveWindow(graph, scale(235), top, std::max(1, width - scale(247)), bottom - top, TRUE);
  MoveWindow(compactButton, scale(12), height - scale(35), scale(120), scale(25), TRUE);
  MoveWindow(statusLabel, scale(146), height - scale(32), std::max(1, width - scale(270)), scale(24), TRUE);
  MoveWindow(end, width - scale(102), height - scale(35), scale(90), scale(25), TRUE);
  ShowWindow(end, selectedTab == 1 || selectedTab == 2 ? SW_HIDE : SW_SHOW);
  SetWindowTextW(end, selectedTab == 3 ? L"Enable/disable" : selectedTab == 4 ? L"Disconnect" : selectedTab == 6 ? L"Start/stop" : L"End task");
  InvalidateRect(graph, nullptr, FALSE);
}
void Application::columns() {
  rebuilding = true; SendMessageW(list, WM_SETREDRAW, FALSE, 0); ListView_SetItemCountEx(list, 0, LVSICF_NOINVALIDATEALL); rows.clear();
  while (Header_GetItemCount(ListView_GetHeader(list))) ListView_DeleteColumn(list, 0);
  std::vector<std::pair<std::wstring, int>> fields;
  switch (selectedTab) {
  case 0: fields = {{L"Name", 300}, {L"Status", 90}, {L"PID", 70}, {L"CPU", 75}, {L"Memory", 115}, {L"I/O", 100}, {L"GPU", 70}, {L"Publisher", 180}}; break;
  case 2: fields = {{L"Name", 320}, {L"CPU time", 120}, {L"Network", 120}}; break;
  case 3: fields = {{L"Name", 320}, {L"Publisher", 180}, {L"Status", 110}, {L"Startup impact", 140}}; break;
  case 4: fields = {{L"User / process", 300}, {L"Session / PID", 110}, {L"Status", 120}, {L"CPU", 100}, {L"Memory", 130}}; break;
  case 5: fields = {{L"Name", 210}, {L"PID", 75}, {L"Status", 95}, {L"User name", 140}, {L"CPU", 75}, {L"Memory", 120}, {L"Description", 260}, {L"Session", 75}, {L"Threads", 75}, {L"Handles", 75}, {L"CPU time", 110}, {L"I/O", 100}, {L"GPU", 75}}; break;
  case 6: fields = {{L"Name", 240}, {L"PID", 85}, {L"Description", 350}, {L"Status", 110}}; break;
  default: break;
  }
  const auto section = std::to_wstring(selectedTab); auto& widths = columnWidths[size_t(selectedTab)];
  for (size_t index = 0; index < fields.size(); ++index) {
    const auto key = L"Width" + std::to_wstring(index); int width = index < widths.size() ? widths[index] : hidden ? fields[index].second : int(GetPrivateProfileIntW(section.c_str(), key.c_str(), fields[index].second, preferences.c_str()));
    width = std::clamp(width, 0, 2000); LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = fields[index].first.data(); column.cx = scale(width); column.fmt = index == 0 ? LVCFMT_LEFT : LVCFMT_LEFT; ListView_InsertColumn(list, int(index), &column);
  }
  SendMessageW(list, WM_SETREDRAW, TRUE, 0); rebuilding = false;
}
#include "ui.inc"
