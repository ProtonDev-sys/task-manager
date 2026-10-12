#include "app.hpp"
#include "atomic_file.hpp"
#include "report.hpp"
#include <dwmapi.h>
#include <windowsx.h>
#include <iterator>

namespace taskmgr {
bool Application::verifyBenchmarkSamplingHealth() {
  bool passed = true;
  const auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  GUID guid{}; wchar_t identifier[40]{}; std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  check(!error && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, identifier, int(std::size(identifier))) > 0, "benchmark-health-fixture-temporary-name");
  if (error || !*identifier) return false;
  const auto directory = temporary / (std::wstring(L"TaskManagerBenchmarkHealth-") + identifier);
  const bool created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
  check(created, "benchmark-health-fixture-directory"); if (!created) return false;
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
  RunOptions settings; settings.hidden = settings.idle = true; settings.output = (directory / L"health.json").wstring();
  Application fixture(settings); fixture.updates = 1; fixture.keyboardPassed = fixture.sortingPassed = true;
  fixture.icons = std::make_unique<Icons>(nullptr, scale(16), true, false);
  const auto report = [&](bool expected, const char* name) {
    fixture.writeBenchmark();
    std::ifstream file{std::filesystem::path(settings.output), std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto value = expected ? "true" : "false";
    check((fixture.exitCode == 0) == expected && contents.find(std::string("\"samplingHealthy\":") + value) != contents.npos &&
      contents.find(std::string("\"passed\":") + value) != contents.npos && contents.find("\"samplingFailure") == contents.npos, name);
  };
  report(false, "benchmark-health-requires-measurement-boundary");
  fixture.samplingFailures = 3; fixture.beginSamplingMeasurement();
  report(true, "benchmark-health-excludes-recovered-warmup-failures");
  ++fixture.samplingFailures; fixture.samplingFailed = true;
  report(false, "benchmark-health-rejects-current-measured-failure");
  fixture.samplingFailed = false;
  report(false, "benchmark-health-rejects-recovered-measured-failure");
  fixture.samplingFailed = true; fixture.beginSamplingMeasurement(); fixture.samplingFailed = false;
  report(false, "benchmark-health-rejects-unhealthy-measurement-start");
  fixture.beginSamplingMeasurement(); fixture.samplingFailed = true;
  report(false, "benchmark-health-requires-healthy-measurement-end");
  fixture.samplingFailed = false; fixture.interval = 0; fixture.beginSamplingMeasurement();
  report(true, "benchmark-health-allows-intentional-pause-without-new-samples");
  fixture.interval = 1000;
  for (const bool idle : {true, false}) {
    fixture.options.idle = idle; fixture.beginSamplingMeasurement(); ++fixture.samplingFailures;
    report(false, idle ? "benchmark-idle-rejects-sampling-failure" : "benchmark-interaction-rejects-sampling-failure");
  }
  return passed;
}
bool Application::verifyServiceStatus() {
  bool passed = true;
  const auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  Application fixture(RunOptions{}); fixture.selectedTab = ServicesTab;
  const HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 420, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(owner != nullptr, "service-status-fixture-owner"); if (!owner) return false;
  struct WindowCleanup { HWND value; ~WindowCleanup() { DestroyWindow(value); } } cleanup{owner};
  const auto control = [&](const wchar_t* type) { return CreateWindowExW(0, type, L"", WS_CHILD, 0, 0, 280, 34, owner, nullptr, GetModuleHandleW(nullptr), nullptr); };
  fixture.end = control(L"BUTTON"); fixture.fewer = control(L"BUTTON"); fixture.link = control(L"BUTTON"); fixture.serviceStatus = control(L"STATIC");
  check(fixture.end && fixture.fewer && fixture.link && fixture.serviceStatus, "service-status-fixture-controls");
  if (!fixture.end || !fixture.fewer || !fixture.link || !fixture.serviceStatus) return false;
  const auto text = [&] { wchar_t value[128]{}; GetWindowTextW(fixture.serviceStatus, value, 128); return std::wstring(value); };
  const auto visible = [&] { return (GetWindowLongPtrW(fixture.serviceStatus, GWL_STYLE) & WS_VISIBLE) != 0; };
  auto inventory = std::make_shared<ServiceInventory>(); fixture.current = std::make_shared<Sample>(); fixture.current->inventory = inventory;
  inventory->servicesError = ERROR_ACCESS_DENIED; fixture.updateFooter();
  check(visible() && text() == L"Services unavailable (error 5). Press F5 to retry.", "service-initial-failure-exposes-unavailable-status");
  inventory->services.push_back(Entry{}); fixture.current->services = inventory->services; fixture.updateFooter();
  check(visible() && text() == L"Services may be outdated (error 5). Press F5 to retry.", "service-retained-rows-expose-stale-status");
  const auto retained = fixture.current->services.data();
  inventory->servicesError = ERROR_SUCCESS; fixture.updateFooter();
  check(!visible() && text().empty() && fixture.current->services.data() == retained, "service-recovery-clears-status-with-unchanged-rows");
  inventory->servicesError = ERROR_INVALID_DATA; fixture.updateFooter();
  check(visible() && text().find(std::to_wstring(ERROR_INVALID_DATA)) != std::wstring::npos, "service-status-exposes-current-error-code");
  fixture.selectedTab = DetailsTab; fixture.updateFooter(); check(!visible() && text().empty(), "service-status-hidden-on-other-tabs");
  fixture.selectedTab = ServicesTab; fixture.compact = true; fixture.updateFooter(); check(!visible(), "service-status-hidden-in-compact-view");
  fixture.compact = false; fixture.summary = 1; fixture.updateFooter(); check(!visible(), "service-status-hidden-in-summary-view");
  fixture.summary = 0; fixture.current.reset(); fixture.updateFooter(); check(!visible() && text().empty(), "service-status-no-snapshot-is-inert");
  return passed;
}
bool Application::verifyTableExport() {
  bool passed = true;
  const auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  Application fixture(RunOptions{}); fixture.selectedTab = DetailsTab;
  const HWND owner = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(owner != nullptr, "export-fixture-owner"); if (!owner) return false;
  struct WindowCleanup { HWND value; ~WindowCleanup() { DestroyWindow(value); } } windowCleanup{owner};
  fixture.list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | LVS_REPORT, 0, 0, 300, 200, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  check(fixture.list != nullptr, "export-fixture-table"); if (!fixture.list) return false;
  fixture.header = ListView_GetHeader(fixture.list); fixture.shown[DetailsTab] = {0, 1, 2};
  const auto defs = definitions(DetailsTab);
  for (int index = 0; index < 3; ++index) { LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(defs[size_t(index)].title); column.cx = 100; check(ListView_InsertColumn(fixture.list, index, &column) == index, "export-fixture-column"); }
  const int order[] = {2, 0, 1}; check(ListView_SetColumnOrderArray(fixture.list, 3, order) != FALSE, "export-fixture-dragged-column-order");
  Row heading; heading.kind = RowKind::Heading; heading.cells = {{L"Category heading"}};
  Row first; first.key = L"first"; first.cells = {{L"alpha,\"quoted\"\n\u5de5\u5177 \U0001f680.exe"}, {L"42"}, {L"Running"}, {L"Hidden account"}};
  Row second; second.key = L"second"; second.cells = {{L"=1+1"}, {L"7"}, {L"Suspended"}};
  fixture.rows = {heading, second, first};
  const auto captured = fixture.tableCsv();
  const auto expected = std::string("\xef\xbb\xbf") + "\"Status\",\"Name\",\"PID\"\r\n\"Suspended\",\"'=1+1\",\"7\"\r\n\"Running\",\"alpha,\"\"quoted\"\"\n" + utf8(L"\u5de5\u5177 \U0001f680.exe") + "\",\"42\"\r\n";
  check(captured == expected, "export-visible-order-unicode-quotes-and-formula-text");
  check(captured.find("Category heading") == captured.npos && captured.find("Hidden account") == captured.npos, "export-omits-category-headings-and-hidden-columns");
  fixture.rows = {first}; fixture.shown[DetailsTab] = {2, 1, 0};
  check(captured == expected && fixture.tableCsv() != captured, "export-snapshot-survives-rows-and-columns-changing");
  fixture.compact = true; check(!fixture.canExportTable() && fixture.tableCsv().empty(), "export-unavailable-in-compact-view"); fixture.compact = false;
  fixture.selectedTab = PerformanceTab; check(!fixture.canExportTable(), "export-unavailable-for-performance-graphs"); fixture.selectedTab = DetailsTab;
  fixture.rows = {heading}; check(!fixture.canExportTable() && fixture.tableCsv().empty(), "export-unavailable-with-no-result-rows");
  GUID guid{}; wchar_t identifier[40]{}; std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  check(!error && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, identifier, int(std::size(identifier))) > 0, "export-fixture-temporary-name");
  if (error || !*identifier) return false;
  const auto directory = temporary / (std::wstring(L"TaskManagerExport-") + identifier);
  const bool created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
  check(created, "export-fixture-directory"); if (!created) return false;
  struct DirectoryCleanup { std::filesystem::path path; ~DirectoryCleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } directoryCleanup{directory};
  const auto path = (directory / L"\u5de5\u5177.csv").wstring();
  const auto read = [&]() { std::ifstream file{std::filesystem::path(path), std::ios::binary}; return std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()}; };
  try {
    writeExportFile(path, captured); check(read() == captured, "export-writes-exact-utf8-snapshot");
    const auto longName = (directory / (std::wstring(240, L'x') + L".csv")).wstring();
    const auto extendedName = longName.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + longName.substr(2) : L"\\\\?\\" + longName;
    writeExportFile(extendedName, captured);
    { std::ifstream file{std::filesystem::path(extendedName), std::ios::binary}; const std::string contents{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
      check(contents == captured, "export-valid-long-filename-does-not-lengthen-staging-basename"); }
    check(DeleteFileW(extendedName.c_str()) != FALSE, "export-long-filename-fixture-cleanup");
    const std::string large(1024 * 1024 + 17, 'x');
    writeExportFile(path, large); check(read() == large, "export-replaces-existing-file-across-write-chunks");
    bool failed = false;
    { Handle lock(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      check(lock.value != INVALID_HANDLE_VALUE, "export-fixture-locks-existing-destination");
      if (lock.value != INVALID_HANDLE_VALUE) { try { writeExportFile(path, "replacement"); } catch (const std::exception&) { failed = true; } }
    }
    check(failed && read() == large, "export-failed-replacement-preserves-existing-destination");
    failed = false; try { writeExportFile((directory / L"missing" / L"file.csv").wstring(), captured); } catch (const std::exception&) { failed = true; }
    check(failed, "export-missing-parent-reported");
    size_t files = 0; for (const auto& entry : std::filesystem::directory_iterator(directory)) { static_cast<void>(entry); ++files; }
    check(files == 1, "export-failure-cleans-only-owned-temporary-files");
  } catch (const std::exception&) { check(false, "export-native-file-operations"); }
  return passed;
}
bool Application::verifyActionSnapshots() {
  const auto savedSample = current; const int savedTab = selectedTab, savedSelection = selectedIndex(), savedTop = ListView_GetTopIndex(list);
  const bool savedRebuilding = rebuilding, wasVisible = (GetWindowLongPtrW(list, GWL_STYLE) & WS_VISIBLE) != 0;
  rebuilding = true;
  auto savedRows = std::move(rows);
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  auto sample = std::make_shared<Sample>(); sample->processes.resize(3);
  sample->processes[0].id = 900001; sample->processes[0].created = 100; sample->processes[0].name = L"Original process";
  sample->processes[1].id = 900002; sample->processes[1].parent = 900001; sample->processes[1].created = 101;
  sample->processes[2].id = 900003; sample->processes[2].parent = 900001; sample->processes[2].created = 99;
  current = sample; selectedTab = DetailsTab;
  Row row; row.key = L"action-fixture"; row.process = 0; row.members = {0}; rows = {row};
  ListView_SetItemCountEx(list, 1, LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL); select(0);
  const auto process = selectedProcess(); const auto members = selectedMembers(); const auto tree = selectedMembers(true);
  rows[0].kind = RowKind::Window; rows[0].members.clear();
  const auto windowMembers = selectedMembers();
  rows[0].process = -1; check(selectedMembers().empty(), "action-window-without-owner-has-no-target"); rows[0].process = 0;
  auto inventory = std::make_shared<ServiceInventory>(); Entry user; user.key = L"original-session"; user.session = 123; user.cells = {L"Original user"}; inventory->sessions = {user};
  sample->inventory = inventory; sample->sessions = inventory->sessions;
  selectedTab = UsersTab; rows[0].kind = RowKind::User; rows[0].entry = 0;
  const auto session = selectedEntry();
  auto startup = std::make_shared<StartupInventory>(); Entry item; item.key = L"original-startup"; item.enabled = true; item.path = L"C:\\Fixture\\original.exe"; startup->entries = {item}; sample->startup = startup;
  selectedTab = StartupTab; const auto startupEntry = selectedEntry();
  item.key = L"original-service"; inventory->services = {item}; sample->services = inventory->services;
  selectedTab = ServicesTab; const auto service = selectedEntry();

  // Simulate a refresh and selection change while a confirmation dialog is open.
  // Reuse the old fixture storage as well, so borrowed references fail deterministically.
  current = std::make_shared<Sample>(); rows[0].key = L"replacement-selection";
  sample->processes[0].created = 200; sample->processes[0].name = L"Reused PID";
  sample->processes[1].id = 900004; inventory->sessions[0].session = 456; inventory->sessions[0].cells[0] = L"Replacement user";
  startup->entries[0].enabled = false; startup->entries[0].path = L"C:\\Fixture\\replacement.exe"; inventory->services[0].key = L"replacement-service";
  check(process && process->id == 900001 && process->created == 100 && process->name == L"Original process", "action-process-keeps-confirmed-identity");
  check(members.size() == 1 && members[0].id == 900001 && members[0].created == 100, "action-members-own-process-snapshots");
  check(windowMembers.size() == 1 && windowMembers[0].id == 900001 && windowMembers[0].created == 100, "action-window-targets-owning-process-snapshot");
  check(tree.size() == 2 && tree[0].id == 900002 && tree[1].id == 900001 && tree[1].created == 100, "action-tree-keeps-original-descendants");
  check(session && session->session == 123 && session->cells[0] == L"Original user", "action-session-keeps-confirmed-target");
  check(startupEntry && startupEntry->enabled && startupEntry->path == L"C:\\Fixture\\original.exe", "action-startup-entry-owns-snapshot");
  check(service && service->key == L"original-service", "action-service-entry-owns-snapshot");
  rows[0].kind = RowKind::Window;
  check(!selectedProcess() && !selectedEntry() && selectedMembers().empty(), "action-stale-selection-has-no-target");

  sample = std::make_shared<Sample>(); sample->processes.resize(5000);
  Row group; group.kind = RowKind::Group; group.key = L"large-action-group";
  for (size_t index = 0; index < sample->processes.size(); ++index) {
    auto& member = sample->processes[index]; member.id = DWORD(900000 + index); member.parent = index ? member.id - 1 : 0; member.created = 1000 + index;
    group.members.push_back(int(sample->processes.size() - index - 1));
  }
  current = sample; selectedTab = ProcessesTab; rows = {std::move(group)}; select(0);
  const auto groupedTree = selectedMembers(true);
  bool completeGroup = groupedTree.size() == sample->processes.size();
  for (size_t index = 0; completeGroup && index < groupedTree.size(); ++index) completeGroup = groupedTree[index].id == sample->processes[groupedTree.size() - index - 1].id;
  check(completeGroup, "action-group-tree-complete-unique-and-children-first");
  sample->processes.clear();
  check(groupedTree.size() == 5000 && groupedTree.front().created == 5999 && groupedTree.back().created == 1000, "action-group-tree-owns-large-snapshot");

  current = savedSample; selectedTab = savedTab; rows = std::move(savedRows);
  ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  if (savedSelection >= 0) select(savedSelection);
  ListView_Scroll(list, 0, (savedTop - ListView_GetTopIndex(list)) * rowHeight());
  SendMessageW(list, WM_SETREDRAW, TRUE, 0);
  if (!wasVisible) ShowWindow(list, SW_HIDE);
  rebuilding = savedRebuilding;
  return passed;
}
bool Application::verifyHistoryPersistence() {
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  auto saved = std::move(usage);
  struct Restore { decltype(usage)& target; decltype(usage)& original; ~Restore() { target = std::move(original); } } restore{usage, saved};
  GUID guid{}; wchar_t identifier[40]{}; std::error_code error;
  const auto temporary = std::filesystem::temp_directory_path(error);
  check(!error && SUCCEEDED(CoCreateGuid(&guid)) && StringFromGUID2(guid, identifier, int(std::size(identifier))) > 0, "history-fixture-temporary-name");
  if (error || !*identifier) return false;
  const auto directory = temporary / (std::wstring(L"TaskManagerHistory-") + identifier);
  const bool created = CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
  check(created, "history-fixture-directory"); if (!created) return false;
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
  const auto path = (directory / L"history.tsv").wstring(), neighbor = path + L".tmp";
  const auto read = [](const std::wstring& file) { std::ifstream input{std::filesystem::path(file), std::ios::binary}; return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()}; };
  const auto stagingFiles = [&] { size_t count = 0; for (const auto& entry : std::filesystem::directory_iterator(directory)) { const auto name = entry.path().filename().wstring(); if (name.starts_with(L".taskmgr-") && name.ends_with(L".tmp")) ++count; } return count; };
  constexpr std::string_view sentinel = "A preexisting temporary neighbor belongs to another operation.";
  { std::ofstream fixture{std::filesystem::path(neighbor), std::ios::binary}; fixture << sentinel; }
  check(read(neighbor) == sentinel, "history-preexisting-temporary-neighbor-fixture");
  const std::wstring key = L"\u5de5\u5177.exe", name = L"Caf\u00e9 \U0001f680\tTabbed\r\nsecond line\\literal", executablePath = L"C:\\\u7528\u6237\\M\u00fcnchen\\\u5de5\u5177.exe";
  usage.clear(); usage[key] = {name, executablePath, 123456789, true}; usage[L"empty.exe"] = {L"", L"", 0, false};
  check(writeHistoryFile(path), "history-unicode-write");
  {
    const auto bytes = read(path);
    check(bytes.find("\xe5\xb7\xa5\xe5\x85\xb7.exe") != std::string::npos && bytes.find("\xf0\x9f\x9a\x80") != std::string::npos, "history-explicit-utf8-bytes");
    check(std::count(bytes.begin(), bytes.end(), '\n') == 3, "history-metadata-newlines-escaped");
    const std::string historyHeader = "# TaskManagerNative history UTF-8 v1\n", emptyRow = "empty.exe\t0\t\t\t0\n";
    const std::string unicodeRow = "\xe5\xb7\xa5\xe5\x85\xb7.exe\t123456789\tCaf\xc3\xa9 \xf0\x9f\x9a\x80\\tTabbed\\r\\nsecond line\\\\literal\tC:\\\\\xe7\x94\xa8\xe6\x88\xb7\\\\M\xc3\xbcnchen\\\\\xe5\xb7\xa5\xe5\x85\xb7.exe\t1\n";
    check(bytes == historyHeader + emptyRow + unicodeRow || bytes == historyHeader + unicodeRow + emptyRow, "history-serialization-preserves-exact-format-bytes");
  }
  check(read(neighbor) == sentinel && stagingFiles() == 0, "history-success-preserves-preexisting-temporary-neighbor");
  usage.clear(); readHistoryFile(path);
  const auto found = usage.find(key), empty = usage.find(L"empty.exe");
  check(usage.size() == 2 && found != usage.end() && found->second.name == name && found->second.path == executablePath && found->second.ticks == 123456789 && found->second.app, "history-unicode-round-trip");
  check(empty != usage.end() && empty->second.name.empty() && empty->second.path.empty() && empty->second.ticks == 0 && !empty->second.app, "history-empty-fields-round-trip");
  usage.clear(); usage[L"max.exe"] = {L"Maximum", L"", UINT64_MAX, false};
  check(writeHistoryFile(path) && read(path) == "# TaskManagerNative history UTF-8 v1\nmax.exe\t18446744073709551615\tMaximum\t\t0\n", "history-full-width-ticks-keep-decimal-format");
  usage.clear(); readHistoryFile(path);
  check(usage.size() == 1 && usage.contains(L"max.exe") && usage.at(L"max.exe").ticks == UINT64_MAX, "history-full-width-ticks-round-trip");
  {
    std::ofstream legacy{std::filesystem::path(path), std::ios::binary};
    legacy << "legacy.exe\t42\tLegacy app\tC:\\Program Files\\Legacy\\tool.exe\t1\r\nold.exe\t7\tOld app\r\n";
  }
  usage.clear(); readHistoryFile(path);
  const auto legacy = usage.find(L"legacy.exe"), old = usage.find(L"old.exe");
  check(usage.size() == 2 && legacy != usage.end() && legacy->second.path == L"C:\\Program Files\\Legacy\\tool.exe" && legacy->second.ticks == 42 && legacy->second.app && old != usage.end() && old->second.path.empty() && old->second.ticks == 7, "history-legacy-ascii-compatible");
  usage.clear(); check(writeHistoryFile(path), "history-invalid-fixture-header");
  {
    std::ofstream malformed{std::filesystem::path(path), std::ios::binary | std::ios::app};
    malformed << "\xff.exe\t1\tBad UTF-8\t\t0\nnegative.exe\t-1\tNegative\t\t0\npartial.exe\t12x\tPartial\t\t0\nescape.exe\t1\tBad\\q\t\t0\ngood.exe\t9\tGood\t\t1\n";
  }
  readHistoryFile(path);
  check(usage.size() == 1 && usage.contains(L"good.exe") && usage.at(L"good.exe").ticks == 9, "history-malformed-records-skipped");
  try {
    const auto before = read(path);
    bool failed = false;
    { Handle lock(CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
      check(lock.value != INVALID_HANDLE_VALUE, "history-fixture-locks-existing-destination");
      if (lock.value != INVALID_HANDLE_VALUE) failed = !writeHistoryFile(path); }
    check(failed && read(path) == before, "history-locked-destination-is-preserved");
    check(read(neighbor) == sentinel && stagingFiles() == 0, "history-locked-save-cleans-only-owned-temporary");
    check(!writeHistoryFile((directory / L"missing" / L"history.tsv").wstring()), "history-missing-parent-reported");
    try {
      AtomicFile interrupted(path); interrupted.append(std::string(64 * 1024 + 17, 'x'));
      throw std::runtime_error("Owned serialization failure fixture");
    } catch (const std::exception&) {}
    check(read(path) == before && read(neighbor) == sentinel && stagingFiles() == 0, "history-aborted-serialization-preserves-destination-and-cleans-owned-file");

    // Both writers have open staging files at once. Committing either one must
    // leave the other one's complete snapshot available for its own commit.
    const std::string firstContents(64 * 1024 + 19, 'a'), secondContents(64 * 1024 + 31, 'b');
    { AtomicFile first(path), second(path); first.append(firstContents); second.append(secondContents);
      check(stagingFiles() == 2, "history-overlapping-writers-own-distinct-staging-files");
      first.commit(); check(read(path) == firstContents && stagingFiles() == 1, "history-first-commit-preserves-other-writers-staging");
      second.commit(); check(read(path) == secondContents && stagingFiles() == 0, "history-second-commit-publishes-complete-replacement"); }

    Application first(RunOptions{}), second(RunOptions{});
    for (unsigned index = 0; index < 160; ++index) {
      first.usage[L"first-" + std::to_wstring(index)] = {std::wstring(1024, L'a'), L"C:\\first.exe", index, true};
      second.usage[L"second-" + std::to_wstring(index)] = {std::wstring(1024, L'b'), L"C:\\second.exe", index + 1000u, false};
    }
    first.usage[L"large-first"] = {std::wstring(64 * 1024 + 17, L'a'), L"", UINT64_MAX, true};
    second.usage[L"large-second"] = {std::wstring(64 * 1024 + 31, L'b'), L"", UINT64_MAX, false};
    const auto firstPath = (directory / L"first.tsv").wstring(), secondPath = (directory / L"second.tsv").wstring();
    check(first.writeHistoryFile(firstPath) && second.writeHistoryFile(secondPath), "history-concurrent-snapshot-fixtures");
    const auto firstSnapshot = read(firstPath), secondSnapshot = read(secondPath);
    unsigned firstCompleted = 0, secondCompleted = 0;
    { std::jthread firstWriter([&] { for (unsigned attempt = 0; attempt < 4; ++attempt) if (first.writeHistoryFile(path)) ++firstCompleted; });
      std::jthread secondWriter([&] { for (unsigned attempt = 0; attempt < 4; ++attempt) if (second.writeHistoryFile(path)) ++secondCompleted; }); }
    const auto concurrent = read(path);
    check(firstCompleted + secondCompleted > 0 && !firstSnapshot.empty() && !secondSnapshot.empty() && (concurrent == firstSnapshot || concurrent == secondSnapshot), "history-concurrent-saves-publish-one-complete-snapshot");
    usage.clear(); readHistoryFile(path);
    check(usage.size() == 161 && (usage.contains(L"large-first") != usage.contains(L"large-second")), "history-concurrent-snapshot-remains-readable");
    check(read(neighbor) == sentinel && stagingFiles() == 0, "history-concurrent-writers-clean-only-owned-files");
  } catch (const std::exception&) { check(false, "history-native-file-operations"); }
  const bool blocked = DeleteFileW(path.c_str()) && CreateDirectoryW(path.c_str(), nullptr);
  check(blocked, "history-rename-failure-fixture");
  if (blocked) {
    check(!writeHistoryFile(path), "history-rename-failure-reported");
    check(std::filesystem::is_directory(std::filesystem::path(path), error) && !error, "history-directory-destination-is-preserved");
    check(read(neighbor) == sentinel && stagingFiles() == 0, "history-rename-failure-cleans-only-owned-temporary");
  }
  return passed;
}
bool Application::verifyTrayLifecycle() {
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  // This fixture has no window, sampler or persistent files. Intercept every
  // notification so only its owned GDI objects are created, never shell icons.
  Application fixture(RunOptions{});
  std::vector<DWORD> messages; bool acceptModify = true, acceptAdd = true;
  fixture.trayNotification = [&](DWORD message) { messages.push_back(message); return message == NIM_ADD ? acceptAdd : acceptModify; };
  fixture.current = std::make_shared<Sample>(); fixture.current->cpu = 10;
  fixture.pending = std::make_shared<Sample>(); fixture.pending->cpu = 75; fixture.pending->memory.total = 100; fixture.pending->memory.available = 40;
  const auto displayed = fixture.current, newest = fixture.pending;
  fixture.minimized = true; fixture.notification = true; fixture.consume();
  check(fixture.trayAdded && fixture.trayLevel == 11 && std::wstring_view(fixture.tray.szTip).starts_with(L"CPU: 75%\nMemory: 60%"), "minimized-tray-uses-new-sample");
  check(messages == std::vector<DWORD>{NIM_ADD} && !fixture.notification, "minimized-tray-acknowledges-notification");
  check(fixture.current == displayed && fixture.pending == newest && fixture.fullRebuilds == 0 && fixture.updates == 0 && fixture.histories.empty() && fixture.usage.empty(), "minimized-tray-retains-display-snapshot-without-rebuilding");
  messages.clear(); acceptModify = acceptAdd = false;
  fixture.pending = std::make_shared<Sample>(); fixture.pending->cpu = 25;
  const auto following = fixture.pending; fixture.notification = true; fixture.consume();
  check(messages == std::vector<DWORD>{NIM_MODIFY, NIM_ADD} && !fixture.trayAdded && fixture.trayLevel == 4 && std::wstring_view(fixture.tray.szTip).starts_with(L"CPU: 25%"), "tray-notification-failure-keeps-new-meter");
  check(fixture.current == displayed && fixture.pending == following && !fixture.notification, "minimized-tray-keeps-latest-pending-snapshot");
  messages.clear(); acceptAdd = true; fixture.interval = 0; fixture.publishTray();
  check(fixture.trayAdded && messages == std::vector<DWORD>{NIM_ADD} && fixture.pending == following && !fixture.force && fixture.interval == 0, "paused-tray-retry-does-not-refresh-sample");
  messages.clear(); fixture.closing = true; fixture.publishTray();
  check(messages.empty(), "closing-tray-does-not-register-again");

  // Verify the actual registered-message and timer routes on this app's hidden
  // test window. Its tray callback is intercepted for the entire scope.
  auto savedCallback = std::move(trayNotification); const bool savedAdded = trayAdded; const int savedInterval = interval.load();
  struct Restore {
    std::function<bool(DWORD)>& callback; std::function<bool(DWORD)>& original;
    bool& added; bool oldAdded; std::atomic<int>& interval; int oldInterval; std::condition_variable& wake;
    ~Restore() { callback = std::move(original); added = oldAdded; interval = oldInterval; wake.notify_one(); }
  } restore{trayNotification, savedCallback, trayAdded, savedAdded, interval, savedInterval, wake};
  messages.clear(); bool available = false;
  trayNotification = [&](DWORD message) { messages.push_back(message); return available; };
  const auto sampleBefore = current; const unsigned rebuildsBefore = fullRebuilds;
  const std::wstring tipBefore = tray.szTip; const HICON iconBefore = tray.hIcon;
  trayAdded = true; interval = 0;
  check(taskbarCreatedMessage != 0, "taskbar-created-message-registered");
  if (taskbarCreatedMessage) {
    SendMessageW(window, taskbarCreatedMessage, 0, 0);
    check(messages == std::vector<DWORD>{NIM_ADD} && !trayAdded, "taskbar-created-readds-lost-icon");
    messages.clear(); available = true; SendMessageW(window, WM_TIMER, 1, 0);
    check(messages == std::vector<DWORD>{NIM_ADD} && trayAdded, "taskbar-created-retries-failed-registration");
    check(current == sampleBefore && fullRebuilds == rebuildsBefore && interval == 0 && tray.szTip == tipBefore && tray.hIcon == iconBefore, "paused-taskbar-recovery-retains-meter-and-display");
  }
  return passed;
}
bool Application::verifyViewportRefresh() {
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  const auto savedSample = current; const int savedTab = selectedTab, savedColumnsTab = columnsTab, savedSort = sortColumn[DetailsTab];
  const bool savedDirection = ascending[DetailsTab], savedCompact = compact, savedSortQueued = sortQueued;
  const int savedTop = ListView_GetTopIndex(list), savedSelection = selectedIndex(), savedFocused = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
  const int savedOrderedTab = orderedTab, savedOrderedColumn = orderedColumn; const bool savedOrderedAscending = orderedAscending;
  const HWND savedFocus = GetFocus();
  auto savedRows = std::move(rows), savedSearchRows = std::move(searchRows); auto savedSearch = std::move(searchText);
  rows.clear(); searchRows.clear(); searchText.clear(); compact = false; selectedTab = DetailsTab;
  auto sample = std::make_shared<Sample>(); sample->processes.resize(256);
  for (size_t index = 0; index < sample->processes.size(); ++index) {
    // PID zero prevents metadata requests; distinct creation stamps give anonymous row identities.
    auto& process = sample->processes[index]; process.created = uint64_t(index) + 1;
    process.name = L"Viewport fixture " + std::to_wstring(index); process.cpu = double(index) / 3;
  }
  current = sample; sortColumn[DetailsTab] = 5; ascending[DetailsTab] = true; sortQueued = false; columns(); layout(); rebuild();
  select(10); SendMessageW(list, WM_VSCROLL, SB_TOP, 0); ListView_Scroll(list, 0, rowHeight() * 80);
  const int top = ListView_GetTopIndex(list); const auto selected = rowKey(selectedIndex());
  check(top > 10 && !selected.empty(), "viewport-fixture-selection-outside-view");
  ListView_SetItemState(list, -1, 0, LVIS_FOCUSED); ListView_SetItemState(list, 20, LVIS_FOCUSED, LVIS_FOCUSED);
  const auto focused = rowKey(20); sample->processes[20].cpu = 95;
  sample->processes[10].cpu = 99; rebuild();
  check(rowKey(selectedIndex()) == selected && selectedIndex() == 255, "numeric-refresh-retains-selected-identity-at-new-rank");
  check(rowKey(ListView_GetNextItem(list, -1, LVNI_FOCUSED)) == focused && ListView_GetNextItem(list, -1, LVNI_FOCUSED) != selectedIndex(), "numeric-refresh-retains-independent-keyboard-focus");
  check(ListView_GetTopIndex(list) == top, "numeric-refresh-does-not-scroll-to-lower-rank");
  sample->processes[10].cpu = 0; rebuild();
  check(rowKey(selectedIndex()) == selected && selectedIndex() <= 1, "numeric-refresh-retains-selected-identity-at-top");
  check(ListView_GetTopIndex(list) == top, "numeric-refresh-does-not-scroll-to-upper-rank");
  sample->processes.erase(sample->processes.begin() + 10); rebuild();
  check(selectedIndex() < 0 && rowKey(ListView_GetNextItem(list, -1, LVNI_FOCUSED)) == focused, "refresh-retains-keyboard-focus-when-selected-process-exits");
  searchText = L"name:\"viewport fixture\""; rebuild(); ValidateRect(list, nullptr); rebuild();
  check(!GetUpdateRect(list, nullptr, FALSE), "unchanged-filtered-snapshot-no-table-repaint");
  sample->processes[0].privateWorking += 1024; rebuild();
  check(!GetUpdateRect(list, nullptr, FALSE), "filtered-offscreen-cell-change-no-table-repaint");
  const int visible = ListView_GetTopIndex(list);
  if (visible >= 0 && size_t(visible) < rows.size()) {
    sample->processes[size_t(rows[size_t(visible)].process)].privateWorking += 1024; rebuild();
    RECT painted{}, rowBounds{}; ListView_GetItemRect(list, visible, &rowBounds, LVIR_BOUNDS);
    check(GetUpdateRect(list, &painted, FALSE) && painted.top >= rowBounds.top && painted.bottom <= rowBounds.bottom, "filtered-visible-cell-change-repaints-only-changed-row");
  } else check(false, "filtered-viewport-fixture-has-visible-row");
  searchText = L"name:no-matching-viewport-fixture"; rebuild();
  check(rows.empty() && ListView_GetItemCount(list) == 0, "filtered-membership-change-updates-native-item-count");
  searchText.clear(); rebuild();
  select(200);
  check(ListView_GetTopIndex(list) <= 200 && ListView_GetTopIndex(list) + ListView_GetCountPerPage(list) >= 200, "explicit-selection-still-scrolls-into-view");
  const auto explicitSelection = rowKey(selectedIndex()); ascending[DetailsTab] = false; rebuild(true);
  check(ListView_GetTopIndex(list) == 0 && rowKey(selectedIndex()) == explicitSelection, "manual-sort-still-starts-at-top");
  current = savedSample; sortColumn[DetailsTab] = savedSort; ascending[DetailsTab] = savedDirection; compact = savedCompact;
  // Restore the retained table even when the caller was displaying Performance.
  selectedTab = savedColumnsTab >= 0 ? savedColumnsTab : savedTab; columns(); rebuilding = true;
  rows = std::move(savedRows); searchRows = std::move(savedSearchRows); searchText = std::move(savedSearch);
  orderedTab = savedOrderedTab; orderedColumn = savedOrderedColumn; orderedAscending = savedOrderedAscending;
  ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL);
  ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  if (savedSelection >= 0) ListView_SetItemState(list, savedSelection, LVIS_SELECTED, LVIS_SELECTED);
  if (savedFocused >= 0) ListView_SetItemState(list, savedFocused, LVIS_FOCUSED, LVIS_FOCUSED);
  ListView_Scroll(list, 0, (savedTop - ListView_GetTopIndex(list)) * rowHeight());
  selectedTab = savedTab; rebuilding = false; layout(); updateFooter();
  if (GetFocus() != savedFocus) SetFocus(savedFocus);
  sortQueued = savedSortQueued; if (sortQueued) SetTimer(window, 3, 32, nullptr);
  return passed;
}
bool Application::verifySearch() {
  bool passed = true;
  const auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  const auto savedSample = current; const auto savedSearch = searchText; const int savedTab = selectedTab;
  auto sample = std::make_shared<Sample>(); sample->processes.resize(3);
  sample->processes[0].id = 4242; sample->processes[0].created = UINT64_MAX - 10; sample->processes[0].name = L"browser.exe";
  sample->processes[1].id = 14242; sample->processes[1].created = UINT64_MAX - 11; sample->processes[1].name = L"renderer.exe"; sample->processes[1].suspended = true;
  sample->processes[2].id = 8000; sample->processes[2].created = UINT64_MAX - 12; sample->processes[2].name = L"editor.exe"; sample->processes[2].hung = true;
  Metadata browser; browser.description = L"Orbit Browser"; browser.publisher = L"Orbit Labs"; browser.user = L"LOCAL\\Alice";
  browser.path = L"C:\\Program Files\\Orbit\\browser.exe"; browser.commandLine = L"\"C:\\Program Files\\Orbit\\browser.exe\" --profile Main";
  Metadata renderer; renderer.description = L"Orbit Renderer"; renderer.publisher = L"Renderer Labs"; renderer.user = L"LOCAL\\Guest";
  renderer.path = L"C:\\Program Files\\Orbit\\renderer.exe"; renderer.commandLine = L"renderer.exe --utility";
  Metadata editor; editor.description = L"Text Editor"; editor.publisher = L"Editor Labs"; editor.path = L"C:\\Tools\\editor.exe";
  icons->setTestMetadata(sample->processes[0], browser); icons->setTestMetadata(sample->processes[1], renderer); icons->setTestMetadata(sample->processes[2], editor);
  auto users = std::make_shared<std::unordered_map<DWORD, std::wstring>>(); (*users)[8000] = L"LOCAL\\Alice"; sample->users = users;
  current = sample; selectedTab = ProcessesTab;
  std::vector<Row> singles(3);
  for (size_t index = 0; index < singles.size(); ++index) { singles[index].key = std::to_wstring(index); singles[index].process = int(index); singles[index].members = {int(index)}; }
  const auto keys = [&](std::wstring_view query, const std::vector<Row>& source) {
    searchText = query; std::wstring result;
    for (const auto& row : filteredRows(source)) result += row.key;
    return result;
  };
  check(keys(L"4242", singles) == L"01", "plain-pid-retains-substring-search");
  check(keys(L"pid:4242", singles) == L"0" && keys(L"PID:0004242", singles) == L"0", "scoped-pid-is-exact-and-case-insensitive");
  check(keys(L"pid:4294967296", singles).empty() && keys(L"pid:42x", singles).empty(), "scoped-invalid-pid-never-matches");
  check(keys(L"name:browser publisher:\"Orbit Labs\"", singles) == L"0", "scoped-fields-and-quoted-values");
  check(keys(L"name:\"Orbit Labs\"", singles).empty(), "scoped-name-does-not-match-publisher");
  check(keys(L"path:\"C:\\Program Files\\Orbit\\\" -status:suspended", singles) == L"0", "scoped-path-and-status-exclusion");
  check(keys(L"command:\"--profile Main\"", singles) == L"0", "scoped-command-preserves-switches-and-spaces");
  check(keys(L"user:alice", singles) == L"02", "scoped-user-includes-sampled-account-fallback");
  check(keys(L"status:\"not responding\"", singles) == L"2", "scoped-hung-process-status");
  check(keys(L"name:renderer suspended", singles) == L"1", "unqualified-scoped-term-matches-process-status");
  check(keys(L"-name:browser", singles) == L"12", "scoped-exclusion-only-query");
  check(keys(L"Orbit Browser", singles) == L"0" && keys(L"Browser Orbit", singles).empty(), "plain-phrase-remains-contiguous");
  check(keys(browser.path, singles) == L"0", "plain-search-includes-hidden-path-metadata");
  check(keys(L"LOCAL\\Guest", singles) == L"1", "plain-search-includes-hidden-account-metadata");
  Row heading; heading.kind = RowKind::Heading; heading.key = L"h";
  Row group = singles[0]; group.kind = RowKind::Group; group.key = L"g"; group.members = {0, 1}; group.cells.resize(15); group.cells[0] = {L"Orbit (2)"};
  Row child = singles[1]; child.key = L"c"; child.depth = 1;
  const std::vector<Row> grouped{heading, group, child, singles[2]};
  check(keys(L"pid:14242 name:renderer", grouped) == L"hgc", "scoped-child-match-retains-entire-group-context");
  check(keys(L"pid:4242 name:renderer", grouped).empty(), "scoped-group-cannot-combine-different-member-fields");
  check(keys(L"name:browser -pid:4242", grouped).empty(), "scoped-group-empty-pid-cannot-bypass-exclusion");
  check(keys(L"name:browser name:renderer", grouped).empty(), "scoped-all-positive-terms-need-one-member");
  group.process = -1;
  check(keys(L"pid:14242", {heading, group}) == L"hg", "scoped-collapsed-group-member-identity");
  selectedTab = UsersTab; Row session; session.kind = RowKind::User; session.key = L"u"; session.members = {0, 1}; session.cells = {{L"LOCAL\\Alice (2)"}, {L"1"}, {L"Console"}, {L"Disconnected"}};
  check(keys(L"user:alice status:disconnected", {session}) == L"u", "scoped-user-row-can-match-session-account-and-status");
  check(keys(L"user:alice pid:14242", {session}).empty(), "scoped-session-fields-cannot-combine-different-member-accounts");
  selectedTab = ServicesTab; Row service; service.kind = RowKind::Entry; service.key = L"s"; service.cells = {{L"Fixture service"}, {L"642", 642}, {L"Fixture description"}, {L"Running"}, {L"Fixture group"}};
  check(keys(L"name:fixture pid:642 status:running", {service}) == L"s" && keys(L"pid:64", {service}).empty(), "scoped-service-cell-fields-and-exact-pid");
  selectedTab = StartupTab; auto startup = std::make_shared<StartupInventory>(); Entry entry; entry.path = L"C:\\Startup Fixture\\launcher.exe"; startup->entries.push_back(entry); sample->startup = startup;
  Row startupRow; startupRow.kind = RowKind::Entry; startupRow.key = L"t"; startupRow.entry = 0; startupRow.cells = {{L"Launcher"}, {L"Orbit Labs"}, {L"Disabled"}, {L"Not measured"}, {L"Run"}, {L"launcher.exe --startup"}, {L"LOCAL\\Alice"}, {L"Fixture location"}};
  check(keys(L"name:launcher publisher:\"Orbit Labs\" status:disabled user:alice command:--startup path:\"Startup Fixture\"", {startupRow}) == L"t", "scoped-startup-search-covers-hidden-columns-and-path");
  check(current == sample, "search-does-not-install-or-request-a-new-snapshot");
  current = savedSample; selectedTab = savedTab; searchText = savedSearch;
  return passed;
}
bool Application::verifySorting() {
  verifyingSorting = true;
  const int savedTab = selectedTab; const bool savedGrouping = groupByType; const auto savedExpanded = expanded;
  const auto savedColumns = sortColumn; const auto savedDirections = ascending;
  bool passed = true;
  auto check = [&](bool result, const std::string& name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  const bool priorMeasured = measured; const unsigned priorTests = tests;
  benchmarkTick(); check(measured == priorMeasured && tests == priorTests, "benchmark-nested-tick-skipped");
  check(verifyHistoryPersistence(), "history-persistence");
  check(verifyTrayLifecycle(), "tray-lifecycle");
  check(verifyWorkerRecovery(), "sampler-worker-recovery");
  check(verifyBenchmarkSamplingHealth(), "benchmark-sampling-health");
  check(verifyServiceStatus(), "service-inventory-status");
  check(verifyViewportRefresh(), "viewport-refresh");
  check(verifySearch(), "scoped-search");
  check(verifyTableExport(), "table-export");
  {
    Row previous; previous.key = L"identity"; previous.cells = {{L"1.0%", 1.01, 0}};
    auto next = previous; next.cells[0].value = 1.04; next.process = 5;
    check(sameRowPaint(previous, next), "row-paint-ignores-unrendered-values-and-snapshot-indices");
    next.cells[0].text = L"1.1%"; check(!sameRowPaint(previous, next), "row-paint-detects-text");
    next = previous; next.cells[0].heat = 1; check(!sameRowPaint(previous, next), "row-paint-detects-heat");
    next = previous; ++next.icon; check(!sameRowPaint(previous, next), "row-paint-detects-icon");
    next = previous; next.expanded = true; check(!sameRowPaint(previous, next), "row-paint-detects-expansion");
    next = previous; next.key = L"replacement"; check(!sameRowPaint(previous, next), "row-paint-detects-identity");
  }
  {
    // Priority labels must follow their scheduler values in full and cached sorts.
    const auto savedSample = current; const int originalTab = selectedTab, originalColumn = sortColumn[DetailsTab];
    const bool originalDirection = ascending[DetailsTab], originalRebuilding = rebuilding; rebuilding = true;
    const int previousOrderedTab = orderedTab, previousOrderedColumn = orderedColumn; const bool previousOrderedAscending = orderedAscending;
    auto savedRows = std::move(rows); auto sample = std::make_shared<Sample>();
    for (const LONG priority : {8, 24, 1, 13, 6, 10, 0}) { Process process; process.id = DWORD(900501 + sample->processes.size() * 4); process.created = 1; process.name = L"Priority fixture"; process.priority = priority; sample->processes.push_back(std::move(process)); }
    current = sample; selectedTab = DetailsTab; sortColumn[DetailsTab] = 13;
    const std::array<double, 7> values{0, 1, 6, 8, 10, 13, 24};
    const std::array<std::wstring_view, 7> labels{L"N/A", L"Low", L"Below normal", L"Normal", L"Above normal", L"High", L"Realtime"};
    auto priorityOrder = [&](const std::vector<Row>& items, bool direction) {
      if (items.size() != values.size()) return false;
      for (size_t index = 0; index < items.size(); ++index) { const size_t expected = direction ? index : values.size() - index - 1; if (items[index].cells[13].value != values[expected] || items[index].cells[13].text != labels[expected]) return false; }
      return true;
    };
    for (const bool direction : {false, true}) {
      ascending[DetailsTab] = direction; std::vector<Row> fixture; buildDetails(fixture);
      check(priorityOrder(fixture, direction), "priority-full-sort-follows-scheduler-order");
      rows = fixture; std::reverse(rows.begin(), rows.end()); orderedTab = -1; sortVisibleRows();
      check(priorityOrder(rows, direction), "priority-cached-sort-follows-scheduler-order");
      orderedTab = DetailsTab; orderedColumn = 13; orderedAscending = direction; ascending[DetailsTab] = !direction; sortVisibleRows();
      check(priorityOrder(rows, !direction), "priority-cached-direction-reversal");
    }
    current = savedSample; selectedTab = originalTab; sortColumn[DetailsTab] = originalColumn; ascending[DetailsTab] = originalDirection; rows = std::move(savedRows); rebuilding = originalRebuilding;
    orderedTab = previousOrderedTab; orderedColumn = previousOrderedColumn; orderedAscending = previousOrderedAscending;
  }
  {
    const auto savedSample = current;
    auto inventory = std::make_shared<ServiceInventory>(); inventory->services.resize(3);
    inventory->services[0].pid = 42; inventory->services[0].description = L"Zulu";
    inventory->services[1].pid = 42; inventory->services[1].description = L"Alpha";
    inventory->services[2].pid = 0;
    current = std::make_shared<Sample>(); current->inventory = inventory; current->services = inventory->services;
    const auto& firstIndex = servicesByProcess();
    check(firstIndex.size() == 1 && firstIndex.at(42).size() == 2 && firstIndex.at(42).front()->description == L"Alpha", "service-index-order-and-zero-pid");
    const auto* retained = firstIndex.at(42).data(); servicesByProcess();
    check(indexedServices.at(42).data() == retained, "service-index-reuses-storage");
    auto replacement = std::make_shared<ServiceInventory>(); replacement->services.resize(1); replacement->services[0].pid = 43;
    current = std::make_shared<Sample>(); current->inventory = replacement; current->services = replacement->services;
    servicesByProcess(); check(indexedServices.size() == 1 && indexedServices.contains(43) && indexedInventory == replacement, "service-index-refresh");
    current = savedSample; indexedServices.clear(); indexedInventory.reset();
  }
  check(verifyActionSnapshots(), "action-target-snapshots");
  check(activity.size() == current->processes.size(), "activity-live-count");
  check(ImageList_GetImageCount(icons->images) >= 4, "stock-icons-present");
  HICON fallbackIcon = ImageList_GetIcon(icons->images, Icons::DefaultIcon, ILD_NORMAL);
  check(fallbackIcon != nullptr, "generic-process-icon-available");
  if (fallbackIcon) DestroyIcon(fallbackIcon);
  check(verifyPerformanceHover(), "performance-hover");
  check(verifyGraphPanels(), "performance-graph-panels");
  check(verifyTabSwitching(), "tab-switching");
  check(verifyHighContrast(), "high-contrast");
  check(verifyHeadingNavigation(), "heading-navigation");
  for (const auto& process : current->processes) { const auto found = activity.find({process.id, process.created}); check(found != activity.end() && found->second.ticks == process.cpuTicks && found->second.seen == activityGeneration && std::isfinite(found->second.trend), "activity-live-identity"); }
  {
    // Group labels follow the busiest engine; samples without engine detail retain the legacy fallback.
    const auto savedSample = current; const int originalTab = selectedTab; const bool originalRebuilding = rebuilding; rebuilding = true;
    auto sample = std::make_shared<Sample>(); sample->processes.resize(3);
    sample->processes[0].gpu = 30; sample->processes[0].gpuEngine = L"GPU 0 - 3D";
    sample->processes[1].gpu = 25; sample->processes[1].gpuEngine = L"GPU 0 - Copy";
    sample->processes[2].gpu = 40; sample->processes[2].gpuEngine = L"GPU 0 - Compute";
    current = sample; selectedTab = ProcessesTab;
    std::array<int, 3> members{0, 1, 2};
    do {
      Row row; row.kind = RowKind::Group; processCells(row, members);
      check(row.cells[12].text == L"GPU 0 - Compute" && row.cells[11].value == 95, "gpu-group-legacy-fallback-uses-busiest-member-label");
    } while (std::next_permutation(members.begin(), members.end()));
    const auto setEngines = [&](size_t index, std::initializer_list<GpuEngineUsage> engines) {
      auto usage = std::make_shared<ProcessGpu>(); usage->engines = engines;
      for (const auto& engine : engines) if (engine.value > usage->value) { usage->value = engine.value; usage->engine = engine.label; }
      auto& process = sample->processes[index]; process.gpu = std::min(100.0, usage->value); process.gpuEngine = usage->engine; process.gpuUsage = std::move(usage);
    };
    const std::wstring first = L"luid_1|_phys_0_eng_0|3D", second = L"luid_1|_phys_0_eng_1|Copy", third = L"luid_1|_phys_0_eng_2|Compute";
    setEngines(0, {{first, L"GPU 0 - 3D", 30}}); setEngines(1, {{second, L"GPU 0 - Copy", 25}}); setEngines(2, {{third, L"GPU 0 - Compute", 40}});
    do {
      Row row; row.kind = RowKind::Group; processCells(row, members);
      check(row.cells[11].value == 40 && row.cells[12].text == L"GPU 0 - Compute", "gpu-group-uses-busiest-actual-engine");
    } while (std::next_permutation(members.begin(), members.end()));
    sample->processes[1].gpuUsage.reset();
    Row mixed; processCells(mixed, members); check(mixed.cells[11].value == 95 && mixed.cells[12].text == L"GPU 0 - Compute", "gpu-group-mixed-samples-use-whole-row-fallback");
    sample->processes[1].gpu = 0;
    Row inactive; processCells(inactive, members); check(inactive.cells[11].value == 40 && inactive.cells[12].text == L"GPU 0 - Compute", "gpu-group-idle-member-needs-no-engine-detail");
    setEngines(0, {{first, L"GPU 0 - 3D", 25}, {second, L"GPU 0 - Copy", 45}}); setEngines(1, {{first, L"GPU 0 - 3D", 35}}); setEngines(2, {});
    Row shared; processCells(shared, members); check(shared.cells[11].value == 60 && shared.cells[12].text == L"GPU 0 - 3D", "gpu-group-sums-shared-engine-including-nonpeak-contributions");
    Row single; processCells(single, std::span<const int>(members).first(1)); check(single.cells[11].value == 45 && single.cells[12].text == L"GPU 0 - Copy", "gpu-single-process-retains-summary");
    setEngines(0, {{first, L"GPU 0 - 3D", 70}}); setEngines(1, {{first, L"GPU 0 - 3D", 55}});
    Row capped; processCells(capped, members); check(capped.cells[11].value == 100 && capped.cells[12].text == L"GPU 0 - 3D", "gpu-group-shared-engine-capped-at-one-hundred");
    setEngines(0, {{first, L"GPU 0 - 3D", 30}}); setEngines(1, {{L"luid_2|_phys_0_eng_0|3D", L"GPU 1 - 3D", 40}});
    Row adapters; processCells(adapters, members); check(adapters.cells[11].value == 40 && adapters.cells[12].text == L"GPU 1 - 3D", "gpu-group-distinguishes-logical-adapters");
    setEngines(1, {{L"luid_1|_phys_1_eng_0|3D", L"GPU 0 - 3D", 40}});
    Row physical; processCells(physical, members); check(physical.cells[11].value == 40 && physical.cells[12].text == L"GPU 0 - 3D", "gpu-group-distinguishes-linked-physical-adapters");
    setEngines(1, {{L"luid_1|_phys_0_eng_1|3D", L"GPU 0 - 3D", 40}});
    Row independent; processCells(independent, members); check(independent.cells[11].value == 40, "gpu-group-distinguishes-engines-with-the-same-type");
    for (size_t index = 0; index < sample->processes.size(); ++index) setEngines(index, {});
    Row idle; processCells(idle, members); check(idle.cells[12].text.empty() && idle.cells[11].value == 0, "gpu-idle-group-clears-engine-label");
    current = savedSample; selectedTab = originalTab; rebuilding = originalRebuilding;
  }
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
        if (tab == ProcessesTab) {
          const bool headings = std::any_of(rows.begin(), rows.end(), [](const Row& row) { return row.kind == RowKind::Heading; });
          check(headings == (grouped && column == 0), "process-headings-only-name-sort");
          for (const auto& row : rows) if (row.depth == 0 && row.category == 0) check(std::any_of(row.members.begin(), row.members.end(), [&](int member) { return current->processes[size_t(member)].app; }), "app-group-requires-visible-window");
        }
        check(rowKey(selectedIndex()) == selected, "selection");
        std::vector<std::wstring> actual; actual.reserve(rows.size()); for (const auto& row : rows) actual.push_back(row.key);
        rebuild();
        check(rows.size() == actual.size(), "row-count");
        for (size_t index = 0; index < std::min(rows.size(), actual.size()); ++index) check(rows[index].key == actual[index], "order-tab-" + std::to_string(tab) + "-column-" + std::to_string(column));
      }
    }
  }
  {
    // Anonymous packaged and desktop applications in separate logon sessions.
    const auto savedSample = current; const int originalTab = selectedTab, originalSort = sortColumn[ProcessesTab];
    const bool originalCompact = compact, originalGrouping = groupByType, originalDirection = ascending[ProcessesTab];
    auto originalExpanded = std::move(expanded);
    auto sample = std::make_shared<Sample>(); sample->processes.resize(7);
    for (size_t index = 0; index < sample->processes.size(); ++index) {
      auto& process = sample->processes[index]; process.id = DWORD(900101 + index * 4); process.created = 100 + index; process.name = L"Fixture.exe";
      process.session = index < 2 ? 11 : index < 4 ? 22 : index == 4 ? 33 : 44; process.app = index == 0 || index == 2 || index == 5;
      if (index < 4) { Metadata metadata; metadata.package = L"Anonymous.Package"; icons->setTestMetadata(process, std::move(metadata)); }
    }
    sample->processes[1].parent = sample->processes[0].id; sample->processes[3].parent = sample->processes[2].id;
    sample->processes[4].parent = sample->processes[0].id; sample->processes[6].parent = sample->processes[5].id;
    current = sample; selectedTab = ProcessesTab; compact = false; sortColumn[ProcessesTab] = 0; ascending[ProcessesTab] = true; expanded.clear();
    for (const bool grouped : {false, true}) {
      groupByType = grouped; std::vector<Row> fixture; buildProcesses(fixture);
      unsigned roots = 0;
      for (const auto& row : fixture) if (row.depth == 0 && (row.kind == RowKind::Process || row.kind == RowKind::Group)) {
        ++roots; const DWORD session = sample->processes[size_t(row.process)].session;
        check(std::all_of(row.members.begin(), row.members.end(), [&](int member) { return sample->processes[size_t(member)].session == session; }), "process-group-stays-in-one-session");
        const std::vector<int> expected = session == 11 ? std::vector<int>{0, 1} : session == 22 ? std::vector<int>{2, 3} : session == 33 ? std::vector<int>{4} : std::vector<int>{5, 6};
        check(row.members == expected, "process-group-keeps-same-session-members");
        if (row.expandable) expanded.insert(row.key);
      }
      check(roots == 4, "process-groups-separate-package-and-parent-sessions");
      fixture.clear(); buildProcesses(fixture);
      check(std::count_if(fixture.begin(), fixture.end(), [](const Row& row) { return row.depth == 1 && row.kind == RowKind::Process; }) == 6, "process-group-expansion-keeps-members");
      expanded.clear();
    }
    Process highParent; highParent.id = 0xfffffffcu; highParent.created = 100; highParent.name = L"High PID fixture.exe"; highParent.app = true;
    Process highChild = highParent; highChild.id = 123456; highChild.parent = highParent.id; highChild.created = 101; highChild.app = false;
    sample->processes = {highChild, highParent};
    std::vector<Row> highRows; buildProcesses(highRows);
    const auto highGroup = std::find_if(highRows.begin(), highRows.end(), [](const Row& row) { return row.kind == RowKind::Group; });
    check(highGroup != highRows.end() && highGroup->process == 1 && highGroup->key == L"g:4294967292:100" && highGroup->members == std::vector<int>{0, 1}, "process-group-preserves-unsigned-pid-root-with-child-first");
    current = savedSample; selectedTab = originalTab; compact = originalCompact; groupByType = originalGrouping; sortColumn[ProcessesTab] = originalSort; ascending[ProcessesTab] = originalDirection; expanded = std::move(originalExpanded);
    indexedServices.clear(); indexedInventory.reset(); icons->prune(activity);
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
    check(!ListView_SetColumnOrderArray(list, columnCount, reversedOrder.data()), "column-order-keeps-name-first");
    check(!Header_SetOrderArray(header, columnCount, reversedOrder.data()), "header-order-keeps-name-first");
    HDITEMW moveName{}; moveName.mask = HDI_ORDER; moveName.iOrder = columnCount - 1;
    check(!Header_SetItem(header, 0, &moveName), "column-name-cannot-move-away");
    moveName.iOrder = 0; check(!Header_SetItem(header, 1, &moveName), "column-cannot-move-before-name");
    NMHEADERW drag{}; drag.hdr = {header, 0, HDN_BEGINDRAG}; drag.iItem = 0;
    check(SendMessageW(list, WM_NOTIFY, 0, LPARAM(&drag)) != 0, "column-name-drag-rejected");
    drag.hdr.code = HDN_ENDDRAG; drag.iItem = 1; drag.pitem = &moveName;
    check(SendMessageW(list, WM_NOTIFY, 0, LPARAM(&drag)) != 0, "column-drop-before-name-rejected");
    reversedOrder[0] = 0;
    for (int position = 1; position < columnCount; ++position) reversedOrder[size_t(position)] = originalOrder[size_t(columnCount - position)];
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
      check(actualWidth == std::max(minimum, requested), "column-native-drag-minimum-and-live-width: requested=" + std::to_string(requested) + ", minimum=" + std::to_string(minimum) + ", actual=" + std::to_string(actualWidth));
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
    selectTab(DetailsTab); captureColumns(); columns(true);
    const UINT originalDpi = dpi; RECT originalBounds{}; GetWindowRect(window, &originalBounds);
    const auto originalShown = shown[DetailsTab], originalWidths = widths[DetailsTab];
    const int originalSelection = selectedIndex(), originalTop = ListView_GetTopIndex(list);
    const int originalSort = sortColumn[DetailsTab]; const bool originalDirection = ascending[DetailsTab];
    sortColumn[DetailsTab] = 0; ascending[DetailsTab] = true; rebuild();
    const int count = Header_GetItemCount(header); std::vector<int> order(size_t(count), 0), expectedIds, logicalWidths;
    std::iota(order.begin(), order.end(), 0); if (count > 2) std::swap(order[1], order[2]);
    for (int index = 0; index < count; ++index) { const int width = minimumColumnWidth(DetailsTab, columnAt(index)) + 80 + index * 8; logicalWidths.push_back(width); ListView_SetColumnWidth(list, index, scale(width)); }
    for (const int index : order) expectedIds.push_back(originalShown[size_t(index)]);
    check(ListView_SetColumnOrderArray(list, count, order.data()), "dpi-fixture-native-column-order");
    if (rows.size() > 5) select(5);
    SendMessageW(list, WM_VSCROLL, SB_TOP, 0); ListView_Scroll(list, 0, rowHeight() * 2);
    const auto selected = rowKey(selectedIndex()), top = rowKey(ListView_GetTopIndex(list));
    for (const UINT targetDpi : {originalDpi * 2, originalDpi}) {
      SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(targetDpi, targetDpi), LPARAM(&originalBounds));
      check(dpi == targetDpi && shown[DetailsTab] == expectedIds, "dpi-change-preserves-column-order");
      for (int position = 0; position < count; ++position) check(ListView_GetColumnWidth(list, position) == scale(logicalWidths[size_t(order[size_t(position)])]), "dpi-change-scales-live-column-widths");
      int spacerWidth = 0, spacerHeight = 0;
      check(ImageList_GetIconSize(rowSpacer, &spacerWidth, &spacerHeight) && spacerHeight == rowHeight() - 1, "dpi-change-updates-row-height");
      check(rowKey(selectedIndex()) == selected && rowKey(ListView_GetTopIndex(list)) == top, "dpi-change-preserves-selection-and-scroll");
    }
    shown[DetailsTab] = originalShown; widths[DetailsTab] = originalWidths; sortColumn[DetailsTab] = originalSort; ascending[DetailsTab] = originalDirection; columns(true); rebuild();
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); if (originalSelection >= 0) select(originalSelection);
    ListView_Scroll(list, 0, (originalTop - ListView_GetTopIndex(list)) * rowHeight());
    HDC screen = GetDC(list), canvas = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = canvas ? CreateCompatibleBitmap(screen, 64, 40) : nullptr;
    const auto previousBitmap = bitmap ? SelectObject(canvas, bitmap) : nullptr;
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = 16; info.bmiHeader.biHeight = -16; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr; HBITMAP source = screen ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
    HIMAGELIST fixtureImages = ImageList_Create(16, 16, ILC_COLOR32, 1, 1);
    check(bitmap && source && pixels && fixtureImages, "dpi-icon-fixture-resources");
    if (bitmap && source && pixels && fixtureImages) {
      std::fill_n(static_cast<DWORD*>(pixels), 256, DWORD(0xff336699));
      check(ImageList_Add(fixtureImages, source, nullptr) == 0, "dpi-icon-fixture-image");
      const auto originalImages = std::exchange(icons->images, fixtureImages);
      for (const int size : {8, 16, 32}) for (const int index : {0, 9999}) {
        PatBlt(canvas, 0, 0, 64, 40, WHITENESS);
        check(icons->draw(canvas, index, 1, 1, size), "dpi-icon-draw-and-fallback");
        check(GetPixel(canvas, size - 1, size - 1) == RGB(51, 102, 153) && GetPixel(canvas, size + 1, size + 1) == RGB(255, 255, 255), "dpi-icon-fills-only-requested-size");
      }
      icons->images = originalImages;
      if (!options.output.empty()) {
        PatBlt(canvas, 0, 0, 64, 40, WHITENESS); icons->draw(canvas, Icons::DefaultIcon, 4, 12, 16); icons->draw(canvas, Icons::DefaultIcon, 28, 4, 32);
        const std::filesystem::path output(options.output), capturePath = output.parent_path() / (output.stem().wstring() + L"-dpi-icons-16px-32px.png");
        std::error_code error; if (!capturePath.parent_path().empty()) std::filesystem::create_directories(capturePath.parent_path(), error);
        SelectObject(canvas, previousBitmap);
        { Gdiplus::Bitmap capture(bitmap, nullptr); const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}}; check(capture.Save(capturePath.c_str(), &png, nullptr) == Gdiplus::Ok, "dpi-icon-review-capture"); }
        SelectObject(canvas, bitmap);
      }
    }
    if (fixtureImages) ImageList_Destroy(fixtureImages); if (source) DeleteObject(source);
    if (bitmap) { SelectObject(canvas, previousBitmap); DeleteObject(bitmap); } if (canvas) DeleteDC(canvas); if (screen) ReleaseDC(list, screen);
  }
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
    const HWND previousFocus = GetFocus(); searchVisible = true; layout(); SetFocus(searchBox);
    const auto exactPid = L"pid:" + std::to_wstring(GetCurrentProcessId()); SetWindowTextW(searchBox, exactPid.c_str());
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
    MSG enterKey{}; enterKey.hwnd = searchBox; enterKey.message = WM_KEYDOWN; enterKey.wParam = VK_RETURN;
    check((SendMessageW(searchBox, WM_GETDLGCODE, VK_RETURN, LPARAM(&enterKey)) & DLGC_WANTMESSAGE) != 0, "search-enter-is-not-a-dialog-default-action");
    SendMessageW(searchBox, WM_KEYDOWN, VK_RETURN, 1);
    check(!searchQueued && searchText == exactPid && rows.size() == 1 && selectedIndex() == 0 && GetFocus() == list, "search-enter-applies-pending-query-and-selects-result");
    check(current == snapshot && fullRebuilds == builds, "search-enter-uses-cached-snapshot");
    searchVisible = savedSearchVisible; layout(); SetFocus(previousFocus);
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
  sortColumn = savedColumns; ascending = savedDirections; groupByType = savedGrouping; expanded = savedExpanded; selectTab(ProcessesTab); rebuild();
  ValidateRect(list, nullptr); rebuild();
  check(!GetUpdateRect(list, nullptr, FALSE), "unchanged-process-snapshot-no-table-repaint");
  selectTab(savedTab); rebuild();
  verifyingSorting = false;
  return passed;
}
bool Application::verifyHeadingNavigation() {
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  std::array<BYTE, 256> savedKeys{};
  if (!GetKeyboardState(savedKeys.data())) { check(false, "heading-keyboard-state-readable"); return false; }
  struct KeyboardRestore { std::array<BYTE, 256> keys; ~KeyboardRestore() { SetKeyboardState(keys.data()); } } keyboardRestore{savedKeys};
  auto keys = savedKeys;
  for (const int key : {VK_CONTROL, VK_LCONTROL, VK_RCONTROL, VK_SHIFT, VK_LSHIFT, VK_RSHIFT, VK_MENU, VK_LMENU, VK_RMENU}) keys[size_t(key)] = 0;
  // SetKeyboardState affects this diagnostic thread only; no input is sent to other windows.
  if (!SetKeyboardState(keys.data())) { check(false, "heading-keyboard-state-isolated"); return false; }
  const HWND savedFocus = GetFocus();
  const HWND fixture = CreateWindowExW(0, WC_LISTVIEWW, L"Heading navigation fixture", WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL,
    0, 0, scale(640), scale(360), window, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!fixture) { check(false, "heading-list-fixture-created"); return false; }
  const HWND savedList = list, savedHeader = header; const bool savedRebuilding = rebuilding;
  auto savedRows = std::move(rows);
  struct Restore { std::function<void()> run; ~Restore() { run(); } } restore{[&] {
    RemoveWindowSubclass(fixture, listProcedure, 1);
    list = savedList; header = savedHeader; rows = std::move(savedRows); rebuilding = savedRebuilding; columnGeometryDirty = true;
    DestroyWindow(fixture); if (GetFocus() != savedFocus) SetFocus(savedFocus); updateFooter(); InvalidateRect(list, nullptr, FALSE); InvalidateRect(header, nullptr, FALSE);
  }};
  list = fixture; header = ListView_GetHeader(fixture); rebuilding = true; columnGeometryDirty = true;
  LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(L"Fixture"); column.cx = scale(400);
  ListView_InsertColumn(fixture, 0, &column);
  const auto populate = [&](size_t count, bool headings) {
    rows.assign(count, Row{});
    for (size_t index = 0; index < rows.size(); ++index) { rows[index].key = L"heading-fixture:" + std::to_wstring(index); rows[index].cells.resize(24); rows[index].cells[0].text = L"Anonymous row " + std::to_wstring(index); }
    if (headings) for (const size_t index : {size_t(0), size_t(2), size_t(3), size_t(6)}) rows[index].kind = RowKind::Heading;
    ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOINVALIDATEALL);
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); SendMessageW(list, WM_VSCROLL, SB_TOP, 0);
  };
  const auto press = [&](int from, WPARAM key) { select(from); SendMessageW(list, WM_KEYDOWN, key, 1); };
  populate(7, true); SetFocus(list);
  check(GetFocus() == list, "heading-fixture-has-thread-focus");
  // Before installing our subclass, reproduce the old native key fall-through.
  press(1, VK_HOME);
  check(selectedIndex() == 0 && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 0, "heading-native-home-selects-heading");
  auto controlKeys = keys; controlKeys[VK_CONTROL] = controlKeys[VK_LCONTROL] = 0x80;
  check(SetKeyboardState(controlKeys.data()) != FALSE, "heading-control-state-isolated");
  press(1, VK_DOWN); const int nativeControlSelection = selectedIndex(), nativeControlFocus = ListView_GetNextItem(list, -1, LVNI_FOCUSED);
  SetKeyboardState(keys.data());
  populate(120, false); press(1, VK_NEXT);
  const int nativePageSelection = selectedIndex(), nativePageTop = ListView_GetTopIndex(list);
  check(nativePageSelection > 1 && nativePageSelection + 1 < int(rows.size()), "heading-native-page-fixture-range");
  if (!SetWindowSubclass(fixture, listProcedure, 1, reinterpret_cast<DWORD_PTR>(this))) { check(false, "heading-list-subclass-installed"); return false; }
  populate(7, true);
  press(5, VK_HOME); check(selectedIndex() == 1, "heading-home-skips-first-heading");
  press(1, VK_UP); check(selectedIndex() == 1, "heading-up-stops-at-first-process");
  press(1, VK_DOWN); check(selectedIndex() == 4, "heading-down-skips-consecutive-headings");
  press(4, VK_UP); check(selectedIndex() == 1, "heading-up-skips-consecutive-headings");
  press(1, VK_END); check(selectedIndex() == 5, "heading-end-skips-last-heading");
  press(1, VK_NEXT); check(selectedIndex() == 5, "heading-page-down-skips-last-heading");
  press(5, VK_PRIOR); check(selectedIndex() == 1, "heading-page-up-skips-first-heading");
  press(4, VK_DOWN); check(selectedIndex() == 5, "heading-navigation-keeps-ordinary-rows");
  SetKeyboardState(controlKeys.data()); press(1, VK_DOWN);
  check(selectedIndex() == (nativeControlSelection == 2 ? 4 : nativeControlSelection) && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == (nativeControlFocus == 2 ? 4 : nativeControlFocus), "heading-control-navigation-keeps-native-selection");
  SetKeyboardState(keys.data());
  select(1); ListView_SetItemState(list, 1, 0, LVIS_FOCUSED); ListView_SetItemState(list, 2, LVIS_FOCUSED, LVIS_FOCUSED); skipHeading(1);
  check(selectedIndex() == 1 && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 4, "heading-focus-only-correction-preserves-selected-process");
  populate(120, false); press(1, VK_NEXT);
  check(selectedIndex() == nativePageSelection && ListView_GetTopIndex(list) == nativePageTop, "heading-navigation-preserves-native-paging");
  if (nativePageSelection > 1 && nativePageSelection + 1 < int(rows.size())) {
    populate(120, false); rows[size_t(nativePageSelection)].kind = RowKind::Heading; press(1, VK_NEXT);
    check(selectedIndex() == nativePageSelection + 1, "heading-native-page-destination-skips-heading");
  }
  {
    populate(7, false); select(1); ListView_SetItemState(list, 1, 0, LVIS_FOCUSED); ListView_SetItemState(list, 2, LVIS_FOCUSED, LVIS_FOCUSED);
    const bool savedDark = dark, savedContrast = highContrast;
    const RECT area{0, 0, scale(400), rowHeight()};
    HDC screen = GetDC(list), memory = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, area.right, area.bottom) : nullptr;
    check(screen && memory && bitmap, "keyboard-focus-render-buffer");
    if (bitmap) {
      const auto previous = SelectObject(memory, bitmap);
      const auto pixels = [&] { std::vector<COLORREF> result; for (int x = scale(40); x < scale(300); ++x) result.push_back(GetPixel(memory, x, 1)); return result; };
      for (const int appearance : {0, 1, 2}) {
        dark = appearance == 1; highContrast = appearance == 2;
        SendMessageW(list, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0); drawRow(memory, 2, area); const auto hiddenFocus = pixels();
        SendMessageW(list, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0); drawRow(memory, 2, area);
        check(pixels() != hiddenFocus, "keyboard-focus-visible-in-light-dark-and-high-contrast");
        SendMessageW(list, WM_UPDATEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0); drawRow(memory, 2, area);
        check(pixels() == hiddenFocus, "keyboard-focus-respects-hidden-focus-cues");
        SendMessageW(list, WM_UPDATEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0); SetFocus(tabs); drawRow(memory, 2, area);
        check(pixels() == hiddenFocus, "keyboard-focus-hidden-when-list-loses-thread-focus"); SetFocus(list);
      }
      SelectObject(memory, previous); DeleteObject(bitmap);
    }
    if (memory) DeleteDC(memory); if (screen) ReleaseDC(list, screen);
    dark = savedDark; highContrast = savedContrast;
    check(selectedIndex() == 1 && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == 2, "keyboard-focus-painting-keeps-native-selection-and-focus");
  }
  populate(7, true); for (auto& row : rows) row.kind = RowKind::Heading;
  ListView_SetItemState(list, 3, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); skipHeading(1);
  check(selectedIndex() == -1 && ListView_GetNextItem(list, -1, LVNI_FOCUSED) == -1, "heading-only-list-has-no-action-selection");
  return passed;
}
bool Application::verifyTabSwitching() {
  const int savedTab = selectedTab, savedTheme = theme;
  const HWND savedFocus = GetFocus();
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
  // Exercise only this thread's owned diagnostic controls; do not request foreground activation.
  selectTab(PerformanceTab); SetFocus(performance);
  check(GetFocus() == performance, "tab-focus-fixture-owned-performance-child");
  selectTab(DetailsTab);
  check(GetFocus() == list, "tab-leaving-performance-focuses-visible-table");
  SetFocus(tabs); selectTab(ProcessesTab);
  check(GetFocus() == tabs, "tab-navigation-keeps-tab-strip-focus");
  theme = savedTheme; applyTheme(); selectTab(savedTab); SetFocus(savedFocus);
  return passed;
}
bool Application::verifyHighContrast() {
  const int savedTab = selectedTab, savedHeader = hotHeader, savedResourceHover = hotResource, savedScroll = sidebarScroll;
  const bool savedContrast = highContrast, savedDark = dark, savedGraphs = hideGraphs;
  const auto savedResource = selectedResource; const auto snapshot = current;
  const auto savedBrush = backgroundBrush;
  bool passed = true;
  auto check = [&](bool result, const char* name) { if (!result && sortingFailure.empty()) sortingFailure = name; passed = passed && result; };
  selectTab(ProcessesTab);
  // Exercise High Contrast without changing the desktop's accessibility settings.
  highContrast = true; dark = false; backgroundBrush = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
  RECT headerBounds{}, tabBounds{}; GetClientRect(header, &headerBounds); GetClientRect(tabs, &tabBounds);
  const int width = std::max({int(headerBounds.right), int(tabBounds.right), scale(222)});
  const int height = std::max({int(headerBounds.bottom), int(tabBounds.bottom), scale(210)});
  HDC screen = GetDC(window), memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
  check(screen && memory && bitmap && backgroundBrush, "high-contrast-render-buffer");
  if (screen && memory && bitmap && backgroundBrush) {
    const auto previous = SelectObject(memory, bitmap); SetBkMode(memory, TRANSPARENT);
    const RECT canvas{0, 0, width, height}; FillRect(memory, &canvas, backgroundBrush);
    auto capture = [&](RECT bounds, const wchar_t* name) {
      if (options.output.empty()) return;
      const std::filesystem::path output(options.output);
      const auto path = output.parent_path() / (output.stem().wstring() + L"-high-contrast-" + name + L".png");
      std::error_code error; if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path(), error);
      if (error) { check(false, "high-contrast-capture-directory"); return; }
      GdiFlush(); SelectObject(memory, previous);
      {
        Gdiplus::Bitmap image(bitmap, nullptr); const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        std::unique_ptr<Gdiplus::Bitmap> cropped(image.Clone(Gdiplus::Rect(bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top), PixelFormat32bppRGB));
        check(cropped && cropped->Save(path.c_str(), &png, nullptr) == Gdiplus::Ok, "high-contrast-capture-save");
      }
      SelectObject(memory, bitmap);
    };
    current = std::make_shared<Sample>(); current->cpu = 95;
    const auto cpu = std::find(shown[ProcessesTab].begin(), shown[ProcessesTab].end(), 7);
    check(cpu != shown[ProcessesTab].end(), "high-contrast-cpu-column");
    if (cpu != shown[ProcessesTab].end()) {
      const int column = int(cpu - shown[ProcessesTab].begin()); RECT box{}; Header_GetItemRect(header, column, &box);
      for (const bool hovered : {false, true}) {
        const int saved = SaveDC(memory); IntersectClipRect(memory, box.left, box.top, box.right, box.bottom);
        hotHeader = hovered ? column : -1; paintHeader(memory, headerBounds);
        check(GetPixel(memory, box.left + 1, (box.top + box.bottom) / 2) == GetSysColor(hovered ? COLOR_HIGHLIGHT : COLOR_WINDOW), "high-contrast-busy-header-background");
        check(GetTextColor(memory) == GetSysColor(hovered ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT), "high-contrast-header-text-pair");
        RestoreDC(memory, saved);
        capture(box, hovered ? L"header-hover" : L"header-normal");
      }
    }
    current = snapshot;
    // Native selection can move before a coalesced tab switch is committed.
    for (const int selection : {ProcessesTab, ServicesTab}) {
      TabCtrl_SetCurSel(tabs, selection); SendMessageW(tabs, WM_PRINTCLIENT, WPARAM(memory), PRF_CLIENT);
      for (int index = 0; index < TabCount; ++index) {
        RECT box{}; TabCtrl_GetItemRect(tabs, index, &box);
        check(GetPixel(memory, box.left + 1, box.top + 1) == GetSysColor(index == selection ? COLOR_HIGHLIGHT : COLOR_WINDOW), "high-contrast-tab-background");
      }
      check(GetTextColor(memory) == GetSysColor(selection == ServicesTab ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT), "high-contrast-tab-text-pair");
      RECT last{}; TabCtrl_GetItemRect(tabs, TabCount - 1, &last);
      capture({0, 0, std::min(tabBounds.right, last.right + scale(2)), tabBounds.bottom}, selection == ServicesTab ? L"tabs-services" : L"tabs-processes");
    }
    TabCtrl_SetCurSel(tabs, selectedTab);
    const auto savedSidebarFont = sidebarFont, savedLabelFont = labelFont;
    // A stock bitmap font makes these bounded text-pixel checks independent of ClearType.
    sidebarFont = labelFont = HFONT(GetStockObject(SYSTEM_FIXED_FONT));
    hideGraphs = true; sidebarScroll = 0; selectedResource = L"cpu"; hotResource = 1;
    const std::vector<PerfItem> fixtures{{L"cpu", L"CPU", L"", L"95%", L"", CpuColor}, {L"memory", L"Memory", L"Usage", L"30%", L"", MemoryColor}, {L"gpu", L"GPU", L"", L"10%", L"", GpuColor}};
    const RECT sidebar{0, 0, scale(222), scale(210)}; FillRect(memory, &sidebar, backgroundBrush); paintSidebar(memory, sidebar, fixtures);
    auto containsColor = [&](RECT bounds, COLORREF color) {
      for (LONG y = bounds.top; y < bounds.bottom; ++y) for (LONG x = bounds.left; x < bounds.right; ++x) if (GetPixel(memory, x, y) == color) return true;
      return false;
    };
    for (int index = 0; index < int(fixtures.size()); ++index) {
      const int top = scale(12) + index * scale(60); const bool highlighted = index < 2;
      const COLORREF foreground = GetSysColor(highlighted ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT);
      check(GetPixel(memory, scale(3), top + 1) == GetSysColor(highlighted ? COLOR_HIGHLIGHT : COLOR_WINDOW), "high-contrast-sidebar-background");
      check(containsColor({scale(16), top + scale(6), scale(150), top + scale(28)}, foreground), "high-contrast-sidebar-title-text");
      check(containsColor({scale(16), top + scale(27), scale(150), top + scale(57)}, foreground), "high-contrast-sidebar-secondary-text");
    }
    sidebarFont = savedSidebarFont; labelFont = savedLabelFont;
    FillRect(memory, &sidebar, backgroundBrush); paintSidebar(memory, sidebar, fixtures); capture(sidebar, L"sidebar");
    SelectObject(memory, previous);
  }
  if (bitmap) DeleteObject(bitmap); if (memory) DeleteDC(memory); if (screen) ReleaseDC(window, screen);
  if (backgroundBrush) DeleteObject(backgroundBrush); backgroundBrush = savedBrush;
  highContrast = savedContrast; dark = savedDark; hideGraphs = savedGraphs; hotResource = savedResourceHover; sidebarScroll = savedScroll; selectedResource = savedResource; current = snapshot;
  selectTab(savedTab); hotHeader = savedHeader;
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
  {
    auto fixture = std::make_shared<Sample>(*snapshot);
    fixture->disks.clear(); fixture->networks.clear(); fixture->gpus.clear();
    DiskInfo disk; disk.key = L"removable"; disk.title = L"Removable test disk"; fixture->disks.push_back(disk);
    NetworkInfo network; network.luid = 123; network.type = L"Test network"; fixture->networks.push_back(network);
    current = fixture; performanceItemsDirty = true; selectedResource = L"disk/removable";
    check(performanceSummary().starts_with(L"Removable test disk\r\n"), "resource-copy-selected-device");
    current = std::make_shared<Sample>(*fixture); current->disks.clear(); performanceItemsDirty = true;
    const auto labelBuilds = performanceItemBuilds;
    const auto copied = performanceSummary();
    check(copied.starts_with(L"CPU\r\n") && copied.find(L"\tProcesses:\t") != std::wstring::npos && copied.find(L"\tUp time:\t") != std::wstring::npos, "removed-resource-copy-resolves-before-paint");
    performanceKey(VK_DOWN);
    check(selectedResource == L"memory", "removed-resource-down-resolves-before-paint");
    selectedResource = L"disk/removable"; performanceKey(VK_UP);
    check(selectedResource == L"cpu", "removed-resource-up-resolves-before-paint");
    selectedResource = L"disk/removable"; performanceKey(VK_END);
    check(selectedResource == L"net/123" && performanceSummary().starts_with(L"Test network\r\n"), "removed-resource-end-selects-last");
    performanceKey(VK_HOME); check(selectedResource == L"cpu", "resource-home-selects-first");
    selectedResource = L"disk/removable"; sidebarScroll = scale(820);
    RedrawWindow(performance, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    check(selectedResource == L"cpu" && sidebarScroll == 0, "removed-resource-paint-reveals-fallback");
    check(performanceItemBuilds == labelBuilds + 1, "resource-fallback-reuses-cached-labels");
    current.reset(); performanceItemsDirty = true; selectedResource = L"disk/removable";
    performanceKey(VK_DOWN);
    check(performanceSummary().empty() && selectedResource == L"disk/removable", "resource-no-sample-is-inert");
    current = snapshot; performanceItemsDirty = true; selectedResource = L"cpu";
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
    if (options.tab == StartupTab && (!current->startup->complete || current->startup->entries.empty())) {
      selectTab(StartupTab);
      if (Clock::now() - started > std::chrono::seconds(30)) { sortingFailure = "startup-inventory-timeout"; writeBenchmark(); SendMessageW(window, WM_CLOSE, 0, 0); }
      return;
    }
    sortingChecked = true; sortingPassed = verifySorting();
  }
  if (keyboardPassed && options.tab >= 0 && !options.tabSpam) selectTab(options.tab);
  if (keyboardPassed && options.minimized && !IsIconic(window)) ShowWindow(window, SW_SHOWMINNOACTIVE);
  if (!measured && keyboardPassed && (!options.minimized || interval == 0 || (minimized && current && current->elapsed >= 3.5)) && std::chrono::duration<double>(Clock::now() - started).count() >= options.warmup) {
    beginSamplingMeasurement(); LARGE_INTEGER counter{}; QueryPerformanceCounter(&counter); measurementCounter = counter.QuadPart; FILETIME created{}, exited{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &cpuStartKernel, &cpuStartUser); GetProcessHandleCount(GetCurrentProcess(), &baselineHandles);
    baselineGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS); baselineUser = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    deliveryTimes.clear(); updateTimes.clear(); paintTimes.clear(); samplerTimes.clear(); inputTimes.clear(); sortTimes.clear(); tests = sortRequests = fullRebuilds = sortPasses = 0; for (auto& times : stageTimes) times.clear();
    sampleCadenceTimes.clear(); partialTableUpdates = invalidatedRows = resourceUpdates = 0;
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
void Application::beginSamplingMeasurement() {
  // Publish the boundary and capture failure state together with the worker's
  // completed iteration, so recovered warmup failures do not taint measurement.
  std::lock_guard lock(workerMutex);
  samplingFailureBaseline = samplingFailures.load(); samplingHealthyAtStart = !samplingFailed;
  measured = true; measurementStarted = Clock::now();
}
bool Application::samplingMeasurementHealthy() {
  std::lock_guard lock(workerMutex);
  return measured && samplingHealthyAtStart && !samplingFailed && samplingFailures == samplingFailureBaseline;
}
void Application::writeBenchmark() {
  const bool coalescingPassed = !options.sortSpam || (sortRequests >= 128 && !sortTimes.empty() && sortTimes.size() * 16 < sortRequests);
  const bool tabSpamPassed = !options.tabSpam || (tabRequests >= 128 && tabCommits > 0 && tabCommits * 16 < tabRequests);
  const bool samplingHealthy = samplingMeasurementHealthy();
  exitCode = updates > 0 && keyboardPassed && sortingPassed && coalescingPassed && tabSpamPassed && samplingHealthy ? 0 : 1;
  if (options.output.empty()) return;
  Report stream{options.output}; FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
  PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)}; GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)); DWORD handles = 0; GetProcessHandleCount(GetCurrentProcess(), &handles);
  const double elapsed = std::max(.001, std::chrono::duration<double>(Clock::now() - (measured ? measurementStarted : started)).count()); const double cpu = double(ticks(kernel) + ticks(user) - ticks(cpuStartKernel) - ticks(cpuStartUser)) / 10000000;
  stream << "{\"native\":true,\"passed\":" << (exitCode == 0 ? "true" : "false") << ",\"samplingHealthy\":" << (samplingHealthy ? "true" : "false") << ",\"elapsedSeconds\":" << elapsed << ",\"firstSampleMilliseconds\":" << (firstSample == Time{} ? 0 : std::chrono::duration<double, std::milli>(firstSample - started).count()) << ",\"processCpuPercentOneCore\":" << cpu * 100 / elapsed << ",\"privateBytes\":" << memory.PrivateUsage << ",\"handles\":" << handles << ",\"updates\":" << updates << ",\"exercises\":" << tests << ",\"keyboardPassed\":" << (keyboardPassed ? "true" : "false") << ',';
  stream << "\"workingSetBytes\":" << memory.WorkingSetSize << ",\"peakWorkingSetBytes\":" << memory.PeakWorkingSetSize << ',';
  stream << "\"warmupSeconds\":" << options.warmup << ",\"handleDelta\":" << int64_t(handles) - baselineHandles << ",\"gdiDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) - baselineGdi << ",\"userDelta\":" << int64_t(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)) - baselineUser << ',';
  stream << "\"sortingFailure\":"; stream.jsonString(sortingFailure) << ',';
  stream << "\"sortingPassed\":" << (sortingPassed ? "true" : "false") << ",\"coalescingPassed\":" << (coalescingPassed ? "true" : "false") << ",\"sortRequests\":" << sortRequests << ",\"fullRebuilds\":" << fullRebuilds << ",\"sortPasses\":" << sortPasses << ','; writeMetric(stream, "cachedSort", sortTimes); stream << ',';
  writeMetric(stream, "performanceHover", hoverTimes); stream << ',';
  writeMetric(stream, "tabSwitch", tabTimes); stream << ',';
  writeMetric(stream, "columnResize", columnTimes); stream << ',';
  stream << "\"tabRequests\":" << tabRequests << ",\"tabCommits\":" << tabCommits << ",\"tabSpamPassed\":" << (tabSpamPassed ? "true" : "false") << ',';
  writeMetric(stream, "sampler", samplerTimes); stream << ','; writeMetric(stream, "messageQueue", deliveryTimes); stream << ','; writeMetric(stream, "uiUpdate", updateTimes); stream << ','; writeMetric(stream, "graphPaint", paintTimes); stream << ','; writeMetric(stream, "letterNavigation", inputTimes); stream << ','; writeMetric(stream, "iconRequestToExtraction", icons->latencies); stream << ','; writeMetric(stream, "iconRequestToDisplay", icons->displayLatencies);
  stream << ','; writeMetric(stream, "sampleCadence", sampleCadenceTimes); stream << ",\"partialTableUpdates\":" << partialTableUpdates << ",\"invalidatedRows\":" << invalidatedRows << ",\"resourceUpdates\":" << resourceUpdates;
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
  if (screenshotStep % 2 && selectedTab == StartupTab && (!current || !current->startup->complete || current->startup->entries.empty()) && Clock::now() - started < std::chrono::seconds(30)) return;
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
