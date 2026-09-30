void Application::resourceSelection() { const int selected = int(SendMessageW(resources, LB_GETCURSEL, 0, 0)); if (selected >= 0) selectedResource = selected; InvalidateRect(graph, nullptr, FALSE); }
void Application::updateFonts() {
  const auto oldFont = font, oldTitle = titleFont; font = CreateFontW(-scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"); titleFont = CreateFontW(-scale(27), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI"); for (const auto control : {search, tabs, list, resources, end, compactButton, statusLabel}) SendMessageW(control, WM_SETFONT, WPARAM(font), TRUE); SendMessageW(resources, LB_SETITEMHEIGHT, 0, scale(74)); if (oldFont) DeleteObject(oldFont); if (oldTitle) DeleteObject(oldTitle);
}
void Application::copy() {
  const int index = selectedIndex(); if (index < 0) return; std::wstring text;
  for (int column = 0; column < Header_GetItemCount(ListView_GetHeader(list)); ++column) { if (column) text += L'\t'; text += cell(rows[size_t(index)], column); }
  if (!OpenClipboard(window)) return; HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t)); if (storage) { void* bytes = GlobalLock(storage); if (bytes) { memcpy(bytes, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(storage); EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, storage)) GlobalFree(storage); } else GlobalFree(storage); } CloseClipboard();
}
void Application::saveSettings() {
  auto& widths = columnWidths[size_t(selectedTab)]; widths.clear(); for (int column = 0; column < Header_GetItemCount(ListView_GetHeader(list)); ++column) widths.push_back(MulDiv(ListView_GetColumnWidth(list, column), 96, int(GetDpiForWindow(window))));
  if (hidden) return;
  auto write = [&](const std::wstring& section, const std::wstring& key, int value) { WritePrivateProfileStringW(section.c_str(), key.c_str(), std::to_wstring(value).c_str(), preferences.c_str()); };
  write(L"Window", L"Tab", selectedTab); write(L"Window", L"Interval", interval); write(L"Window", L"Topmost", topmost); write(L"Window", L"Hide", hideMinimized); write(L"Window", L"Group", grouped);
  RECT rect{}; GetWindowRect(window, &rect); if (!IsIconic(window) && !IsZoomed(window)) { write(L"Window", L"Width", rect.right - rect.left); write(L"Window", L"Height", rect.bottom - rect.top); }
  for (size_t tab = 0; tab < columnWidths.size(); ++tab) { const auto section = std::to_wstring(tab); for (size_t column = 0; column < columnWidths[tab].size(); ++column) write(section, L"Width" + std::to_wstring(column), columnWidths[tab][column]); write(section, L"Sort", sortColumn[tab]); write(section, L"Ascending", ascending[tab]); }
}
void Application::saveHistory() {
  if (hidden || historyFile.empty()) return; const auto temporary = historyFile + L".tmp";
  { std::wofstream stream{std::filesystem::path(temporary)}; for (const auto& [key, value] : usage) stream << key << L'\t' << value.ticks << L'\t' << value.name << L'\n'; if (!stream) return; }
  MoveFileExW(temporary.c_str(), historyFile.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}
void Application::loadHistory() {
  std::wifstream stream{std::filesystem::path(historyFile)}; std::wstring line; while (std::getline(stream, line) && usage.size() < 8192) { const auto first = line.find(L'\t'), second = first == line.npos ? line.npos : line.find(L'\t', first + 1); if (second == line.npos) continue; try { usage[line.substr(0, first)] = {line.substr(second + 1), std::stoull(line.substr(first + 1, second - first - 1))}; } catch (...) {} }
}
void Application::asynchronous(std::function<std::wstring()> operation) {
  if (hidden) return;
  if (actionBusy.exchange(true)) { MessageBoxW(window, L"An action is already in progress. The process list remains available.", L"Task Manager", MB_OK); return; }
  if (actionWorker.joinable()) actionWorker.join();
  actionWorker = std::thread([this, operation = std::move(operation)] { ActionResult result; try { result.report = operation(); } catch (const std::exception& error) { const auto text = error.what(); const int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0); if (needed > 0) { result.error.resize(size_t(needed)); MultiByteToWideChar(CP_UTF8, 0, text, -1, result.error.data(), needed); result.error.resize(size_t(needed - 1)); } }
    { std::lock_guard lock(actionMutex); actionResults.push_back(std::move(result)); } actionBusy = false; if (!stopping) PostMessageW(window, ActionMessage, 0, 0);
  });
}
void Application::menu(POINT point) {
  if (hidden) return; HMENU popup = CreatePopupMenu();
  auto add = [&](UINT id, const wchar_t* text) { AppendMenuW(popup, MF_STRING, id, text); };
  if (selectedProcess()) { add(200, L"End task"); add(201, L"Go to details"); add(202, L"Open file location"); add(203, L"Properties"); add(204, L"Search online"); add(210, L"Priority: Idle"); add(211, L"Priority: Normal"); add(212, L"Priority: Above normal"); add(213, L"Priority: High"); add(214, L"Enable efficiency mode"); add(215, L"Disable efficiency mode"); add(216, L"Set affinity…"); add(217, L"Create memory dump…"); add(218, L"Analyze wait chain"); }
  else if (selectedTab == 3) add(220, L"Enable / disable startup");
  else if (selectedTab == 4) { add(221, L"Disconnect"); add(222, L"Sign out…"); }
  else if (selectedTab == 6) { add(223, L"Start"); add(224, L"Stop"); add(225, L"Restart"); add(226, L"Open Services"); }
  add(205, L"Copy row"); TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr); DestroyMenu(popup);
}
void Application::command(int id) {
  if (id > 300 && id < 320) { const int column = id - 300; if (column < Header_GetItemCount(ListView_GetHeader(list))) { ListView_SetColumnWidth(list, column, ListView_GetColumnWidth(list, column) ? 0 : scale(120)); saveSettings(); } return; }
  if (id == ExitId) { SendMessageW(window, WM_CLOSE, 0, 0); return; }
  if (id == RefreshId) { refresh(); return; }
  if (id == CompactId) { compact = !compact; SetWindowTextW(compactButton, compact ? L"More details" : L"Fewer details"); layout(); return; }
  if (id == TopmostId) { topmost = !topmost; SetWindowPos(window, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE); }
  if (id == HideId) hideMinimized = !hideMinimized;
  if (id == GroupId) { grouped = !grouped; rebuild(); }
  if (id >= SpeedHigh && id <= SpeedPause) { interval = id == SpeedHigh ? 500 : id == SpeedNormal ? 1000 : id == SpeedLow ? 4000 : 0; refresh(); }
  if (id >= TopmostId && id <= SpeedPause) { CheckMenuItem(GetMenu(window), TopmostId, MF_BYCOMMAND | (topmost ? MF_CHECKED : MF_UNCHECKED)); CheckMenuItem(GetMenu(window), HideId, MF_BYCOMMAND | (hideMinimized ? MF_CHECKED : MF_UNCHECKED)); CheckMenuItem(GetMenu(window), GroupId, MF_BYCOMMAND | (grouped ? MF_CHECKED : MF_UNCHECKED)); saveSettings(); return; }
  if (hidden) return;
  if (id == RunId) { Input input; if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), window, inputProcedure, LPARAM(&input)) == IDOK && !input.text.empty()) { std::wstring file, parameters; if (input.text.front() == L'"') { const auto close = input.text.find(L'"', 1); if (close != input.text.npos) { file = input.text.substr(1, close - 1); parameters = input.text.substr(close + 1); } } else { const auto space = input.text.find(L' '); file = input.text.substr(0, space); if (space != input.text.npos) parameters = input.text.substr(space + 1); } if (file.empty()) return; const auto result = ShellExecuteW(window, input.elevated ? L"runas" : L"open", file.c_str(), parameters.c_str(), nullptr, SW_SHOWNORMAL); if (INT_PTR(result) <= 32) MessageBoxW(window, L"Could not launch the requested task.", L"Task Manager", MB_OK | MB_ICONERROR); } return; }
  if (id == 205) { copy(); return; }
  if (id == 226) { ShellExecuteW(window, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL); return; }
  const int index = selectedIndex(); if (index < 0 || size_t(index) >= rows.size()) return;
  if (id == EndId) id = selectedTab == 3 ? 220 : selectedTab == 4 && !selectedProcess() ? 221 : selectedTab == 6 ? 223 : 200;
  if (id >= 220 && id <= 225) { const auto entry = rows[size_t(index)].entry; if ((id == 221 || id == 222 || id == 224 || id == 225) && MessageBoxW(window, L"This may interrupt work or lose unsaved data. Continue?", L"Confirm action", MB_YESNO | MB_ICONWARNING) != IDYES) return; asynchronous([entry, id] { if (id == 220) startupAction(entry); else if (id == 221 || id == 222) sessionAction(entry, id == 222); else serviceAction(entry, id - 223); return std::wstring{}; }); return; }
  const auto selected = selectedProcess(); if (!selected) return; const auto process = *selected; const auto metadata = icons->get(process);
  if (id == 201) { selectedTab = 5; TabCtrl_SetCurSel(tabs, 5); SetWindowTextW(search, L""); columns(); rebuild(); layout(); for (size_t position = 0; position < rows.size(); ++position) if (rows[position].process >= 0 && current->processes[size_t(rows[position].process)].id == process.id) { select(int(position)); break; } return; }
  if (id == 202 || id == 203) { if (metadata.path.empty()) return; if (id == 202) ShellExecuteW(window, L"open", L"explorer.exe", (L"/select,\"" + metadata.path + L"\"").c_str(), nullptr, SW_SHOWNORMAL); else { SHELLEXECUTEINFOW info{sizeof(info)}; info.fMask = SEE_MASK_INVOKEIDLIST; info.hwnd = window; info.lpVerb = L"properties"; info.lpFile = metadata.path.c_str(); info.nShow = SW_SHOWNORMAL; ShellExecuteExW(&info); } return; }
  if (id == 204) { std::wstring escaped; for (const auto character : process.name) { if (iswalnum(character) || character == L'.' || character == L'-') escaped += character; else { wchar_t code[16]{}; swprintf_s(code, L"%%%02X", unsigned(character)); escaped += code; } } ShellExecuteW(window, L"open", (L"https://www.bing.com/search?q=" + escaped).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
  if (id == 218) { asynchronous([process] { return waitChain(process); }); return; }
  std::wstring argument; Action action = Action::End;
  if (id == 200 && MessageBoxW(window, L"Ending this task may lose unsaved data. Continue?", L"End task", MB_YESNO | MB_ICONWARNING) != IDYES) return;
  if (id >= 210 && id <= 215) { static constexpr Action choices[] = {Action::Idle, Action::Normal, Action::AboveNormal, Action::High, Action::EfficiencyOn, Action::EfficiencyOff}; action = choices[id - 210]; }
  else if (id == 216) { Input input{L"1"}; if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(102), window, inputProcedure, LPARAM(&input)) != IDOK) return; action = Action::Affinity; argument = input.text; }
  else if (id == 217) { if (MessageBoxW(window, L"Memory dumps may contain passwords and private data. Save a dump?", L"Create dump", MB_YESNO | MB_ICONWARNING) != IDYES) return; wchar_t path[32768] = L"process.dmp"; OPENFILENAMEW file{sizeof(file)}; file.hwndOwner = window; file.lpstrFile = path; file.nMaxFile = DWORD(std::size(path)); file.lpstrFilter = L"Memory dumps\0*.dmp\0"; file.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR; if (!GetSaveFileNameW(&file)) return; action = Action::Dump; argument = path; }
  else if (id != 200 && (id < 210 || id > 215)) return;
  asynchronous([process, action, argument] { processAction(process, action, argument); return std::wstring{}; });
}
void Application::benchmarkTick() {
  if (!current) { if (Clock::now() - shown > std::chrono::seconds(30)) { writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0); } return; }
  if (!measured && std::chrono::duration<double>(Clock::now() - shown).count() >= warmupSeconds) { measured = true; FILETIME created{}, exited{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &cpuStartKernel, &cpuStartUser); GetProcessHandleCount(GetCurrentProcess(), &baselineHandles); baselineGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); baselineUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS); deliveryTimes.clear(); updateTimes.clear(); paintTimes.clear(); searchTimes.clear(); samplerTimes.clear(); inputTimes.clear(); for (auto& times : stageTimes) times.clear(); }
  ++tests;
  if (!keyboardPassed || (!benchmarkIdle && !benchmarkMinimized)) {
  selectedTab = benchmarkTab >= 0 && keyboardPassed ? benchmarkTab : int(tests % 7); TabCtrl_SetCurSel(tabs, selectedTab); columns(); rebuild(); layout();
  if (selectedTab == 1) { HDC dc = GetDC(graph); HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, 800, 600); auto old = SelectObject(memory, bitmap); paint(memory, {0, 0, 800, 600}); SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(graph, dc); }
  if (selectedTab == 0 && !rows.empty()) { select(0); const auto name = cell(rows[0], 0); if (!name.empty()) { std::vector<std::wstring> names; for (const auto& row : rows) names.push_back(cell(row, 0)); prefix.clear(); const int expected = findNext(names, name.substr(0, 1), 1); SendMessageW(list, WM_CHAR, WPARAM(name[0]), 0); bool passed = selectedIndex() == expected; const int repeated = findNext(names, name.substr(0, 1), expected + 1); SendMessageW(list, WM_CHAR, WPARAM(name[0]), 0); passed = passed && selectedIndex() == repeated; keyboardPassed = tests <= 7 ? passed : keyboardPassed && passed; } SetWindowTextW(search, L"svchost"); SetWindowTextW(search, L""); }
  }
  if (std::chrono::duration<double>(Clock::now() - shown).count() >= benchmarkSeconds + warmupSeconds) {
    selectedTab = 1; layout();
    HDC screen = GetDC(window); HDC memory = CreateCompatibleDC(screen); HBITMAP bitmap = CreateCompatibleBitmap(screen, 1000, 700); auto old = SelectObject(memory, bitmap); paint(memory, {0, 0, 1000, 700});
    Gdiplus::Bitmap image(bitmap, nullptr); const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}}; image.Save((output + L".performance.png").c_str(), &png, nullptr); SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(window, screen);
    writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0);
  }
}
static void writeMetric(std::ostream& stream, const char* name, std::vector<double> values) {
  std::sort(values.begin(), values.end()); const double total = std::accumulate(values.begin(), values.end(), 0.0); stream << '"' << name << "\":{\"count\":" << values.size() << ",\"meanMilliseconds\":" << (values.empty() ? 0 : total / double(values.size())) << ",\"p95Milliseconds\":" << (values.empty() ? 0 : values[size_t(std::ceil(double(values.size()) * .95)) - 1]) << ",\"maxMilliseconds\":" << (values.empty() ? 0 : values.back()) << '}';
}
void Application::writeBenchmark() {
  exitCode = updates > 0 && keyboardPassed ? 0 : 1;
  if (output.empty()) return; std::ofstream stream{std::filesystem::path(output)}; FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user); PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)); DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
  const double elapsed = std::max(.001, std::chrono::duration<double>(Clock::now() - shown).count() - warmupSeconds); const double cpu = double(ticks(kernel) + ticks(user) - ticks(cpuStartKernel) - ticks(cpuStartUser)) / 10000000;
  stream << "{\"native\":true,\"passed\":" << (updates > 0 && keyboardPassed ? "true" : "false") << ",\"elapsedSeconds\":" << elapsed << ",\"firstSampleMilliseconds\":" << (firstSample == Time{} ? 0 : std::chrono::duration<double, std::milli>(firstSample - shown).count()) << ",\"processCpuPercentOneCore\":" << cpu * 100 / elapsed << ",\"privateBytes\":" << memory.PrivateUsage << ",\"handles\":" << handles << ",\"updates\":" << updates << ",\"exercises\":" << tests << ",\"keyboardPassed\":" << (keyboardPassed ? "true" : "false") << ',';
  stream << "\"warmupSeconds\":" << warmupSeconds << ",\"handleDelta\":" << int64_t(handles) - baselineHandles << ",\"gdiDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - baselineGdi << ",\"userDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - baselineUser << ',';
  writeMetric(stream, "sampler", samplerTimes); stream << ','; writeMetric(stream, "messageQueue", deliveryTimes); stream << ','; writeMetric(stream, "uiUpdate", updateTimes); stream << ','; writeMetric(stream, "graphPaint", paintTimes); stream << ','; writeMetric(stream, "search", searchTimes); stream << ','; writeMetric(stream, "letterNavigation", inputTimes); stream << ','; writeMetric(stream, "iconRequestToExtraction", icons->latencies); stream << ','; writeMetric(stream, "iconRequestToDisplay", icons->displayLatencies);
  static constexpr const char* stages[] = {"processInventory", "cpuAndMemory", "pdhAndDisk", "network", "gpuAggregation", "servicesSessionsStartup"}; for (size_t index = 0; index < stageTimes.size(); ++index) { stream << ','; writeMetric(stream, stages[index], stageTimes[index]); } stream << '}';
}
int runApplication(bool hidden, int benchmarkSeconds, const std::wstring& output, int intervalOverride, int tab, bool minimized, bool idle, int warmup) { Application application(hidden, benchmarkSeconds, output, intervalOverride, tab, minimized, idle, warmup); return application.run(); }
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES}; InitCommonControlsEx(&controls); const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); Gdiplus::GdiplusStartupInput input; ULONG_PTR token = 0; Gdiplus::GdiplusStartup(&token, &input, nullptr);
  int count = 0; auto arguments = CommandLineToArgvW(GetCommandLineW(), &count); std::vector<std::wstring> values; for (int index = 1; index < count; ++index) values.emplace_back(arguments[index]); LocalFree(arguments); int result = 0;
  try { result = values.empty() ? taskmgr::runApplication() : taskmgr::diagnostics(values); } catch (const std::exception& error) { if (values.empty()) MessageBoxA(nullptr, error.what(), "Task Manager", MB_OK | MB_ICONERROR); else { auto report = std::find(values.begin(), values.end(), L"--output"); if (report != values.end() && report + 1 != values.end()) { std::ofstream stream{std::filesystem::path(*(report + 1))}; stream << "{\"native\":true,\"passed\":false,\"error\":" << std::quoted(error.what()) << '}'; } } result = 1; }
  Gdiplus::GdiplusShutdown(token); if (SUCCEEDED(initialized)) CoUninitialize(); return result;
}
