#pragma once
#include "core.hpp"

namespace taskmgr {
enum Tab { ProcessesTab, PerformanceTab, HistoryTab, StartupTab, UsersTab, DetailsTab, ServicesTab, TabCount };
enum class RowKind { Heading, Process, Group, Service, Entry, User, Interrupts, Window };
struct Cell { std::wstring text; double value = 0; int heat = -1; };
struct Row {
  RowKind kind = RowKind::Process;
  std::wstring key;
  std::vector<Cell> cells;
  int icon = 0, depth = 0, process = -1, entry = -1, category = -1;
  bool expandable = false, expanded = false;
  std::vector<int> members;
  HWND window = nullptr;
};
struct ColumnDef { const wchar_t* title; int width; bool right, visible, heat; };
struct PerfItem { std::wstring key, title, subtitle, value, description; COLORREF color; };
struct ActionResult { std::wstring error, report; };
constexpr int FewerId = 15, EndId = 14, LinkId = 18, HistoryLinkId = 19, ListId = 12, TabsId = 11, PerformanceId = 13;
constexpr int StartupManageId = 241;
constexpr int RunId = 100, ExitId = 101, RefreshId = 102, TopmostId = 103, MinimizeOnUseId = 104, HideId = 105, GroupId = 106, ExpandAllId = 107, CollapseAllId = 108;
constexpr int SpeedHigh = 110, SpeedNormal = 111, SpeedLow = 112, SpeedPause = 113, DefaultTabId = 120, FullNameId = 130, AllHistoryId = 131, ColumnId = 300;
constexpr int ReplaceDefaultId = 132;
constexpr int ThemeLightId = 133, ThemeDarkId = 134, ThemeSystemId = 135, SearchId = 136;
constexpr int EfficiencyId = 137;
enum Command { EndTask = 200, EndTree, RestartExplorer, SwitchTo, BringToFront, MinimizeWindow, MaximizeWindow, Toggle, GoToDetails, GoToServices, OpenLocation, SearchOnline, Properties, CreateDump, WaitChainId, AffinityId, CopyId,
  PriorityRealtime = 220, PriorityHigh, PriorityAbove, PriorityNormal, PriorityBelow, PriorityLow, ServiceStart = 230, ServiceStop, ServiceRestart, OpenServices, StartupToggle = 240, Disconnect = 250, SignOff, ManageAccounts,
  MemoryValues = 260, MemoryPercents, NetworkValues, NetworkPercents, DeleteHistory, SummaryView = 400, OverallView, LogicalView, CopyPerformance, HideGraphs, SidebarSummary, ViewResource = 410, TrayRestore = 500 };
constexpr COLORREF CpuColor = RGB(17, 125, 187), MemoryColor = RGB(139, 18, 174), DiskColor = RGB(77, 166, 12), NetworkColor = RGB(167, 79, 1), GpuColor = RGB(17, 125, 187);
class Application {
public:
#ifdef TASKMGR_DIAGNOSTICS
  explicit Application(RunOptions options);
#else
  Application() = default;
#endif
  ~Application();
  int run();
private:
#ifdef TASKMGR_DIAGNOSTICS
  RunOptions options;
#endif
  HWND window = nullptr, tabs = nullptr, list = nullptr, header = nullptr, performance = nullptr, end = nullptr, fewer = nullptr, link = nullptr, historyLink = nullptr;
  HMENU menuBar = nullptr;
  HFONT font = nullptr, percentFont = nullptr, headingFont = nullptr, titleFont = nullptr, subtitleFont = nullptr, labelFont = nullptr, valueFont = nullptr, sidebarFont = nullptr, linkFont = nullptr;
  HIMAGELIST rowSpacer = nullptr;
  HICON windowIcon = nullptr, smallIcon = nullptr, trayIcon = nullptr;
  std::unique_ptr<Icons> icons;
  std::shared_ptr<Sample> current;
  mutable std::vector<PerfItem> performanceItems;
  mutable bool performanceItemsDirty = true;
  bool performanceTracking = false;
  int hotResource = -1;
  std::vector<Row> rows;
  std::vector<Row> searchRows;
  HWND searchBox = nullptr;
  std::wstring searchText;
  bool searchVisible = false, searchQueued = false, dark = false, highContrast = false;
  int theme = 0;
  HBRUSH backgroundBrush = nullptr;
  std::unordered_map<std::wstring, History> histories;
  struct Usage { std::wstring name, path; uint64_t ticks = 0; bool app = false; };
  std::unordered_map<std::wstring, Usage> usage;
  struct Activity { uint64_t ticks = 0, seen = 0; double trend = 0; };
  std::unordered_map<Identity, Activity, IdentityHash> activity;
  uint64_t activityGeneration = 0;
  std::set<std::wstring> expanded;
  std::array<int, TabCount> sortColumn{};
  std::array<bool, TabCount> ascending{};
  std::array<std::vector<int>, TabCount> shown;
  std::array<std::vector<int>, TabCount> widths;
  std::wstring prefix, preferences, historyFile, selectedResource = L"cpu";
  Time prefixAt{}, started = Clock::now(), historySince{}, sortDue{};
  int selectedTab = 0, defaultTab = 0, hotRow = -1, hotHeader = -1, sidebarScroll = 0, sidebarWheelDelta = 0, exitCode = 0, summary = 0, trayLevel = -1;
  UINT dpi = 96;
  int orderedTab = -1, orderedColumn = -1, displayCount = 0, columnsTab = -1, pendingTab = -1;
  bool tabQueued = false, startupRequested = false;
  bool orderedAscending = false, columnGeometryDirty = true, geometryQueued = false, columnTracking = false;
  unsigned columnMutationDepth = 0;
  std::array<int, 32> displayOrder{}, displayWidths{};
  double graphTime = 0, lastTrend = 0;
  bool topmost = false, minimizeOnUse = false, hideMinimized = false, groupByType = true, compact = false, closing = false, rebuilding = false, memoryPercent = false, networkPercent = false, logical = false, hideGraphs = false, fullName = false, allHistory = true, trayAdded = false;
  RECT normalRect{}, compactRect{};
  std::atomic<int> interval{1000};
  std::atomic<bool> minimized{false}, stopping{false}, notification{false}, startupVisible{false};
  std::mutex workerMutex;
  std::condition_variable wake;
  bool force = false, forceInventory = false;
  Time refreshAt{};
  std::shared_ptr<Sample> pending;
  std::thread sampler, actionWorker;
  std::atomic<bool> actionBusy{false};
  std::mutex actionMutex;
  std::deque<ActionResult> actionResults;
  NOTIFYICONDATAW tray{sizeof(tray)};
  bool sortQueued = false;
#ifdef TASKMGR_DIAGNOSTICS
  Time published{}, firstSample{};
  std::thread inputProbe;
  bool measured = false, keyboardPassed = false, sortingPassed = false, sortingChecked = false, verifyingSorting = false, benchmarkRunning = false;
  Time measurementStarted{};
  LONGLONG measurementCounter = 0;
  DWORD baselineHandles = 0, baselineGdi = 0, baselineUser = 0;
  std::vector<double> deliveryTimes, updateTimes, paintTimes, samplerTimes, inputTimes, sortTimes;
  std::vector<double> hoverTimes, tabTimes, columnTimes;
  std::string sortingFailure;
  std::array<std::vector<double>, 6> stageTimes;
  unsigned updates = 0, tests = 0, screenshotStep = 0, sortRequests = 0, fullRebuilds = 0, sortPasses = 0, headerMenuRequests = 0;
  mutable unsigned performanceItemBuilds = 0;
  unsigned performancePaints = 0, performanceDetailPaints = 0;
  unsigned headerPaints = 0, columnGeometryCommits = 0, scrollbarPaints = 0;
  unsigned tabRequests = 0, tabCommits = 0;
  FILETIME cpuStartKernel{}, cpuStartUser{};
#else
  static constexpr bool verifyingSorting = false;
#endif
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM word, LPARAM data);
  static LRESULT CALLBACK performanceProcedure(HWND window, UINT message, WPARAM word, LPARAM data);
  static LRESULT CALLBACK listProcedure(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context);
  static LRESULT CALLBACK headerProcedure(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context);
  static LRESULT CALLBACK linkProcedure(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context);
  static LRESULT CALLBACK tabsProcedure(HWND window, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context);
  COLORREF themeColor(COLORREF light) const;
  void applyTheme();
  void paintScrollbars(HDC destination = nullptr);
  void requestTab(int tab);
  void toggleSearch();
  void applySearch();
  std::vector<Row> filteredRows(const std::vector<Row>& source) const;
#ifdef TASKMGR_DIAGNOSTICS
  bool hidden() const { return options.hidden; }
  bool persistent() const { return !options.hidden && options.screenshots.empty(); }
#else
  static constexpr bool hidden() { return false; }
  static constexpr bool persistent() { return true; }
#endif
  int scale(int value) const { return MulDiv(value, int(dpi), 96); }
  // main.cpp
  void create();
  void createFonts();
  void layout();
  void selectTab(int tab);
  void setCompact(bool value);
  void setSummary(int value);
  void updateMenu(HMENU menu);
  void updateFooter();
  void updateTray();
  void paintWindow(HDC dc);
  void loadSettings();
  void saveSettings();
  void writeSettings();
  void captureColumns();
  void saveHistory();
  void loadHistory();
  void worker();
  void consume();
  void refresh(bool inventory = true) { { std::lock_guard lock(workerMutex); const auto now = Clock::now(); if (stopping || force || (refreshAt != Time{} && now - refreshAt < std::chrono::milliseconds(250))) return; refreshAt = now; force = true; forceInventory = inventory; } wake.notify_one(); }
  // table.cpp
  static std::span<const ColumnDef> definitions(int tab);
  bool twoLineHeader() const { return (selectedTab == ProcessesTab || selectedTab == UsersTab) && !compact; }
  int rowHeight() const;
  int columnAt(int display) const;
  int minimumColumnWidth(int tab, int column) const;
  void columns(bool preserveRows = false);
  void updateColumnGeometry();
  void rebuild(bool sortOnly = false, bool filterOnly = false);
  void sortVisibleRows();
  void buildProcesses(std::vector<Row>& next);
  void buildUsers(std::vector<Row>& next);
  void buildDetails(std::vector<Row>& next);
  void buildEntries(std::vector<Row>& next);
  std::wstring processName(int index, const std::unordered_map<DWORD, std::vector<const Entry*>>& services) const;
  void processCells(Row& row, std::span<const int> members) const;
  void sortRows(std::vector<Row>& items) const;
  double powerScore(const Process& process) const;
  void drawRow(HDC dc, int index, RECT bounds);
  void paintHeader(HDC dc, RECT client);
  std::wstring headerValue(int column) const;
  const std::wstring& cell(const Row& row, int column) const { static const std::wstring empty; return column >= 0 && size_t(column) < row.cells.size() ? row.cells[size_t(column)].text : empty; }
  int findRow(std::wstring_view prefix, int start) const;
  std::wstring rowKey(int index) const { return index >= 0 && size_t(index) < rows.size() ? rows[size_t(index)].key : L""; }
  int selectedIndex() const { return ListView_GetNextItem(list, -1, LVNI_SELECTED); }
  const Row* selectedRow() const { const int index = selectedIndex(); return index >= 0 && size_t(index) < rows.size() ? &rows[size_t(index)] : nullptr; }
  const Process* selectedProcess() const;
  std::vector<const Process*> selectedMembers() const;
  void select(int index);
  void toggle(int index);
  void letter(wchar_t character);
  void copy();
  void invalidateRow(int index);
  // performance.cpp
  const std::vector<PerfItem>& perfItems() const;
  int performanceHitTest(POINT point) const;
  RECT performanceRowRect(int index) const;
  void performanceHover(int index);
  void paintPerformance(HDC dc, RECT bounds);
  void paintSidebar(HDC dc, RECT bounds, const std::vector<PerfItem>& items);
  void paintDetail(HDC dc, RECT bounds, const PerfItem& item);
  void performanceClick(POINT point, bool right, bool twice);
  void performanceKey(WPARAM key);
  std::wstring performanceSummary() const;
  void recordHistories(double now);
  // commands.cpp
  void command(int id);
  void menu(POINT point);
  void headerMenu(POINT point);
  HMENU createHeaderMenu() const;
  void performanceMenu(POINT point, bool sidebar);
  void asynchronous(std::function<std::wstring()> operation);
  bool confirm(const std::wstring& title, const std::wstring& text, const std::wstring& button);
  void endSelected(bool tree);
  void runTask();
  HWND appWindow(const Process& process) const;
  // diagnostics
#ifdef TASKMGR_DIAGNOSTICS
  void benchmarkTick();
  bool verifySorting();
  bool verifyPerformanceHover();
  bool verifyTabSwitching();
  void writeBenchmark();
  void screenshotTick();
#endif
};
}
