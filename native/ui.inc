const Process* Application::selectedProcess() const {
  const int index = selectedIndex();
  if (!current || index < 0 || size_t(index) >= rows.size() || rows[size_t(index)].process < 0) return nullptr;
  return &current->processes[size_t(rows[size_t(index)].process)];
}
std::wstring Application::cell(const Row& row, int column) const {
  if (row.process < 0) return column >= 0 && size_t(column) < row.entry.cells.size() ? row.entry.cells[size_t(column)] : L"";
  const auto& process = current->processes[size_t(row.process)]; const auto& metadata = icons->get(process);
  if (selectedTab == 0) {
    switch (column) {
    case 0: return process.name;
    case 1: return L"";
    case 2: return std::to_wstring(process.id);
    case 3: return number(process.cpu) + L"%";
    case 4: return bytes(double(process.working));
    case 5: return bytes(process.ioRate) + L"/s";
    case 6: return number(process.gpu) + L"%";
    case 7: return metadata.publisher.empty() ? L"—" : metadata.publisher;
    }
  }
  if (selectedTab == 4) {
    switch (column) { case 0: return process.name; case 1: return std::to_wstring(process.id); case 2: return L""; case 3: return number(process.cpu) + L"%"; case 4: return bytes(double(process.working)); }
  }
  if (selectedTab == 5) {
    switch (column) {
    case 0: return process.name; case 1: return std::to_wstring(process.id); case 2: return L"";
    case 3: return metadata.user.empty() ? L"—" : metadata.user; case 4: return number(process.cpu) + L"%";
    case 5: return bytes(double(process.working)); case 6: return metadata.description.empty() ? L"—" : metadata.description;
    case 7: return std::to_wstring(process.session); case 8: return std::to_wstring(process.threads); case 9: return std::to_wstring(process.handles);
    case 10: return number(double(process.cpuTicks) / 10000000, 2) + L" s"; case 11: return bytes(process.ioRate) + L"/s"; case 12: return number(process.gpu) + L"%";
    }
  }
  return L"";
}
void Application::rebuild() {
  if (!current) return;
  const auto started = Clock::now(); const auto selected = rowKey(selectedIndex()); const int top = ListView_GetTopIndex(list); const auto topKey = rowKey(top);
  rebuilding = true; std::vector<Row> next;
  auto addProcess = [&](size_t index, int indent = 0) { const auto& process = current->processes[index]; Row row; row.process = int(index); row.indent = indent; row.key = std::to_wstring(process.id) + L":" + std::to_wstring(process.created); next.push_back(std::move(row)); };
  if (selectedTab == 0 || selectedTab == 5) {
    for (size_t index = 0; index < current->processes.size(); ++index) if (matches(current->processes[index], icons->get(current->processes[index]), filter)) addProcess(index);
  } else if (selectedTab == 2) {
    for (const auto& [key, value] : usage) if (contains(value.name, filter)) { Row row; row.key = key; row.entry.cells = {value.name, number(double(value.ticks) / 10000000, 2) + L" s", L"Unavailable"}; next.push_back(std::move(row)); }
  } else if (selectedTab != 1) {
    const auto& entries = selectedTab == 3 ? current->startup : selectedTab == 4 ? current->sessions : current->services;
    for (const auto& entry : entries) {
      bool match = filter.empty(); for (const auto& text : entry.cells) match = match || contains(text, filter);
      if (!match) continue; Row row; row.key = entry.key; row.entry = entry;
      if (selectedTab == 4) { double cpu = 0; uint64_t memory = 0; for (const auto& process : current->processes) if (process.session == entry.session) { cpu += process.cpu; memory += process.working; } row.entry.cells.push_back(number(cpu) + L"%"); row.entry.cells.push_back(bytes(double(memory))); }
      next.push_back(std::move(row));
    }
  }
  const int column = sortColumn[size_t(selectedTab)]; const bool direction = ascending[size_t(selectedTab)];
  auto numeric = [&](const Process& process) -> double {
    if (selectedTab == 0) { switch (column) { case 2: return process.id; case 3: return process.cpu; case 4: return double(process.working); case 5: return process.ioRate; case 6: return process.gpu; } }
    if (selectedTab == 5) { switch (column) { case 1: return process.id; case 4: return process.cpu; case 5: return double(process.working); case 7: return process.session; case 8: return process.threads; case 9: return process.handles; case 10: return double(process.cpuTicks); case 11: return process.ioRate; case 12: return process.gpu; } }
    return -1;
  };
  std::stable_sort(next.begin(), next.end(), [&](const Row& left, const Row& right) { int compare = 0; if (left.process >= 0 && right.process >= 0 && numeric(current->processes[size_t(left.process)]) >= 0) { const double first = numeric(current->processes[size_t(left.process)]), second = numeric(current->processes[size_t(right.process)]); compare = first < second ? -1 : first > second ? 1 : 0; } else compare = CompareStringOrdinal(cell(left, column).c_str(), -1, cell(right, column).c_str(), -1, TRUE) - CSTR_EQUAL; if (!compare) compare = left.key.compare(right.key); return direction ? compare < 0 : compare > 0; });
  if (selectedTab == 0 && grouped && filter.empty()) {
    std::vector<Row> groupedRows; std::unordered_set<DWORD> childIds;
    for (const auto& root : next) if (current->processes[size_t(root.process)].app) {
      auto parent = root; std::vector<Row> children;
      const auto& rootProcess = current->processes[size_t(root.process)];
      for (const auto& candidate : next) { const auto& process = current->processes[size_t(candidate.process)]; if (process.parent == rootProcess.id && process.created >= rootProcess.created && process.id != rootProcess.id && !process.app) { auto child = candidate; child.indent = 1; children.push_back(child); childIds.insert(process.id); } }
      parent.children = unsigned(children.size()); groupedRows.push_back(parent);
      if (expanded.contains(parent.key)) groupedRows.insert(groupedRows.end(), children.begin(), children.end());
    }
    for (const auto& row : next) if (!current->processes[size_t(row.process)].app && !childIds.contains(current->processes[size_t(row.process)].id)) groupedRows.push_back(row);
    next.swap(groupedRows);
  }
  if (selectedTab == 4 && filter.empty()) {
    std::vector<Row> expandedRows;
    for (const auto& row : next) { expandedRows.push_back(row); if (expanded.contains(row.key)) for (size_t index = 0; index < current->processes.size(); ++index) if (current->processes[index].session == row.entry.session) { Row child; child.process = int(index); child.indent = 1; child.key = std::to_wstring(current->processes[index].id) + L":" + std::to_wstring(current->processes[index].created); expandedRows.push_back(std::move(child)); } }
    next.swap(expandedRows);
  }
  rows.swap(next); SendMessageW(list, WM_SETREDRAW, FALSE, 0); ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  for (size_t index = 0; index < rows.size(); ++index) if (rows[index].key == selected) select(int(index));
  if (selected.empty() && !rows.empty()) select(0);
  if (!topKey.empty()) for (size_t index = 0; index < rows.size(); ++index) if (rows[index].key == topKey) { ListView_Scroll(list, 0, (int(index) - ListView_GetTopIndex(list)) * scale(20)); break; }
  SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, FALSE); rebuilding = false;
  const int visible = std::max(0, ListView_GetTopIndex(list)), count = std::max(1, ListView_GetCountPerPage(list));
  for (int index = std::min(int(rows.size()), visible + count) - 1; index >= visible; --index) if (rows[size_t(index)].process >= 0) icons->request(current->processes[size_t(rows[size_t(index)].process)], true);
  for (const auto& row : rows) if (row.process >= 0) icons->request(current->processes[size_t(row.process)]);
  if (hidden) updateTimes.push_back(milliseconds(started));
}
void Application::letter(wchar_t character) {
  const auto started = Clock::now(); if (character < L' ' || rows.empty()) return;
  const wchar_t folded = wchar_t(towlower(character)); const bool expired = prefixAt == Time{} || Clock::now() - prefixAt > std::chrono::seconds(1);
  const bool repeat = !expired && prefix.size() == 1 && prefix[0] == folded;
  if (expired || repeat) prefix.assign(1, folded); else prefix.push_back(folded); prefixAt = Clock::now();
  std::vector<std::wstring> names; names.reserve(rows.size()); for (const auto& row : rows) names.push_back(row.heading ? L"" : cell(row, 0));
  int found = findNext(names, prefix, selectedIndex() + (prefix.size() == 1 ? 1 : 0));
  if (found < 0 && prefix.size() > 1) { prefix.assign(1, folded); found = findNext(names, prefix, selectedIndex() + 1); }
  select(found); if (hidden) inputTimes.push_back(milliseconds(started));
}
void Application::worker() {
  try {
    Sampler source;
    while (!stopping) {
      auto sample = std::make_shared<Sample>(source.sample());
      { std::lock_guard lock(workerMutex); pending = std::move(sample); published = Clock::now(); }
      if (!notification.exchange(true)) PostMessageW(window, SampleMessage, 0, 0);
      std::unique_lock lock(workerMutex); const int delay = interval.load();
      if (!delay) wake.wait(lock, [&] { return stopping || force || interval.load() != 0; });
      else wake.wait_for(lock, std::chrono::milliseconds(minimized ? std::max(delay, 4000) : delay), [&] { return stopping || force; });
      if (force) source.invalidateInventory(); force = false;
    }
  } catch (const std::exception& exception) { std::lock_guard lock(actionMutex); const auto text = exception.what(); actionResults.push_back({std::wstring(text, text + strlen(text)), L""}); PostMessageW(window, ActionMessage, 0, 0); }
}
void Application::consume() {
  if (!hidden && (minimized || (GetKeyState(VK_CONTROL) & 0x8000))) return;
  std::shared_ptr<Sample> sample; Time publish;
  { std::lock_guard lock(workerMutex); notification = false; sample.swap(pending); publish = published; }
  if (!sample) return; if (hidden) { deliveryTimes.push_back(milliseconds(publish)); samplerTimes.push_back(sample->duration); for (size_t index = 0; index < stageTimes.size(); ++index) stageTimes[index].push_back(sample->stages[index]); } current = std::move(sample); ++updates;
  if (firstSample == Time{}) firstSample = Clock::now(); const double now = std::chrono::duration<double>(Clock::now() - shown).count(); graphTime = now;
  for (const auto& resource : current->resources) histories[resource.key].add(now, resource.value);
  std::unordered_map<Identity, uint64_t, IdentityHash> nextCpu;
  for (const auto& process : current->processes) { const Identity identity{process.id, process.created}; const auto previous = priorCpu.find(identity); const auto key = lower(process.name); auto found = usage.find(key); if (found == usage.end() && usage.size() < 8192) found = usage.emplace(key, Usage{process.name, 0}).first; if (found != usage.end() && previous != priorCpu.end() && process.cpuTicks >= previous->second) found->second.ticks += process.cpuTicks - previous->second; nextCpu.emplace(identity, process.cpuTicks); }
  priorCpu.swap(nextCpu); icons->prune(current->processes);
  if (selectedTab == 1) {
    if (SendMessageW(resources, LB_GETCOUNT, 0, 0) != LRESULT(current->resources.size())) { SendMessageW(resources, LB_RESETCONTENT, 0, 0); for (const auto& resource : current->resources) SendMessageW(resources, LB_ADDSTRING, 0, LPARAM(resource.name.c_str())); }
    selectedResource = std::clamp(selectedResource, 0, std::max(0, int(current->resources.size()) - 1)); SendMessageW(resources, LB_SETCURSEL, WPARAM(selectedResource), 0); InvalidateRect(resources, nullptr, FALSE); InvalidateRect(graph, nullptr, FALSE);
  } else rebuild();
  SetWindowTextW(statusLabel, (L"CPU " + number(current->cpu) + L"%   •   Memory " + bytes(double(current->memoryUsed)) + L"   •   " + std::to_wstring(current->processes.size()) + L" processes").c_str());
  if (!hidden && Clock::now() - lastSave > std::chrono::seconds(30)) { saveHistory(); lastSave = Clock::now(); }
}
void Application::paint(HDC dc, RECT bounds) {
  const auto started = Clock::now(); FillRect(dc, &bounds, GetSysColorBrush(COLOR_WINDOW));
  if (!current || current->resources.empty()) return;
  const auto& resource = current->resources[size_t(std::clamp(selectedResource, 0, int(current->resources.size()) - 1))]; SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(30, 30, 30)); SelectObject(dc, titleFont);
  RECT title{scale(20), scale(20), bounds.right - scale(20), scale(70)}; DrawTextW(dc, resource.name.c_str(), -1, &title, DT_LEFT | DT_SINGLELINE);
  SelectObject(dc, font); RECT detail{scale(20), scale(73), bounds.right - scale(20), scale(108)}; DrawTextW(dc, resource.detail.c_str(), -1, &detail, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
  const double now = graphTime; RECT area{scale(20), scale(122), bounds.right - scale(20), std::max(LONG(scale(180)), bounds.bottom - scale(150))};
  const auto& history = histories[resource.key]; double maximum = resource.maximum;
  if (resource.unit == L"bytes/s") { maximum = 1024; for (const auto& point : history.window(now)) maximum = std::max(maximum, point.value * 1.15); }
  drawGraph(dc, area, history, now, maximum, resource.color);
  RECT duration{area.left, area.bottom + scale(8), area.right, area.bottom + scale(30)}; const auto range = history.window(now); const auto text = number(range.size() > 1 ? range.back().time - range.front().time : 0, 0) + L" seconds   •   " + (resource.unit == L"bytes/s" ? bytes(maximum) + L"/s" : number(maximum) + resource.unit) + L" scale"; DrawTextW(dc, text.c_str(), -1, &duration, DT_LEFT | DT_SINGLELINE);
  RECT value{area.left, area.bottom + scale(47), area.right, bounds.bottom}; const auto summary = resource.unit == L"bytes/s" ? bytes(resource.value) + L"/s" : number(resource.value) + resource.unit; SelectObject(dc, titleFont); DrawTextW(dc, summary.c_str(), -1, &value, DT_LEFT | DT_SINGLELINE);
  if (hidden) paintTimes.push_back(milliseconds(started));
}
LRESULT CALLBACK Application::graphProcedure(HWND target, UINT message, WPARAM word, LPARAM data) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(target, GWLP_USERDATA));
  if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams); SetWindowLongPtrW(target, GWLP_USERDATA, LONG_PTR(app)); }
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_PAINT && app) { PAINTSTRUCT paint{}; HDC dc = BeginPaint(target, &paint); RECT bounds{}; GetClientRect(target, &bounds); HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, bounds.right), std::max(1L, bounds.bottom)); auto old = SelectObject(memory, bitmap); app->paint(memory, bounds); BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memory, 0, 0, SRCCOPY); SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); EndPaint(target, &paint); return 0; }
  return DefWindowProcW(target, message, word, data);
}
LRESULT CALLBACK Application::listProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context) {
  auto app = reinterpret_cast<Application*>(context);
  if (message == WM_CHAR && (app->hidden || !(GetKeyState(VK_CONTROL) & 0x8000))) { app->letter(wchar_t(word)); return 0; }
  if (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && word == 'C') { app->copy(); return 0; }
  if (message == WM_KEYDOWN && word == VK_DELETE && !app->hidden) { app->command(EndId); return 0; }
  if (message == WM_KEYUP && word == VK_CONTROL) app->consume();
  if (message == WM_NCDESTROY) RemoveWindowSubclass(target, listProcedure, id);
  return DefSubclassProc(target, message, word, data);
}
LRESULT CALLBACK Application::procedure(HWND target, UINT message, WPARAM word, LPARAM data) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(target, GWLP_USERDATA));
  if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams); app->window = target; SetWindowLongPtrW(target, GWLP_USERDATA, LONG_PTR(app)); }
  if (!app) return DefWindowProcW(target, message, word, data);
  switch (message) {
  case WM_CREATE: app->create(); return 0;
  case WM_SIZE: app->minimized = word == SIZE_MINIMIZED; if (app->minimized && app->hideMinimized) ShowWindow(target, SW_HIDE); if (!app->minimized) { app->layout(); app->consume(); } return 0;
  case WM_GETMINMAXINFO: reinterpret_cast<MINMAXINFO*>(data)->ptMinTrackSize = {app->scale(640), app->scale(400)}; return 0;
  case WM_DPICHANGED: { const auto rect = reinterpret_cast<RECT*>(data); SetWindowPos(target, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE); app->updateFonts(); app->layout(); return 0; }
  case SampleMessage: app->consume(); return 0;
  case IconMessage: app->icons->consume(); if (!app->minimized) InvalidateRect(app->list, nullptr, FALSE); return 0;
  case ActionMessage: { std::deque<ActionResult> results; { std::lock_guard lock(app->actionMutex); results.swap(app->actionResults); } for (const auto& result : results) { if (!app->hidden && !result.error.empty()) MessageBoxW(target, result.error.c_str(), L"Task Manager", MB_OK | MB_ICONERROR); if (!app->hidden && !result.report.empty()) DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(103), target, reportProcedure, LPARAM(result.report.c_str())); } app->refresh(); return 0; }
  case WM_APP + 4: if (data == WM_LBUTTONUP || data == WM_LBUTTONDBLCLK) { ShowWindow(target, SW_RESTORE); SetForegroundWindow(target); } else if (data == WM_RBUTTONUP) { POINT point{}; GetCursorPos(&point); HMENU popup = CreatePopupMenu(); AppendMenuW(popup, MF_STRING, ExitId, L"Exit"); SetForegroundWindow(target); TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, target, nullptr); DestroyMenu(popup); } return 0;
  case WM_APP + 5: { LARGE_INTEGER counter{}, frequency{}; QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency); app->deliveryTimes.push_back(1000.0 * double(counter.QuadPart - LONGLONG(word)) / double(frequency.QuadPart)); return 0; }
  case WM_TIMER: if (app->hidden) app->benchmarkTick(); else app->consume(); return 0;
  case WM_COMMAND:
    if (LOWORD(word) == SearchId && HIWORD(word) == EN_CHANGE) { const auto start = Clock::now(); std::wstring value(size_t(GetWindowTextLengthW(app->search)) + 1, L'\0'); GetWindowTextW(app->search, value.data(), int(value.size())); value.resize(wcslen(value.c_str())); app->filter = lower(value); app->rebuild(); if (app->hidden) app->searchTimes.push_back(milliseconds(start)); return 0; }
    if (LOWORD(word) == ResourcesId && HIWORD(word) == LBN_SELCHANGE) { app->resourceSelection(); return 0; }
    app->command(LOWORD(word)); return 0;
  case WM_NOTIFY: {
    const auto header = reinterpret_cast<NMHDR*>(data);
    if (header->hwndFrom == app->tabs && header->code == TCN_SELCHANGE) { app->saveSettings(); app->selectedTab = TabCtrl_GetCurSel(app->tabs); app->columns(); app->rebuild(); app->layout(); app->consume(); if (app->current && app->selectedTab == 1) { SendMessageW(app->resources, LB_RESETCONTENT, 0, 0); for (const auto& resource : app->current->resources) SendMessageW(app->resources, LB_ADDSTRING, 0, LPARAM(resource.name.c_str())); SendMessageW(app->resources, LB_SETCURSEL, WPARAM(app->selectedResource), 0); } return 0; }
    if (header->hwndFrom != app->list) break;
    if (header->code == LVN_GETDISPINFOW) { const auto info = reinterpret_cast<NMLVDISPINFOW*>(data); if (info->item.iItem >= 0 && size_t(info->item.iItem) < app->rows.size() && app->current) { const auto& row = app->rows[size_t(info->item.iItem)]; if (info->item.mask & LVIF_TEXT) { const auto text = app->cell(row, info->item.iSubItem); wcsncpy_s(info->item.pszText, size_t(info->item.cchTextMax), text.c_str(), _TRUNCATE); } if (info->item.mask & LVIF_IMAGE) info->item.iImage = row.process >= 0 ? app->icons->get(app->current->processes[size_t(row.process)]).icon : 0; if (info->item.mask & LVIF_INDENT) info->item.iIndent = row.indent; } return 0; }
    if (header->code == LVN_ODFINDITEMW) { const auto info = reinterpret_cast<NMLVFINDITEMW*>(data); std::vector<std::wstring> names; for (const auto& row : app->rows) names.push_back(app->cell(row, 0)); return info->lvfi.psz ? findNext(names, info->lvfi.psz, info->iStart) : -1; }
    if (header->code == LVN_COLUMNCLICK) { const int column = reinterpret_cast<NMLISTVIEW*>(data)->iSubItem; auto& sort = app->sortColumn[size_t(app->selectedTab)]; auto& direction = app->ascending[size_t(app->selectedTab)]; if (sort == column) direction = !direction; else { sort = column; direction = true; } app->rebuild(); return 0; }
    if (header->code == NM_DBLCLK) { const int index = app->selectedIndex(); if (index >= 0 && size_t(index) < app->rows.size()) { const auto key = app->rows[size_t(index)].key; if (app->expanded.contains(key)) app->expanded.erase(key); else app->expanded.insert(key); app->rebuild(); } return 0; }
    break;
  }
  case WM_DRAWITEM: {
    const auto item = reinterpret_cast<DRAWITEMSTRUCT*>(data); if (item->CtlID != ResourcesId || !app->current || item->itemID >= app->current->resources.size()) break;
    const auto& resource = app->current->resources[item->itemID]; HBRUSH background = CreateSolidBrush((item->itemState & ODS_SELECTED) ? RGB(225, 241, 255) : RGB(255, 255, 255)); FillRect(item->hDC, &item->rcItem, background); DeleteObject(background); SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, RGB(30, 30, 30)); SelectObject(item->hDC, app->font);
    RECT name = item->rcItem; name.left += app->scale(12); name.top += app->scale(8); name.right -= app->scale(70); DrawTextW(item->hDC, resource.name.c_str(), -1, &name, DT_SINGLELINE | DT_END_ELLIPSIS); name.top += app->scale(24); const auto value = resource.unit == L"bytes/s" ? bytes(resource.value) + L"/s" : number(resource.value) + resource.unit; DrawTextW(item->hDC, value.c_str(), -1, &name, DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT miniature = item->rcItem; miniature.left = miniature.right - app->scale(64); miniature.right -= app->scale(8); miniature.top += app->scale(12); miniature.bottom -= app->scale(12); drawGraph(item->hDC, miniature, app->histories[resource.key], app->graphTime, std::max(resource.maximum, resource.value), resource.color, false); return TRUE;
  }
  case WM_CONTEXTMENU: if (reinterpret_cast<HWND>(word) == ListView_GetHeader(app->list)) { HMENU popup = CreatePopupMenu(); const auto header = ListView_GetHeader(app->list); for (int column = 1; column < Header_GetItemCount(header); ++column) { wchar_t name[256]{}; HDITEMW item{}; item.mask = HDI_TEXT; item.pszText = name; item.cchTextMax = int(std::size(name)); Header_GetItem(header, column, &item); AppendMenuW(popup, MF_STRING | (ListView_GetColumnWidth(app->list, column) ? MF_CHECKED : 0), UINT_PTR(300 + column), name); } POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)}; if (point.x == -1) GetCursorPos(&point); TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, target, nullptr); DestroyMenu(popup); return 0; } else if (reinterpret_cast<HWND>(word) == app->list) { POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)}; if (point.x == -1) { GetCursorPos(&point); } else { POINT client = point; ScreenToClient(app->list, &client); LVHITTESTINFO hit{}; hit.pt = client; const int index = ListView_SubItemHitTest(app->list, &hit); if (index >= 0) app->select(index); } app->menu(point); return 0; } break;
  case WM_CLOSE: app->closing = true; app->saveSettings(); app->saveHistory(); app->stopping = true; app->wake.notify_all(); Shell_NotifyIconW(NIM_DELETE, &app->tray); DestroyWindow(target); return 0;
  case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(target, message, word, data);
}
#include "commands.inc"
