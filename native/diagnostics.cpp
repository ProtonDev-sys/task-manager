#include "core.hpp"
#include "report.hpp"

namespace taskmgr {
static int findNext(std::span<const std::wstring> names, std::wstring_view prefix, int start) {
  if (names.empty() || prefix.empty()) return -1;
  const int count = int(names.size());
  const int first = (start % count + count) % count;
  for (int offset = 0; offset < count; ++offset) {
    const int index = (first + offset) % count;
    if (startsWithInsensitive(names[size_t(index)], prefix)) return index;
  }
  return -1;
}
static std::wstring option(const std::vector<std::wstring>& arguments, const std::wstring& key, const std::wstring& fallback = L"") { const auto found = std::find(arguments.begin(), arguments.end(), key); return found != arguments.end() && found + 1 != arguments.end() ? *(found + 1) : fallback; }
static bool flag(const std::vector<std::wstring>& arguments, const std::wstring& key) { return std::find(arguments.begin(), arguments.end(), key) != arguments.end(); }
static void jsonString(Report& stream, const std::wstring& value) {
  stream << '"';
  for (const unsigned char character : utf8(value)) {
    if (character == '"' || character == '\\') stream << '\\' << char(character);
    else if (character < 32) { const char hex[] = "0123456789abcdef"; stream << "\\u00" << hex[character >> 4] << hex[character & 15]; }
    else stream << char(character);
  }
  stream << '"';
}
static int startupReport(const std::wstring& output) {
  const auto began = Clock::now(); const auto inventory = readStartup();
  Report stream{output};
  stream << "{\"native\":true,\"scanMilliseconds\":" << milliseconds(began) << ",\"warnings\":[";
  for (size_t index = 0; index < inventory.warnings.size(); ++index) { if (index) stream << ','; jsonString(stream, inventory.warnings[index]); }
  stream << "],\"entries\":[";
  for (size_t index = 0; index < inventory.entries.size(); ++index) {
    if (index) stream << ','; const auto& entry = inventory.entries[index];
    stream << "{\"key\":"; jsonString(stream, entry.key); stream << ",\"editable\":" << (entry.startupEditable ? "true" : "false") << ",\"cells\":[";
    for (size_t cell = 0; cell < entry.cells.size(); ++cell) { if (cell) stream << ','; jsonString(stream, entry.cells[cell]); } stream << "]}";
  }
  stream << "]}"; return stream ? 0 : 1;
}
static void metric(Report& stream, const std::string& name, std::vector<double> times) { std::sort(times.begin(), times.end()); stream << '"' << name << "\":{\"count\":" << times.size() << ",\"meanMilliseconds\":" << (times.empty() ? 0 : std::accumulate(times.begin(), times.end(), 0.0) / double(times.size())) << ",\"p95Milliseconds\":" << (times.empty() ? 0 : times[size_t(std::ceil(double(times.size()) * .95)) - 1]) << '}'; }
static int selfTest(const std::wstring& output) {
  std::vector<std::pair<std::string, bool>> checks; auto check = [&](std::string name, bool passed) { checks.emplace_back(std::move(name), passed); };
  for (auto& test : startupTests()) checks.push_back(std::move(test));
  check("hexadecimal_bounds", hexadecimal(0) == L"0" && hexadecimal(UINT64_MAX) == L"ffffffffffffffff");
  check("split_empty", splitFields(L"", L',').empty());
  check("split_delimiters", splitFields(L",alpha,,beta,", L',') == std::vector<std::wstring_view>{L"", L"alpha", L"", L"beta"});
  check("split_unicode", splitFields(L"\u03c0\t42\tpath", L'\t') == std::vector<std::wstring_view>{L"\u03c0", L"42", L"path"});
  {
    Report invalid{L""}; invalid << "test"; check("report_open_failure", !invalid);
    wchar_t directory[MAX_PATH]{}; wchar_t temporary[MAX_PATH]{};
    const DWORD length = GetTempPathW(MAX_PATH, directory);
    const bool created = length > 0 && length < MAX_PATH && GetTempFileNameW(directory, L"tmr", 0, temporary) != 0;
    bool written = false, matched = false, removed = false;
    if (created) {
      { Report report{temporary}; report << INT64_MIN << ',' << UINT64_MAX << ',' << 1.25 << ',' << 0.0 << ',' << "text"; written = bool(report); }
      { Handle handle{CreateFileW(temporary, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)}; char text[128]{}; DWORD read = 0;
        matched = handle.value != INVALID_HANDLE_VALUE && ReadFile(handle.value, text, sizeof(text), &read, nullptr) && std::string_view(text, read) == "-9223372036854775808,18446744073709551615,1.25,0,text"; }
      removed = DeleteFileW(temporary) != 0;
    }
    check("report_buffered_write", written); check("report_numeric_roundtrip", matched); check("report_fixture_cleanup", removed);
  }
  const std::vector<std::wstring> names{L"Alpha", L"beta", L"Alpine", L"charlie", L""};
  check("letter_next", findNext(names, L"a", 1) == 2); check("letter_wrap", findNext(names, L"a", 3) == 0); check("letter_case", findNext(names, L"B", 0) == 1); check("prefix", findNext(names, L"alp", 1) == 2); check("missing_prefix", findNext(names, L"zzz", 0) == -1); check("empty_list", findNext({}, L"a", 0) == -1);
  check("letter_negative_start", findNext(names, L"a", -5) == 0); check("letter_large_start", findNext(names, L"a", 100001) == 2); check("letter_blank_prefix", findNext(names, L"", 0) == -1); check("letter_repeat_cycle", findNext(names, L"a", findNext(names, L"a", 1) + 1) == 0);
  check("number_integer", number(123.25, 0) == L"123"); check("number_fraction", number(1.25, 2) == L"1.25"); check("number_negative", number(-12.5, 1) == L"-12.5"); check("number_zero", number(0, 1) == L"0.0"); check("number_precision_bounds", number(1.25, -1) == L"1" && number(1.25, 99) == L"1.250000000"); check("bytes_units", bytes(1024) == L"1.0 KB" && bytes(0) == L"0 B");
  History history; history.add(0, 0); history.add(10, 100); history.add(65, 50); auto points = history.window(65); check("graph_left_boundary", !points.empty() && points.front().time == 5); check("graph_interpolation", !points.empty() && points.front().value == 50); check("graph_right_boundary", !points.empty() && points.back().time == 65);
  history.add(70, 40); points = history.window(75); check("graph_extends_current", points.back().time == 75 && points.back().value == 40); const auto count = history.size(); history.add(69, 1); history.add(NAN, 1); history.add(80, INFINITY); check("graph_rejects_bad_points", history.size() == count); history.add(70, 60); check("graph_replaces_same_time", history.window(70).back().value == 60);
  History startup; startup.add(9, 40); startup.add(10, 50); points = startup.window(10); check("graph_no_fabricated_history", points.front().time == 9 && points.back().time == 10); check("graph_invalid_duration", startup.window(10, 0).empty()); for (int index = 0; index < 10000; ++index) startup.add(11 + index * .01, index % 100); check("graph_bounded_storage", startup.size() <= 512);
  bool monotonic = true, bounded = true;
  History boundary; boundary.add(0, 100, 200); boundary.add(10, 0, 0); boundary.add(65, 0, 0);
  check("graph_peak_includes_interpolated_boundary", boundary.peak(65) == 100);
  check("graph_peak_excludes_future_points", history.peak(5) == 0);
  check("graph_peak_invalid_window", history.peak(NAN) == 0 && history.peak(65, 0) == 0 && history.peak(65, INFINITY) == 0);
  {
    bool matches = true;
    for (const double spacing : {0.1, 1.0}) {
      History ring; std::vector<GraphPoint> reference;
      for (int iteration = 0; iteration < 1300; ++iteration) {
        const double time = iteration * spacing; const GraphPoint value{time, double(iteration % 100), double(iteration % 37)};
        ring.add(time, value.value, value.secondary); reference.push_back(value);
        while (reference.size() > 2 && reference[1].time < time - 60) reference.erase(reference.begin());
        if (reference.size() > 512) reference.erase(reference.begin());
        if (iteration % 97 == 0 || iteration == 1299) {
          const auto actual = ring.window(time);
          const auto visible = std::find_if(reference.begin(), reference.end(), [&](const auto& point) { return point.time >= time - 60; });
          const auto visibleCount = size_t(reference.end() - visible);
          matches = matches && ring.size() == reference.size() && actual.size() == visibleCount;
          if (actual.size() == visibleCount) for (size_t index = 0; index < actual.size(); ++index) matches = matches && actual[index].time == visible[ptrdiff_t(index)].time && actual[index].value == visible[ptrdiff_t(index)].value && actual[index].secondary == visible[ptrdiff_t(index)].secondary;
        }
      }
    }
    check("graph_ring_wrap_and_growth", matches);
    History original; original.add(0, 10); original.add(1, 20); History moved(std::move(original)); original.add(2, 30);
    check("graph_moved_from_reusable", moved.size() == 2 && original.size() == 1 && original.window(2).front().value == 30);
    History assigned; assigned = std::move(moved); moved.add(3, 40); History copied = assigned;
    check("graph_move_assignment_and_copy", assigned.size() == 2 && moved.size() == 1 && copied.window(1).back().value == 20);
    check("graph_stale_series", assigned.recent(61) && !assigned.recent(62) && !History{}.recent(0));
    check("graph_nonfinite_window", assigned.window(1, NAN).empty() && assigned.window(1, INFINITY).empty());
  }
  for (int iteration = 0; iteration < 1000; ++iteration) { const double now = 111 + iteration * .01; startup.add(now, iteration % 100); const auto window = startup.window(now); for (size_t index = 0; index < window.size(); ++index) { bounded = bounded && window[index].time >= now - 60 && window[index].time <= now && std::isfinite(window[index].value); if (index) monotonic = monotonic && window[index].time > window[index - 1].time; } }
  check("graph_monotonic_property", monotonic); check("graph_boundary_property", bounded);
  History secondary; secondary.add(0, 10, 20); secondary.add(1, 30, 5); check("graph_secondary_series", secondary.window(1).back().secondary == 5 && secondary.peak(1) == 30);
  check("number_grouping", grouped(1234567.26, 1) == L"1,234,567.3" && grouped(999) == L"999" && grouped(-12345) == L"-12,345"); check("bits_format", bits(125) == L"1.0 Kbps" && bits(12) == L"96 bps");
  check("duration_format", duration(90061) == L"1:01:01:01"); check("command_executable", commandExecutable(L"\"C:\\Program Files\\App\\app.exe\" --flag") == L"C:\\Program Files\\App\\app.exe");
  check("service_group_caption", serviceGroupCaption(L"LocalSystemNetworkRestricted") == L"Local System (Network Restricted)" && serviceGroupCaption(L"custom") == L"custom");
  Sampler sampler; const auto sample = sampler.sample(); check("native_process_snapshot", !sample.processes.empty()); check("native_memory", sample.memory.total > 0 && sample.memory.available <= sample.memory.total); check("native_cpu_bounds", sample.cpu >= 0 && sample.cpu <= 100); check("native_resources", sample.memory.total > 0 && !sample.cores.empty()); check("own_process_present", std::any_of(sample.processes.begin(), sample.processes.end(), [](const Process& value) { return value.id == GetCurrentProcessId(); }));
  std::wstring command = L"\"" + executable() + L"\" --test-child"; STARTUPINFOW start{sizeof(start)}; PROCESS_INFORMATION child{};
  const bool spawned = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &child) != FALSE; check("owned_child_spawn", spawned);
  if (spawned) {
    Handle childHandle(child.hProcess), thread(child.hThread); FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(childHandle.value, &created, &exited, &kernel, &user); Process owned; owned.id = child.dwProcessId; owned.created = ticks(created); owned.name = L"Owned test child";
    PROCESS_POWER_THROTTLING_STATE available{}; available.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    const bool supportsEfficiency = GetProcessInformation(childHandle.value, ProcessPowerThrottling, &available, sizeof(available)) != FALSE;
    const DWORD efficiencyError = supportsEfficiency ? ERROR_SUCCESS : GetLastError();
    if (!supportsEfficiency) check("efficiency_api_unavailable_safe_skip", efficiencyError == ERROR_INVALID_PARAMETER || efficiencyError == ERROR_NOT_SUPPORTED || efficiencyError == ERROR_CALL_NOT_IMPLEMENTED);
    if (supportsEfficiency) try {
      const auto before = powerPolicy(owned); processAction(owned, Action::BelowNormal);
      efficiencyAction(owned, true); const auto efficient = powerPolicy(owned);
      check("owned_child_efficiency_enable", efficient.priority == IDLE_PRIORITY_CLASS && (efficient.control & efficient.state & PROCESS_POWER_THROTTLING_EXECUTION_SPEED));
      efficiencyAction(owned, true); efficiencyAction(owned, false); const auto restored = powerPolicy(owned);
      check("owned_child_efficiency_restore", restored.priority == BELOW_NORMAL_PRIORITY_CLASS && restored.control == before.control && restored.state == before.state);
      efficiencyAction(owned, false); check("owned_child_efficiency_idempotent", powerPolicy(owned).priority == BELOW_NORMAL_PRIORITY_CLASS);
      auto stalePower = owned; ++stalePower.created; bool rejected = false; try { efficiencyAction(stalePower, true); } catch (...) { rejected = true; }
      check("efficiency_stale_identity_refused", rejected);
      processAction(owned, Action::Normal);
    } catch (...) { check("owned_child_efficiency", false); }
    try { processAction(owned, Action::Idle); check("owned_child_priority_idle", GetPriorityClass(childHandle.value) == IDLE_PRIORITY_CLASS); processAction(owned, Action::BelowNormal); check("owned_child_priority_below", GetPriorityClass(childHandle.value) == BELOW_NORMAL_PRIORITY_CLASS); processAction(owned, Action::Normal); check("owned_child_priority_restore", GetPriorityClass(childHandle.value) == NORMAL_PRIORITY_CLASS); DWORD_PTR affinity = 0, system = 0; GetProcessAffinityMask(childHandle.value, &affinity, &system); processAction(owned, Action::Affinity, hexadecimal(affinity & (~affinity + 1))); DWORD_PTR selected = 0; GetProcessAffinityMask(childHandle.value, &selected, &system); check("owned_child_affinity", selected == (affinity & (~affinity + 1))); processAction(owned, Action::Affinity, hexadecimal(affinity)); check("owned_child_wait_chain", !waitChain(owned).empty()); { std::vector<Process> family(3); family[0] = owned; family[1].id = 900001; family[1].parent = owned.id; family[1].created = owned.created + 1; family[2].id = 900002; family[2].parent = owned.id; family[2].created = owned.created - 1; const auto tree = processTree(family, owned); check("process_tree_descendants", tree.size() == 2 && tree.back().pid == owned.id && tree.front().pid == 900001); } auto stale = owned; ++stale.created; bool refused = false; try { processAction(stale, Action::Idle); } catch (...) { refused = true; } check("stale_identity_refused", refused); processAction(owned, Action::End); check("owned_child_end", WaitForSingleObject(childHandle.value, 5000) == WAIT_OBJECT_0); }
    catch (...) { check("owned_child_actions", false); }
    if (WaitForSingleObject(childHandle.value, 0) == WAIT_TIMEOUT) TerminateProcess(childHandle.value, 1);
  }
  const bool passed = std::all_of(checks.begin(), checks.end(), [](const auto& entry) { return entry.second; });
  if (!output.empty()) { Report stream{output}; stream << "{\"native\":true,\"passed\":" << (passed ? "true" : "false") << ",\"checks\":["; for (size_t index = 0; index < checks.size(); ++index) { if (index) stream << ','; stream << "{\"name\":\"" << checks[index].first << "\",\"passed\":" << (checks[index].second ? "true" : "false") << '}'; } stream << "]}"; if (!stream) return 1; }
  return passed ? 0 : 1;
}
static int components(const std::wstring& output) {
  Report stream{output}; stream << "{\"native\":true,\"passed\":true,";
  for (const int count : {1000, 5000, 20000}) {
    std::vector<std::wstring> names; names.reserve(size_t(count));
    for (int index = 0; index < count; ++index) names.push_back(L"process-" + std::to_wstring(index) + L".exe");
    std::vector<double> navigation, sorting; volatile size_t observed = 0;
    for (int run = 0; run < 35; ++run) { auto start = Clock::now(); observed = size_t(findNext(names, L"zzz", count / 2)); if (run >= 5) navigation.push_back(milliseconds(start)); start = Clock::now(); auto sorted = names; std::sort(sorted.begin(), sorted.end()); observed = sorted.size(); if (run >= 5) sorting.push_back(milliseconds(start)); }
    static_cast<void>(observed); const auto suffix = std::to_string(count); metric(stream, "navigation" + suffix, navigation); stream << ','; metric(stream, "sort" + suffix, sorting); stream << ',';
  }
  History history; for (int index = 0; index < 120; ++index) history.add(index, index % 100); HDC dc = GetDC(nullptr); HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, 1000, 600); auto old = SelectObject(memory, bitmap); std::vector<double> graphTimes, historyTimes;
  for (int run = 0; run < 105; ++run) { auto start = Clock::now(); auto points = history.window(119.5); if (points.empty()) return 1; if (run >= 5) historyTimes.push_back(milliseconds(start)); start = Clock::now(); drawGraph(memory, {0, 0, 1000, 600}, history, 119.5, 100, GraphStyle{}); if (run >= 5) graphTimes.push_back(milliseconds(start)); }
  SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, dc); metric(stream, "graphPaint1000x600", graphTimes); stream << ','; metric(stream, "graphWindow", historyTimes); stream << '}'; return stream ? 0 : 1;
}
int diagnostics(const std::vector<std::wstring>& arguments) {
  if (flag(arguments, L"--test-child")) { Sleep(60000); return 0; }
  const auto output = option(arguments, L"--output", L"native-report.json");
  if (flag(arguments, L"--self-test")) return selfTest(output);
  if (flag(arguments, L"--startup-inventory")) return startupReport(output);
  if (flag(arguments, L"--component-benchmark")) return components(output);
  if (flag(arguments, L"--ui-benchmark")) { RunOptions options; options.hidden = true; options.theme = flag(arguments, L"--dark") ? 1 : 0; options.benchmarkSeconds = std::clamp(std::stoi(option(arguments, L"--seconds", L"15")), 5, 600); options.output = output; options.interval = std::clamp(std::stoi(option(arguments, L"--interval", L"1000")), 0, 4000); options.tab = std::clamp(std::stoi(option(arguments, L"--tab", L"-1")), -1, 6); options.minimized = flag(arguments, L"--minimized"); options.idle = flag(arguments, L"--idle"); options.sortSpam = flag(arguments, L"--sort-spam"); options.tabSpam = flag(arguments, L"--tab-spam"); options.warmup = std::clamp(std::stoi(option(arguments, L"--warmup", L"2")), 0, 60); return runApplication(options); }
  if (flag(arguments, L"--screenshots")) { RunOptions options; options.theme = flag(arguments, L"--dark") ? 1 : 0; options.screenshots = option(arguments, L"--screenshots", L"screenshots"); std::error_code error; std::filesystem::create_directories(options.screenshots, error); return runApplication(options); }
  if (flag(arguments, L"--benchmark")) { Sampler sampler; std::vector<double> times; const int samples = std::clamp(std::stoi(option(arguments, L"--samples", L"30")), 2, 1000), interval = std::clamp(std::stoi(option(arguments, L"--interval", L"100")), 0, 4000); for (int index = 0; index < samples; ++index) { const auto sample = sampler.sample(); times.push_back(sample.duration); if (index + 1 < samples) Sleep(DWORD(interval)); } Report stream{output}; stream << "{\"native\":true,\"passed\":true,"; metric(stream, "sampler", times); stream << '}'; return stream ? 0 : 1; }
  return 2;
}
}
