#include "app.hpp"
#include "report.hpp"
#include <dwmapi.h>
#include <windowsx.h>

namespace taskmgr {
bool Application::verifySorting() {
  verifyingSorting = true;
  const int savedTab = selectedTab; const bool savedGrouping = groupByType; const auto savedExpanded = expanded;
  const auto savedColumns = sortColumn; const auto savedDirections = ascending;
  bool passed = true;
  auto check = [&](bool result, const std::string& name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  const bool priorMeasured = measured; const unsigned priorTests = tests;
  benchmarkTick(); check(measured == priorMeasured && tests == priorTests, "benchmark-nested-tick-skipped");
  check(activity.size() == current->processes.size(), "activity-live-count");
  check(verifyPerformanceHover(), "performance-hover");
  check(verifyTabSwitching(), "tab-switching");
  for (const auto& process : current->processes) { const auto found = activity.find({process.id, process.created}); check(found != activity.end() && found->second.ticks == process.cpuTicks && found->second.seen == activityGeneration && std::isfinite(found->second.trend), "activity-live-identity"); }
  for (const int tab : {ProcessesTab, UsersTab, DetailsTab, HistoryTab, StartupTab, ServicesTab}) {
    selectTab(tab);
    for (size_t index = 0; index < rows.size(); ++index) if (rows[index].kind != RowKind::Heading) { select(int(index)); break; }
    const auto savedShown = shown[size_t(tab)]; const auto savedWidths = widths[size_t(tab)];
    const auto* cachedRows = rows.data(); const auto cachedCount = rows.size(); const auto cachedSample = current;
    const auto priorRebuilds = fullRebuilds; const auto priorSorts = sortPasses;
    const auto priorSpacer = rowSpacer; const auto priorSelection = rowKey(selectedIndex());
    const int priorTop = ListView_GetTopIndex(list);
    const unsigned priorMenus = headerMenuRequests;
    SendMessageW(header, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(header), MAKELPARAM(20, 20));
    SendMessageW(header, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(header), MAKELPARAM(-1, -1));
    check(headerMenuRequests == priorMenus + 2, "header-context-menu-routing");
    RECT titleBounds{}; Header_GetItemRect(header, 0, &titleBounds);
    const LPARAM titlePoint = MAKELPARAM((titleBounds.left + titleBounds.right) / 2, (titleBounds.top + titleBounds.bottom) / 2);
    SendMessageW(header, WM_RBUTTONDOWN, MK_RBUTTON, titlePoint); SendMessageW(header, WM_RBUTTONUP, 0, titlePoint);
    check(headerMenuRequests == priorMenus + 3, "header-right-click-routing");
    auto checkMenu = [&] {
      HMENU popup = createHeaderMenu(); check(popup != nullptr, "column-menu-created");
      if (!popup) return;
      const auto defs = definitions(tab);
      check(GetMenuItemCount(popup) == int(defs.size()) + (tab == ProcessesTab ? 2 : 0), "column-menu-count");
      for (size_t column = 0; column < defs.size(); ++column) {
        const UINT state = GetMenuState(popup, UINT(ColumnId + column), MF_BYCOMMAND);
        const bool visible = std::find(shown[size_t(tab)].begin(), shown[size_t(tab)].end(), int(column)) != shown[size_t(tab)].end();
        check(state != UINT(-1) && bool(state & MF_CHECKED) == visible, "column-menu-checkmark");
        check(bool(state & (MF_DISABLED | MF_GRAYED)) == (column == 0), "column-menu-name-required");
        wchar_t title[128]{}; GetMenuStringW(popup, UINT(ColumnId + column), title, int(std::size(title)), MF_BYCOMMAND);
        check(std::wstring_view(title) == defs[column].title, "column-menu-label");
      }
      DestroyMenu(popup);
    };
    checkMenu();
    command(ColumnId); command(ColumnId + int(definitions(tab).size()));
    check(shown[size_t(tab)] == savedShown && widths[size_t(tab)] == savedWidths, "invalid-column-no-op");
    for (size_t column = 1; column < definitions(tab).size(); ++column) {
      const auto before = shown[size_t(tab)].size();
      const bool visible = std::find(shown[size_t(tab)].begin(), shown[size_t(tab)].end(), int(column)) != shown[size_t(tab)].end();
      command(ColumnId + int(column));
      check(shown[size_t(tab)].size() == (visible ? before - 1 : before + 1), "column-toggle");
      check(Header_GetItemCount(header) == int(shown[size_t(tab)].size()), "column-native-count");
      checkMenu(); command(ColumnId + int(column));
      check(rows.data() == cachedRows && rows.size() == cachedCount && current == cachedSample, "column-cached-snapshot");
      check(fullRebuilds == priorRebuilds && sortPasses == priorSorts && rowSpacer == priorSpacer, "column-no-resampling-or-rebuild");
      check(rowKey(selectedIndex()) == priorSelection && ListView_GetTopIndex(list) == priorTop, "column-selection-scroll");
    }
    shown[size_t(tab)] = savedShown; widths[size_t(tab)] = savedWidths; columns(true);
    for (const bool grouped : {false, true}) {
      if (grouped && tab != ProcessesTab) continue;
      groupByType = grouped; expanded.clear(); rebuild();
      for (const auto& row : rows) if (row.expandable) expanded.insert(row.key);
      rebuild();
      for (size_t column = 0; column < definitions(tab).size(); ++column) for (const bool direction : {false, true}) {
        for (size_t index = 0; index < rows.size(); ++index) if (rows[index].kind != RowKind::Heading) { select(int(index)); break; }
        const auto selected = rowKey(selectedIndex());
        sortColumn[size_t(tab)] = int(column); ascending[size_t(tab)] = direction; rebuild(true);
        check(rowKey(selectedIndex()) == selected, "selection");
        std::vector<std::wstring> actual; actual.reserve(rows.size()); for (const auto& row : rows) actual.push_back(row.key);
        rebuild();
        check(rows.size() == actual.size(), "row-count");
        for (size_t index = 0; index < std::min(rows.size(), actual.size()); ++index) check(rows[index].key == actual[index], "order-tab-" + std::to_string(tab) + "-column-" + std::to_string(column));
      }
    }
  }
  selectTab(DetailsTab);
  const auto cpuColumn = std::find(shown[DetailsTab].begin(), shown[DetailsTab].end(), 5);
  if (cpuColumn != shown[DetailsTab].end()) {
    const int display = int(cpuColumn - shown[DetailsTab].begin());
    sortColumn[DetailsTab] = 0; ascending[DetailsTab] = true; rebuild();
    if (!rows.empty()) select(int(rows.size()) - 1);
    const auto selected = rowKey(selectedIndex()); const auto snapshot = current; const unsigned builds = fullRebuilds;
    const unsigned priorGeometryCommits = columnGeometryCommits;
    RECT bounds{}; Header_GetItemRect(header, display, &bounds); const LPARAM point = MAKELPARAM((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, point); SendMessageW(header, WM_LBUTTONUP, 0, point); SendMessageW(window, WM_TIMER, 3, 0);
    check(sortColumn[DetailsTab] == 5 && !ascending[DetailsTab], "native-header-click"); check(ListView_GetTopIndex(list) == 0, "manual-sort-scroll"); check(rowKey(selectedIndex()) == selected && current == snapshot && fullRebuilds == builds, "cached-header-click");
    SendMessageW(header, WM_LBUTTONDBLCLK, MK_LBUTTON, point); SendMessageW(header, WM_LBUTTONUP, 0, point); Sleep(40);
    NMLISTVIEW repeated{}; repeated.hdr = {list, ListId, LVN_COLUMNCLICK}; repeated.iSubItem = display; SendMessageW(window, WM_NOTIFY, ListId, LPARAM(&repeated));
    check(!sortQueued && !orderedAscending, "continuous-input-sort-deadline");
    SendMessageW(header, WM_LBUTTONDBLCLK, MK_LBUTTON, point); SendMessageW(header, WM_LBUTTONUP, 0, point); SendMessageW(window, WM_TIMER, 3, 0);
    check(ascending[DetailsTab] && orderedAscending, "native-header-double-click");
    check(columnGeometryCommits == priorGeometryCommits, "native-header-sort-no-geometry-repaint");
    const unsigned passes = sortPasses; const auto* storage = rows.data();
    for (int click = 0; click < 128; ++click) { NMLISTVIEW notice{}; notice.hdr = {list, ListId, LVN_COLUMNCLICK}; notice.iSubItem = display; SendMessageW(window, WM_NOTIFY, ListId, LPARAM(&notice)); }
    SendMessageW(window, WM_TIMER, 3, 0);
    check(rows.data() == storage && sortPasses == passes && fullRebuilds == builds && current == snapshot && !sortQueued, "cancelled-sort-burst");
    const int width = ListView_GetColumnWidth(list, display); ListView_SetColumnWidth(list, display, width + 11); updateColumnGeometry();
    bool widthFound = false; for (int position = 0; position < displayCount; ++position) if (displayOrder[size_t(position)] == display) widthFound = displayWidths[size_t(position)] == width + 11;
    check(widthFound, "column-width-cache"); ListView_SetColumnWidth(list, display, width);
    for (int repeat = 0; repeat < 128; ++repeat) {
      const int nextWidth = width + repeat % 19;
      ListView_SetColumnWidth(list, display, nextWidth);
      check(!geometryQueued && !columnGeometryDirty, "column-resize-synchronous-geometry");
      bool aligned = false; for (int position = 0; position < displayCount; ++position) if (displayOrder[size_t(position)] == display) aligned = displayWidths[size_t(position)] == nextWidth;
      check(aligned && current == snapshot && fullRebuilds == builds, "column-resize-no-resampling-or-rebuild");
    }
    ListView_SetColumnWidth(list, display, width);
    std::array<int, 32> originalOrder{}, reversedOrder{};
    const int columnCount = Header_GetItemCount(header);
    check(columnCount <= int(originalOrder.size()) && ListView_GetColumnOrderArray(list, columnCount, originalOrder.data()), "column-original-order");
    for (int position = 0; position < columnCount; ++position) reversedOrder[size_t(position)] = originalOrder[size_t(columnCount - position - 1)];
    check(ListView_SetColumnOrderArray(list, columnCount, reversedOrder.data()), "column-reorder");
    check(!geometryQueued && !columnGeometryDirty && std::equal(reversedOrder.begin(), reversedOrder.begin() + columnCount, displayOrder.begin()), "column-reorder-synchronous-geometry");
    ListView_SetColumnOrderArray(list, columnCount, originalOrder.data());
    check(!(GetWindowLongPtrW(list, GWL_EXSTYLE) & WS_EX_COMPOSITED) && (ListView_GetExtendedListViewStyle(list) & LVS_EX_DOUBLEBUFFER), "bounded-table-buffering");
    check(GetWindowLongPtrW(header, GWL_STYLE) & HDS_FULLDRAG, "native-live-column-resize");
    const int minimum = scale(minimumColumnWidth(DetailsTab, 5));
    ListView_SetColumnWidth(list, display, 0);
    check(ListView_GetColumnWidth(list, display) == minimum, "column-programmatic-minimum");
    HDITEMW narrow{}; narrow.mask = HDI_WIDTH; narrow.cxy = 1; Header_SetItem(header, display, &narrow);
    check(ListView_GetColumnWidth(list, display) == minimum, "column-header-minimum");
    ListView_SetColumnWidth(list, display, width + scale(50));
    RECT divider{}; Header_GetItemRect(header, display, &divider);
    const int dividerY = (divider.top + divider.bottom) / 2;
    const bool previousSortQueued = sortQueued; sortQueued = true;
    SendMessageW(header, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(divider.right - 1, dividerY));
    check(GetCapture() == header, "column-divider-capture");
    for (int repeat = 0; repeat < 128; ++repeat) {
      const int requested = repeat % 2 ? minimum + scale(30) + repeat % 17 : 1;
      const auto resizeStarted = Clock::now();
      const unsigned previousHeaderPaints = headerPaints;
      SendMessageW(header, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(divider.left + requested - 1, dividerY));
      columnTimes.push_back(milliseconds(resizeStarted));
      const int actualWidth = ListView_GetColumnWidth(list, display);
      check(actualWidth == std::max(minimum, requested), "column-native-drag-minimum-and-live-width");
      check(headerPaints > previousHeaderPaints, "column-native-drag-paints-despite-pending-sort");
      check(!geometryQueued && !columnGeometryDirty && !columnMutationDepth && !GetUpdateRect(list, nullptr, FALSE) && !GetUpdateRect(header, nullptr, FALSE), "column-native-drag-complete-frame");
      bool aligned = false; for (int position = 0; position < displayCount; ++position) if (displayOrder[size_t(position)] == display) aligned = displayWidths[size_t(position)] == actualWidth;
      check(aligned && current == snapshot && fullRebuilds == builds && rows.data() == storage, "column-native-drag-cached-only");
      RECT rowBounds{}; ListView_GetItemRect(list, 0, &rowBounds, LVIR_BOUNDS);
      int rowLeft = rowBounds.left;
      for (int position = 0; position < displayCount; ++position) {
        RECT headerBounds{}; Header_GetItemRect(header, displayOrder[size_t(position)], &headerBounds);
        MapWindowPoints(header, list, reinterpret_cast<POINT*>(&headerBounds), 2);
        check(headerBounds.left == rowLeft && headerBounds.right == rowLeft + displayWidths[size_t(position)], "column-native-drag-header-body-boundaries");
        rowLeft += displayWidths[size_t(position)];
      }
    }
    SendMessageW(header, WM_LBUTTONUP, 0, MAKELPARAM(divider.right - 1, dividerY));
    sortQueued = previousSortQueued;
    for (int repeat = 0; repeat < 128; ++repeat) {
      ListView_SetColumnWidth(list, display, ListView_GetColumnWidth(list, display));
      check(!geometryQueued && !columnGeometryDirty && !GetUpdateRect(list, nullptr, FALSE) && !GetUpdateRect(header, nullptr, FALSE), "column-repeated-width-no-repaint");
    }
    ListView_SetColumnWidth(list, display, width);
    for (int tab = 0; tab < TabCount; ++tab) {
      if (tab == PerformanceTab) continue;
      selectTab(tab);
      for (int column = 0; column < Header_GetItemCount(header); ++column) {
        const int originalWidth = ListView_GetColumnWidth(list, column);
        ListView_SetColumnWidth(list, column, 1);
        check(ListView_GetColumnWidth(list, column) == scale(minimumColumnWidth(tab, columnAt(column))), "all-table-column-minimums");
        ListView_SetColumnWidth(list, column, originalWidth);
      }
    }
    selectTab(PerformanceTab);
    const int hiddenWidth = ListView_GetColumnWidth(list, 0);
    ListView_SetColumnWidth(list, 0, hiddenWidth + 1);
    check(!(GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE), "column-update-preserves-hidden-table");
    ListView_SetColumnWidth(list, 0, hiddenWidth);
    selectTab(DetailsTab);
    const unsigned unchanged = fullRebuilds; const auto* unchangedRows = rows.data();
    for (int repeat = 0; repeat < 128; ++repeat) { selectTab(DetailsTab); command(memoryPercent ? MemoryPercents : MemoryValues); command(networkPercent ? NetworkPercents : NetworkValues); }
    check(fullRebuilds == unchanged && rows.data() == unchangedRows, "idempotent-actions");
  } else check(false, "cpu-column-present");
  {
    selectTab(DetailsTab); rebuild(); const auto snapshot = current; const unsigned builds = fullRebuilds; const size_t unfiltered = rows.size();
    const bool savedSearchVisible = searchVisible; searchVisible = true;
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(SearchId, EN_SETFOCUS), LPARAM(searchBox));
    check(searchVisible && searchText.empty(), "search-focus-does-not-toggle"); searchVisible = savedSearchVisible;
    SetWindowTextW(searchBox, L"no-such-process-7d62a97e"); applySearch();
    check(rows.empty() && searchRows.size() == unfiltered && current == snapshot && fullRebuilds == builds, "cached-search-no-match");
    const auto* storage = searchRows.data();
    for (int repeat = 0; repeat < 128; ++repeat) { SetWindowTextW(searchBox, L"no-such-process-7d62a97e"); applySearch(); }
    check(searchRows.data() == storage && current == snapshot && fullRebuilds == builds, "idempotent-search-spam");
    SetWindowTextW(searchBox, std::to_wstring(GetCurrentProcessId()).c_str()); applySearch();
    check(std::any_of(rows.begin(), rows.end(), [&](const Row& row) { return row.process >= 0 && current->processes[size_t(row.process)].id == GetCurrentProcessId(); }), "cached-search-pid");
    sortColumn[DetailsTab] = 5; ascending[DetailsTab] = true; rebuild(true);
    ascending[DetailsTab] = false; SetWindowTextW(searchBox, L""); applySearch();
    check(rows.size() == unfiltered && searchRows.empty() && current == snapshot && fullRebuilds == builds, "cached-search-clear");
    for (size_t index = 1; index < rows.size(); ++index) check(rows[index - 1].cells[5].value >= rows[index].cells[5].value, "search-clear-restores-current-sort");
    const int originalTheme = theme;
    theme = 1; applyTheme(); theme = 0; applyTheme();
    const DWORD originalGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    for (int repeat = 0; repeat < 24; ++repeat) { theme = repeat % 2; applyTheme(); }
    const DWORD finalGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    check(finalGdi <= originalGdi + 2 && current == snapshot && fullRebuilds == builds, "theme-switch-no-leak-or-sampling-" + std::to_string(originalGdi) + "-" + std::to_string(finalGdi));
    theme = originalTheme; applyTheme();
    std::vector<Row> fixtures(4); fixtures[0].kind = RowKind::Heading; fixtures[0].key = L"heading"; fixtures[1].key = L"parent"; fixtures[1].cells = {{L"Browser"}};
    fixtures[2].key = L"child"; fixtures[2].depth = 1; fixtures[2].cells = {{L"Renderer"}}; fixtures[3].key = L"other"; fixtures[3].cells = {{L"Editor"}};
    searchText = L"renderer"; const auto filtered = filteredRows(fixtures); searchText.clear();
    check(filtered.size() == 3 && filtered[0].key == L"heading" && filtered[1].key == L"parent" && filtered[2].key == L"child", "search-retains-group-context");
    const auto ownProcess = std::find_if(current->processes.begin(), current->processes.end(), [](const Process& process) { return process.id == GetCurrentProcessId(); });
    if (ownProcess != current->processes.end()) {
      Row metadataRow; metadataRow.process = int(ownProcess - current->processes.begin()); metadataRow.members = {metadataRow.process};
      const auto& metadata = icons->get(*ownProcess);
      for (const auto& query : {ownProcess->name, std::to_wstring(ownProcess->id), metadata.description, metadata.publisher, metadata.commandLine}) if (!query.empty()) {
        searchText = lower(query); check(filteredRows({metadataRow}).size() == 1, "search-primary-process-metadata");
      }
      searchText = std::to_wstring(ownProcess->id); metadataRow.process = -1;
      check(filteredRows({metadataRow}).size() == 1, "search-collapsed-member-pid");
      searchText = L"no-such-process-7d62a97e"; check(filteredRows({metadataRow}).empty(), "search-member-no-match");
      searchText.clear();
    }
    SetWindowTextW(searchBox, L"no-such-process-7d62a97e");
    setCompact(true);
    const auto* compactStorage = rows.data(); const size_t compactCount = rows.size(); const unsigned compactBuilds = fullRebuilds;
    SendMessageW(window, WM_TIMER, 5, 0);
    check(rows.data() == compactStorage && rows.size() == compactCount && searchRows.empty() && fullRebuilds == compactBuilds, "pending-search-preserves-compact-rows");
    SetWindowTextW(searchBox, L""); SendMessageW(window, WM_TIMER, 5, 0);
    check(rows.data() == compactStorage && rows.size() == compactCount && searchRows.empty() && fullRebuilds == compactBuilds, "pending-search-clear-preserves-compact-rows");
    setCompact(false);
    check(rows.size() == unfiltered && searchText.empty() && searchRows.empty(), "compact-search-restores-full-table");
    setCompact(true); SetWindowTextW(searchBox, L"no-such-process-7d62a97e"); SendMessageW(window, WM_TIMER, 5, 0); setCompact(false);
    check(rows.empty() && searchRows.size() == unfiltered, "compact-search-resumes-filter-in-full-view");
    SetWindowTextW(searchBox, L""); applySearch();
  }
  sortColumn = savedColumns; ascending = savedDirections; groupByType = savedGrouping; expanded = savedExpanded; selectTab(savedTab); rebuild();
  verifyingSorting = false;
  return passed;
}
bool Application::verifyTabSwitching() {
  const int savedTab = selectedTab, savedTheme = theme;
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  const auto sample = current;
  auto checkCaption = [&] {
    DwmFlush();
    RECT frame{}; GetWindowRect(window, &frame);
    HDC screen = GetWindowDC(window), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, frame.right - frame.left, frame.bottom - frame.top);
    const auto previous = SelectObject(memory, bitmap);
    check(PrintWindow(window, memory, PW_RENDERFULLCONTENT) != FALSE, "caption-headless-render");
    const auto pixel = GetPixel(memory, (frame.right - frame.left) / 2, GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CYCAPTION, dpi) / 2);
    if (!highContrast) check(pixel != CLR_INVALID && ((GetRValue(pixel) < 128 && GetGValue(pixel) < 128 && GetBValue(pixel) < 128) == dark), "caption-immediate-theme-pixels");
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
  };
  for (const int appearance : {0, 1}) {
    theme = appearance; applyTheme();
    checkCaption();
    for (int round = 0; round < 64; ++round) {
      const unsigned before = fullRebuilds;
      for (int click = 0; click < 129; ++click) {
        TabCtrl_SetCurSel(tabs, (round + click) % TabCount);
        NMHDR notice{tabs, TabsId, TCN_SELCHANGE};
        SendMessageW(window, WM_NOTIFY, TabsId, LPARAM(&notice));
      }
      check(fullRebuilds == before, "tab-burst-no-synchronous-rebuild");
      const int expected = (round + 128) % TabCount;
      MSG pendingMessage{};
      const auto begin = Clock::now();
      if (PeekMessageW(&pendingMessage, window, WM_APP + 12, WM_APP + 12, PM_REMOVE)) DispatchMessageW(&pendingMessage);
      UpdateWindow(tabs); UpdateWindow(expected == PerformanceTab ? performance : list);
      tabTimes.push_back(milliseconds(begin));
      check(selectedTab == expected && TabCtrl_GetCurSel(tabs) == expected && !tabQueued && pendingTab == -1, "tab-burst-last-request-wins");
      check(fullRebuilds <= before + 1, "tab-burst-single-rebuild");
      check(current == sample, "tab-switch-no-resampling");
      const unsigned unchanged = fullRebuilds;
      selectTab(expected); selectTab(-1); selectTab(TabCount);
      check(fullRebuilds == unchanged, "tab-repeat-invalid-noop");
    }
    checkCaption();
    selectTab(DetailsTab);
    ListView_SetColumnWidth(list, 0, scale(1500));
    UpdateWindow(list);
    RECT bounds{}; GetWindowRect(list, &bounds);
    const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
    HDC screen = GetDC(list), memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    const auto previous = SelectObject(memory, bitmap);
    if (dark) {
      const auto beforePaints = scrollbarPaints;
      rebuilding = true;
      for (int repeat = 0; repeat < 128; ++repeat) {
        SendMessageW(list, WM_NCPAINT, 1, 0);
        SendMessageW(list, WM_NCACTIVATE, repeat % 2, 0);
      }
      check(scrollbarPaints == beforePaints, "scrollbar-rebuild-no-intermediate-paints");
      SendMessageW(list, WM_PRINT, WPARAM(memory), PRF_NONCLIENT | PRF_CLIENT | PRF_CHILDREN);
      check(scrollbarPaints > beforePaints, "scrollbar-rebuild-explicit-capture");
      rebuilding = false;
    }
    check(PrintWindow(list, memory, 0) != FALSE, "scrollbar-headless-render");
    for (const LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
      SCROLLBARINFO info{sizeof(info)};
      check(GetScrollBarInfo(list, object, &info) != FALSE, "scrollbar-accessibility-retained");
      if (info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) continue;
      const int pixelX = info.rcScrollBar.left - bounds.left + 1, pixelY = info.rcScrollBar.top - bounds.top + 1;
      const auto color = GetPixel(memory, pixelX, pixelY);
      if (dark) check(color == RGB(32, 32, 32), "scrollbar-dark-track-pixels");
      else check(color != RGB(32, 32, 32), "scrollbar-light-restored");
      if (dark) for (int repeat = 0; repeat < 64; ++repeat) {
        SendMessageW(list, WM_NCPAINT, 1, 0);
        SendMessageW(list, WM_NCACTIVATE, repeat % 2, 0);
        HDC live = GetWindowDC(list);
        check(live && GetPixel(live, pixelX, pixelY) == RGB(32, 32, 32), "scrollbar-no-light-nonclient-frame");
        if (live) ReleaseDC(list, live);
      }
    }
    SelectObject(memory, previous); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(list, screen);
    SendMessageW(list, WM_HSCROLL, SB_LEFT, 0);
    for (int repeat = 0; repeat < 64; ++repeat) {
      const int beforeScroll = GetScrollPos(list, SB_HORZ);
      SendMessageW(list, WM_HSCROLL, repeat % 2 ? SB_LINELEFT : SB_LINERIGHT, 0);
      check(GetScrollPos(list, SB_HORZ) != beforeScroll, "scrollbar-native-horizontal-scroll");
      check(!GetUpdateRect(list, nullptr, FALSE) && !GetUpdateRect(header, nullptr, FALSE), "horizontal-header-rows-painted-together");
      check(current == sample, "horizontal-scroll-no-resampling");
    }
    SendMessageW(list, WM_HSCROLL, SB_LEFT, 0);
    const int top = ListView_GetTopIndex(list);
    SendMessageW(list, WM_VSCROLL, SB_LINEDOWN, 0);
    check(ListView_GetTopIndex(list) >= top, "scrollbar-native-scroll");
    SendMessageW(list, WM_VSCROLL, SB_TOP, 0);
    check(ListView_GetTopIndex(list) == 0, "scrollbar-native-top");
    ListView_SetColumnWidth(list, 0, scale(widths[DetailsTab][0]));
    BOOL immersive = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &immersive, sizeof(immersive)))) check((immersive != FALSE) == dark, "caption-immediate-theme");
  }
  theme = savedTheme; applyTheme(); selectTab(savedTab);
  return passed;
}
bool Application::verifyPerformanceHover() {
  const int originalTab = selectedTab, originalSummary = summary, originalScroll = sidebarScroll, originalWheelDelta = sidebarWheelDelta, originalTheme = theme;
  const bool originalGraphs = hideGraphs; const auto originalResource = selectedResource; const auto snapshot = current;
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  selectTab(PerformanceTab);
  const auto originalBuilds = fullRebuilds;
  for (const bool graphs : {false, true}) for (const int mode : {0, 2}) for (const int appearance : {0, 1}) {
    theme = appearance; applyTheme(); hideGraphs = graphs; sidebarScroll = 0; setSummary(mode);
    const auto& items = perfItems(); const auto* storage = items.data(); const auto itemBuilds = performanceItemBuilds;
    selectedResource = items.front().key;
    SendMessageW(performance, WM_MOUSELEAVE, 0, 0);
    RedrawWindow(performance, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const auto details = performanceDetailPaints;
    const int pitch = scale(hideGraphs ? 60 : 82), horizontal = scale(150);
    auto move = [&](int vertical) { SendMessageW(performance, WM_MOUSEMOVE, 0, MAKELPARAM(horizontal, vertical)); };
    move(scale(24));
    check(hotResource == 0, "hover-cpu");
    const auto paints = performancePaints;
    for (int repeat = 0; repeat < 10000; ++repeat) move(scale(24) + repeat % scale(10));
    check(performancePaints == paints && !GetUpdateRect(performance, nullptr, FALSE), "same-resource-hover-no-repaint");
    for (int repeat = 0; repeat < 24; ++repeat) {
      const auto begin = Clock::now(); move(scale(24) + (repeat % 2 ? 0 : pitch)); hoverTimes.push_back(milliseconds(begin));
      check(hotResource == (repeat % 2 ? 0 : 1) && !GetUpdateRect(performance, nullptr, FALSE), "hover-immediate");
    }
    check(performanceDetailPaints == details, "hover-does-not-render-detail");
    check(perfItems().data() == storage && performanceItemBuilds == itemBuilds && current == snapshot, "hover-reuses-sample-and-labels");
    move(scale(10)); check(hotResource == -1 && performanceHitTest({horizontal, scale(10)}) == -1, "hover-top-padding");
    move(scale(12) + pitch - scale(2)); check(hotResource == -1, "hover-row-gap");
    move(-1); check(hotResource == -1, "hover-negative-coordinate");
    move(scale(24)); SendMessageW(performance, WM_MOUSELEAVE, 0, 0); check(hotResource == -1 && !performanceTracking, "hover-leave");
    UpdateWindow(performance); const auto beforeClicks = performancePaints;
    for (int repeat = 0; repeat < 1000; ++repeat) performanceClick({horizontal, scale(24)}, false, false);
    check(performancePaints == beforeClicks && !GetUpdateRect(performance, nullptr, FALSE), "repeated-resource-click-no-repaint");
    const auto beforeWheel = performancePaints;
    for (int repeat = 0; repeat < 1000; ++repeat) SendMessageW(performance, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    check(sidebarScroll == 0 && performancePaints == beforeWheel, "scroll-at-limit-no-repaint");
    RECT client{}; GetClientRect(performance, &client);
    for (size_t index = 0; index < items.size(); ++index) {
      sidebarScroll = std::min(int(index) * pitch, std::max(0, int(items.size()) * pitch + scale(24) - int(client.bottom)));
      hotResource = -1; RedrawWindow(performance, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
      const RECT box = performanceRowRect(int(index));
      const auto detailPaints = performanceDetailPaints, builds = performanceItemBuilds;
      move(std::max(0L, box.top) + scale(10));
      check(hotResource == int(index) && performanceDetailPaints == detailPaints && performanceItemBuilds == builds, "hover-every-resource");
    }
  }
  {
    hideGraphs = false; sidebarScroll = 0; sidebarWheelDelta = 0; setSummary(0);
    RECT originalClient{}; GetClientRect(performance, &originalClient);
    SetWindowPos(performance, nullptr, 0, 0, originalClient.right, scale(100), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RedrawWindow(performance, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const auto beforePartialWheel = performancePaints;
    for (int repeat = 0; repeat < 3; ++repeat) SendMessageW(performance, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA / 4), 0);
    check(sidebarScroll == 0 && performancePaints == beforePartialWheel, "partial-wheel-does-not-repaint-before-detent");
    SendMessageW(performance, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA / 4), 0);
    check(sidebarScroll == scale(82), "partial-wheel-accumulates-detent");
    SendMessageW(performance, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
    check(sidebarScroll == 0, "partial-wheel-reverses-direction");
    SetWindowPos(performance, nullptr, 0, 0, originalClient.right, originalClient.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client{}; GetClientRect(performance, &client); const RECT box = performanceRowRect(1);
    HDC screen = GetDC(performance), full = CreateCompatibleDC(screen), clipped = CreateCompatibleDC(screen);
    BITMAPINFO format{}; format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); format.bmiHeader.biWidth = client.right; format.bmiHeader.biHeight = -client.bottom; format.bmiHeader.biPlanes = 1; format.bmiHeader.biBitCount = 32;
    void* fullPixels = nullptr; HBITMAP fullBitmap = CreateDIBSection(screen, &format, DIB_RGB_COLORS, &fullPixels, nullptr, 0);
    format.bmiHeader.biWidth = box.right - box.left; format.bmiHeader.biHeight = box.top - box.bottom;
    void* clippedPixels = nullptr; HBITMAP clippedBitmap = CreateDIBSection(screen, &format, DIB_RGB_COLORS, &clippedPixels, nullptr, 0);
    check(full && clipped && fullBitmap && clippedBitmap, "hover-render-buffer-allocation");
    if (full && clipped && fullBitmap && clippedBitmap) {
      const auto fullPrevious = SelectObject(full, fullBitmap), clippedPrevious = SelectObject(clipped, clippedBitmap);
      SetViewportOrgEx(clipped, -box.left, -box.top, nullptr); IntersectClipRect(clipped, box.left, box.top, box.right, box.bottom);
      for (const int appearance : {0, 1}) {
        theme = appearance; applyTheme(); hotResource = 1; paintPerformance(full, client); paintPerformance(clipped, client); GdiFlush();
        bool equal = true;
        for (int vertical = box.top; vertical < box.bottom; ++vertical) {
          const auto* expected = static_cast<const uint32_t*>(fullPixels) + size_t(vertical) * client.right + box.left;
          const auto* actual = static_cast<const uint32_t*>(clippedPixels) + size_t(vertical - box.top) * (box.right - box.left);
          equal = equal && memcmp(expected, actual, size_t(box.right - box.left) * sizeof(uint32_t)) == 0;
        }
        check(equal, "hover-clipped-render-matches-full-render");
      }
      SelectObject(full, fullPrevious); SelectObject(clipped, clippedPrevious);
    }
    if (fullBitmap) DeleteObject(fullBitmap); if (clippedBitmap) DeleteObject(clippedBitmap);
    if (full) DeleteDC(full); if (clipped) DeleteDC(clipped); if (screen) ReleaseDC(performance, screen);
  }
  setSummary(1); RedrawWindow(performance, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
  const auto paints = performancePaints;
  for (int repeat = 0; repeat < 1000; ++repeat) SendMessageW(performance, WM_MOUSEMOVE, 0, MAKELPARAM(scale(30), scale(30)));
  check(hotResource == -1 && performancePaints == paints, "detail-summary-no-hover-work");
  check(fullRebuilds == originalBuilds && current == snapshot, "hover-no-table-rebuild-or-sampling");
  hideGraphs = originalGraphs; sidebarScroll = originalScroll; sidebarWheelDelta = originalWheelDelta; selectedResource = originalResource;
  theme = originalTheme; applyTheme(); setSummary(originalSummary); selectTab(originalTab);
  return passed;
}
void Application::benchmarkTick() {
  if (benchmarkRunning || verifyingSorting || rebuilding) return;
  benchmarkRunning = true;
  struct TickGuard { bool& running; ~TickGuard() { running = false; } } guard{benchmarkRunning};
  if (!current) { if (Clock::now() - started > std::chrono::seconds(30)) { writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0); } return; }
  if (!sortingChecked) {
    if (options.tab == StartupTab && current->startup->entries.empty()) {
      selectTab(StartupTab);
      if (Clock::now() - started > std::chrono::seconds(30)) { sortingFailure = "startup-inventory-timeout"; writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0); }
      return;
    }
    sortingChecked = true; sortingPassed = verifySorting();
  }
  if (keyboardPassed && options.tab >= 0 && !options.tabSpam) selectTab(options.tab);
  if (!measured && keyboardPassed && std::chrono::duration<double>(Clock::now() - started).count() >= options.warmup) {
    measured = true; measurementStarted = Clock::now(); LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); measurementCounter = counter.QuadPart; FILETIME created{}, exited{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &cpuStartKernel, &cpuStartUser); GetProcessHandleCount(GetCurrentProcess(), &baselineHandles);
    baselineGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); baselineUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    deliveryTimes.clear(); updateTimes.clear(); paintTimes.clear(); samplerTimes.clear(); inputTimes.clear(); sortTimes.clear(); tests = sortRequests = fullRebuilds = sortPasses = 0; for (auto& times : stageTimes) times.clear();
  }
  if (options.sortSpam && keyboardPassed && measured) {
    selectTab(options.tab >= 0 && options.tab != PerformanceTab ? options.tab : ProcessesTab);
    const int columns = Header_GetItemCount(header);
    const int cpu = selectedTab == StartupTab ? 0 : selectedTab == DetailsTab ? 5 : 7; const auto found = std::find(shown[size_t(selectedTab)].begin(), shown[size_t(selectedTab)].end(), cpu);
    if (tests % 3 == 0 && found != shown[size_t(selectedTab)].end()) {
      RECT bounds{}; Header_GetItemRect(header, int(found - shown[size_t(selectedTab)].begin()), &bounds); const LPARAM point = MAKELPARAM((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
      for (int click = 0; click < 129; ++click) { PostMessageW(header, click % 2 ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN, MK_LBUTTON, point); PostMessageW(header, WM_LBUTTONUP, 0, point); ++sortRequests; }
    } else for (int click = 0; click < 128 && columns > 0; ++click) { NMLISTVIEW notice{}; notice.hdr = {list, ListId, LVN_COLUMNCLICK}; notice.iSubItem = int((unsigned(click) + tests) % unsigned(columns)); SendMessageW(window, WM_NOTIFY, ListId, LPARAM(&notice)); ++sortRequests; }
  }
  if (options.tabSpam && keyboardPassed && measured) {
    for (int click = 0; click < 128; ++click) {
      TabCtrl_SetCurSel(tabs, int((tests + unsigned(click)) % TabCount));
      NMHDR notice{tabs, TabsId, TCN_SELCHANGE}; SendMessageW(window, WM_NOTIFY, TabsId, LPARAM(&notice));
    }
  }
  ++tests;
  if (!keyboardPassed || (!options.idle && !options.minimized)) {
    selectTab(options.tab >= 0 && keyboardPassed ? options.tab : int(tests % TabCount));
    if (selectedTab == PerformanceTab) { HDC dc = GetDC(performance); HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, 800, 600); auto old = SelectObject(memory, bitmap); paintPerformance(memory, {0, 0, 800, 600}); SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(performance, dc); }
    if (selectedTab == ProcessesTab && !rows.empty()) {
      select(0); const auto name = cell(rows[0], 0);
      if (!name.empty()) {
        prefix.clear();
        const int expected = findRow(std::wstring_view(name).substr(0, 1), 1); SendMessageW(list, WM_CHAR, WPARAM(name[0]), 0); bool passed = selectedIndex() == expected;
        const int repeated = findRow(std::wstring_view(name).substr(0, 1), expected + 1); SendMessageW(list, WM_CHAR, WPARAM(name[0]), 0); passed = passed && selectedIndex() == repeated;
        keyboardPassed = tests <= TabCount ? passed : keyboardPassed && passed;
      }
    }
  }
  if (measured && std::chrono::duration<double>(Clock::now() - measurementStarted).count() >= options.benchmarkSeconds) {
    HDC screen = GetDC(window); HDC memory = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, 1000, 700); auto old = SelectObject(memory, bitmap); paintPerformance(memory, {0, 0, 1000, 700});
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
    writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0);
  }
}
static void writeMetric(Report& stream, const char* name, std::vector<double> values) {
  std::sort(values.begin(), values.end()); const double total = std::accumulate(values.begin(), values.end(), 0.0);
  stream << '"' << name << "\":{\"count\":" << values.size() << ",\"meanMilliseconds\":" << (values.empty() ? 0 : total / double(values.size())) << ",\"p95Milliseconds\":" << (values.empty() ? 0 : values[size_t(std::ceil(double(values.size()) * .95)) - 1]) << ",\"maxMilliseconds\":" << (values.empty() ? 0 : values.back()) << '}';
}
void Application::writeBenchmark() {
  const bool coalescingPassed = !options.sortSpam || (sortRequests >= 128 && !sortTimes.empty() && sortTimes.size() * 16 < sortRequests);
  const bool tabSpamPassed = !options.tabSpam || (tabRequests >= 128 && tabCommits > 0 && tabCommits * 16 < tabRequests);
  exitCode = updates > 0 && keyboardPassed && sortingPassed && coalescingPassed && tabSpamPassed ? 0 : 1;
  if (options.output.empty()) return;
  Report stream{options.output}; FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)); DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
  const double elapsed = std::max(.001, std::chrono::duration<double>(Clock::now() - (measured ? measurementStarted : started)).count()); const double cpu = double(ticks(kernel) + ticks(user) - ticks(cpuStartKernel) - ticks(cpuStartUser)) / 10000000;
  stream << "{\"native\":true,\"passed\":" << (exitCode == 0 ? "true" : "false") << ",\"elapsedSeconds\":" << elapsed << ",\"firstSampleMilliseconds\":" << (firstSample == Time{} ? 0 : std::chrono::duration<double, std::milli>(firstSample - started).count()) << ",\"processCpuPercentOneCore\":" << cpu * 100 / elapsed << ",\"privateBytes\":" << memory.PrivateUsage << ",\"handles\":" << handles << ",\"updates\":" << updates << ",\"exercises\":" << tests << ",\"keyboardPassed\":" << (keyboardPassed ? "true" : "false") << ',';
  stream << "\"warmupSeconds\":" << options.warmup << ",\"handleDelta\":" << int64_t(handles) - baselineHandles << ",\"gdiDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - baselineGdi << ",\"userDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - baselineUser << ',';
  stream << "\"sortingFailure\":\"" << sortingFailure << "\",";
  stream << "\"sortingPassed\":" << (sortingPassed ? "true" : "false") << ",\"coalescingPassed\":" << (coalescingPassed ? "true" : "false") << ",\"sortRequests\":" << sortRequests << ",\"fullRebuilds\":" << fullRebuilds << ",\"sortPasses\":" << sortPasses << ','; writeMetric(stream, "cachedSort", sortTimes); stream << ',';
  writeMetric(stream, "performanceHover", hoverTimes); stream << ',';
  writeMetric(stream, "tabSwitch", tabTimes); stream << ',';
  writeMetric(stream, "columnResize", columnTimes); stream << ',';
  stream << "\"tabRequests\":" << tabRequests << ",\"tabCommits\":" << tabCommits << ",\"tabSpamPassed\":" << (tabSpamPassed ? "true" : "false") << ',';
  writeMetric(stream, "sampler", samplerTimes); stream << ','; writeMetric(stream, "messageQueue", deliveryTimes); stream << ','; writeMetric(stream, "uiUpdate", updateTimes); stream << ','; writeMetric(stream, "graphPaint", paintTimes); stream << ','; writeMetric(stream, "letterNavigation", inputTimes); stream << ','; writeMetric(stream, "iconRequestToExtraction", icons->latencies); stream << ','; writeMetric(stream, "iconRequestToDisplay", icons->displayLatencies);
  static constexpr const char* stages[] = {"processInventory", "cpuAndMemory", "pdhAndDisk", "network", "gpuAggregation", "servicesSessionsStartup"}; for (size_t index = 0; index < stageTimes.size(); ++index) { stream << ','; writeMetric(stream, stages[index], stageTimes[index]); } stream << '}';
  if (!stream) exitCode = 1;
}
// Renders every view into PNGs from an off-screen, non-activating window, so the real UI can be reviewed without touching the desktop.
void Application::screenshotTick() {
  if (!current || updates < 4 || Clock::now() - started < std::chrono::seconds(6)) return;
  struct Shot { const wchar_t* name; std::function<void()> apply; };
  auto perf = [this](const wchar_t* key) { return [this, start = std::wstring(key)] { setSummary(0); selectTab(PerformanceTab); for (const auto& item : perfItems()) if (item.key.starts_with(start)) { selectedResource = item.key; break; } InvalidateRect(performance, nullptr, FALSE); }; };
  const std::vector<Shot> shots{
    {L"processes", [this] { selectTab(ProcessesTab); }},
    {L"processes-expanded", [this] { for (size_t index = 0; index < rows.size(); ++index) if (rows[index].kind == RowKind::Group) { expanded.insert(rows[index].key); rebuild(); select(int(index) + 1); hotRow = int(index) + 3; break; } for (const auto& row : rows) if (row.kind == RowKind::Process && row.expandable) { expanded.insert(row.key); break; } rebuild(); }},
    {L"processes-background", [this] { expanded.clear(); rebuild(); for (size_t index = 0; index < rows.size(); ++index) if (rows[index].kind == RowKind::Heading && index > 0) { ListView_Scroll(list, 0, (int(index) - ListView_GetTopIndex(list)) * rowHeight()); break; } }},
    {L"processes-ungrouped", [this] { groupByType = false; sortColumn[ProcessesTab] = 7; ascending[ProcessesTab] = false; rebuild(); InvalidateRect(header, nullptr, FALSE); }},
    {L"performance-cpu", perf(L"cpu")}, {L"performance-cpu-logical", [this, apply = perf(L"cpu")] { apply(); logical = true; }}, {L"performance-memory", [this, apply = perf(L"memory")] { logical = false; apply(); }},
    {L"performance-disk", perf(L"disk/")}, {L"performance-network", perf(L"net/")}, {L"performance-gpu", perf(L"gpu/")},
    {L"app-history", [this] { selectTab(HistoryTab); }}, {L"startup", [this] { selectTab(StartupTab); }},
    {L"users", [this] { selectTab(UsersTab); if (!rows.empty()) { expanded.insert(rows[0].key); rebuild(); } }},
    {L"details", [this] { selectTab(DetailsTab); }}, {L"services", [this] { selectTab(ServicesTab); }},
    {L"compact", [this] { selectTab(ProcessesTab); setCompact(true); }}, {L"summary", [this] { setCompact(false); selectTab(PerformanceTab); selectedResource = L"cpu"; setSummary(1); }},
    {L"search-details", [this] { setSummary(0); selectTab(DetailsTab); searchVisible = true; layout(); SetWindowTextW(searchBox, L"taskmanager"); applySearch(); }},
    {L"search-no-results", [this] { SetWindowTextW(searchBox, L"no-such-process-7d62a97e"); applySearch(); }},
  };
  const size_t shot = screenshotStep / 2;
  if (shot >= shots.size()) { SendMessageW(window, WM_CLOSE, 0, 0); return; }
  if (screenshotStep % 2 && selectedTab == StartupTab && (!current || current->startup->entries.empty()) && Clock::now() - started < std::chrono::seconds(30)) return;
  if (screenshotStep++ % 2 == 0) { shots[shot].apply(); RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME); return; }
  RECT frame{}, bounds{}; GetWindowRect(window, &frame); if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds)))) bounds = frame;
  const int width = frame.right - frame.left, height = frame.bottom - frame.top;
  HDC screen = GetDC(nullptr); HDC memory = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height); auto old = SelectObject(memory, bitmap);
  PrintWindow(window, memory, PW_RENDERFULLCONTENT); SelectObject(memory, old);
  {
    Gdiplus::Bitmap image(bitmap, nullptr); const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
    std::unique_ptr<Gdiplus::Bitmap> cropped(image.Clone(Gdiplus::Rect(bounds.left - frame.left, bounds.top - frame.top, bounds.right - bounds.left, bounds.bottom - bounds.top), PixelFormat32bppRGB));
    wchar_t name[128]{}; swprintf_s(name, L"%02zu-%s.png", shot, shots[shot].name);
    (cropped ? cropped.get() : &image)->Save((std::filesystem::path(options.screenshots) / name).c_str(), &png, nullptr);
  }
  DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, screen);
}
}
