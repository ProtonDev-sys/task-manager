#include "core.hpp"
#include "report.hpp"
#include <sddl.h>
#include <iterator>

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
  stream.jsonString(utf8(value));
}
static int startupReport(const std::wstring& output) {
  const auto began = Clock::now(); double firstEntries = -1; size_t snapshots = 0, previousCount = 0; bool progressive = true, completed = false;
  const auto inventory = readStartup([&](const StartupInventory& snapshot) { ++snapshots; progressive = progressive && snapshot.entries.size() >= previousCount && !completed; previousCount = snapshot.entries.size(); completed = snapshot.complete; if (firstEntries < 0 && !snapshot.entries.empty()) firstEntries = milliseconds(began); });
  Report stream{output};
  stream << "{\"native\":true,\"passed\":" << (progressive && completed && snapshots == 6 ? "true" : "false") << ",\"firstEntriesMilliseconds\":" << firstEntries << ",\"progressSnapshots\":" << snapshots << ",\"scanMilliseconds\":" << milliseconds(began) << ",\"warnings\":[";
  for (size_t index = 0; index < inventory.warnings.size(); ++index) { if (index) stream << ','; jsonString(stream, inventory.warnings[index]); }
  stream << "],\"entries\":[";
  for (size_t index = 0; index < inventory.entries.size(); ++index) {
    if (index) stream << ','; const auto& entry = inventory.entries[index];
    stream << "{\"key\":"; jsonString(stream, entry.key); stream << ",\"editable\":" << (entry.startupEditable ? "true" : "false") << ",\"cells\":[";
    for (size_t cell = 0; cell < entry.cells.size(); ++cell) { if (cell) stream << ','; jsonString(stream, entry.cells[cell]); } stream << "]}";
  }
  stream << "]}"; return stream && progressive && completed && snapshots == 6 ? 0 : 1;
}
static std::vector<std::pair<std::string, bool>> servicePermissionTests() {
  std::vector<std::pair<std::string, bool>> result;
  Handle token, impersonation;
  const bool available = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &token.value) && DuplicateToken(token.value, SecurityImpersonation, &impersonation.value);
  result.emplace_back("service_permissions_owned_token", available);
  if (available) {
    // Check actual Windows authorization against an in-memory service-style ACL.
    // No service is created, queried or controlled by these fixtures.
    auto permits = [&](DWORD allowed, int action) -> std::optional<bool> {
      const auto text = L"O:SYG:SYD:(A;;0x" + hexadecimal(allowed) + L";;;WD)";
      PSECURITY_DESCRIPTOR descriptor = nullptr;
      if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return std::nullopt;
      GENERIC_MAPPING mapping{READ_CONTROL | SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS,
                              READ_CONTROL | SERVICE_CHANGE_CONFIG,
                              READ_CONTROL | SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL, SERVICE_ALL_ACCESS};
      std::vector<BYTE> privileges(1024); DWORD size = DWORD(privileges.size()), granted = 0; BOOL permitted = FALSE;
      const bool checked = AccessCheck(descriptor, impersonation.value, serviceActionAccess(action), &mapping, reinterpret_cast<PRIVILEGE_SET*>(privileges.data()), &size, &granted, &permitted) != FALSE;
      LocalFree(descriptor);
      if (!checked) return std::nullopt;
      return permitted != FALSE;
    };
    result.emplace_back("service_start_without_stop_permission", permits(SERVICE_START | SERVICE_QUERY_STATUS, 0) == true);
    result.emplace_back("service_stop_without_start_permission", permits(SERVICE_STOP | SERVICE_QUERY_STATUS, 1) == true);
    result.emplace_back("service_start_does_not_require_query_permission", permits(SERVICE_START, 0) == true);
    result.emplace_back("service_restart_requires_both_permissions", permits(SERVICE_START | SERVICE_QUERY_STATUS, 2) == false && permits(SERVICE_STOP | SERVICE_QUERY_STATUS, 2) == false);
    result.emplace_back("service_restart_with_required_permissions", permits(SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS, 2) == true);
  }
  return result;
}
static void metric(Report& stream, const std::string& name, std::vector<double> times) { std::sort(times.begin(), times.end()); stream << '"' << name << "\":{\"count\":" << times.size() << ",\"meanMilliseconds\":" << (times.empty() ? 0 : std::accumulate(times.begin(), times.end(), 0.0) / double(times.size())) << ",\"p95Milliseconds\":" << (times.empty() ? 0 : times[size_t(std::ceil(double(times.size()) * .95)) - 1]) << '}'; }
static bool cacheTimingMatches(Time previous, Time checked, Time current, Clock::duration lifetime, bool invalidated = false) {
  if (previous == Time{} || checked < previous) return false;
  return !invalidated && checked - previous < lifetime ? current == previous : current >= checked;
}
static std::vector<std::pair<std::string, bool>> cacheTimingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  const Time previous = Time{} + std::chrono::seconds(10);
  for (const auto lifetime : {std::chrono::seconds(1), std::chrono::seconds(5)}) {
    const std::string name = lifetime == std::chrono::seconds(1) ? "resource_cache_" : "inventory_cache_";
    const Time boundary = previous + lifetime, before = boundary - std::chrono::milliseconds(1), after = boundary + std::chrono::seconds(2);
    checks.emplace_back(name + "reuses_before_deadline", cacheTimingMatches(previous, before, previous, lifetime));
    checks.emplace_back(name + "refreshes_at_deadline", cacheTimingMatches(previous, boundary, boundary, lifetime));
    checks.emplace_back(name + "accepts_refresh_after_slow_sample", cacheTimingMatches(previous, after, after + std::chrono::milliseconds(1), lifetime));
    checks.emplace_back(name + "rejects_premature_refresh", !cacheTimingMatches(previous, before, before, lifetime));
    checks.emplace_back(name + "rejects_expired_reuse", !cacheTimingMatches(previous, boundary, previous, lifetime));
    if (lifetime == std::chrono::seconds(1)) {
      checks.emplace_back(name + "accepts_topology_refresh_before_deadline", cacheTimingMatches(previous, before, before, lifetime, true));
      checks.emplace_back(name + "rejects_reuse_after_topology_change", !cacheTimingMatches(previous, before, previous, lifetime, true));
    }
  }
  return checks;
}
static std::vector<std::pair<std::string, bool>> reportEncodingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  wchar_t directory[MAX_PATH]{}, path[MAX_PATH]{};
  const DWORD length = GetTempPathW(MAX_PATH, directory);
  const bool created = length > 0 && length < MAX_PATH && GetTempFileNameW(directory, L"tmr", 0, path) != 0;
  checks.emplace_back("report_encoding_owned_fixture", created);
  if (!created) return checks;
  struct Cleanup { const wchar_t* path; ~Cleanup() { DeleteFileW(path); } } cleanup{path};
  auto contents = [&] { std::ifstream input{std::filesystem::path(path), std::ios::binary}; return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()); };
  const std::string message = "Cannot open \"C:\\Folder\\tool.exe\"\r\n\t" + std::string("\0\x01\x1f", 3) + " Caf\xc3\xa9 \xf0\x9f\x9a\x80";
  const std::string expected = "{\"native\":true,\"passed\":false,\"error\":\"Cannot open \\\"C:\\\\Folder\\\\tool.exe\\\"\\u000d\\u000a\\u0009\\u0000\\u0001\\u001f Caf\xc3\xa9 \xf0\x9f\x9a\x80\"}";
  checks.emplace_back("report_error_write_succeeds", writeDiagnosticError(path, message));
  checks.emplace_back("report_error_escapes_controls_quotes_paths_and_keeps_utf8", contents() == expected);
  checks.emplace_back("report_empty_error_write_succeeds", writeDiagnosticError(path, ""));
  checks.emplace_back("report_empty_error_is_quoted", contents() == "{\"native\":true,\"passed\":false,\"error\":\"\"}");
  return checks;
}
static int selfTest(const std::wstring& output) {
  std::vector<std::pair<std::string, bool>> checks; auto check = [&](std::string name, bool passed) { checks.emplace_back(std::move(name), passed); };
  for (auto& test : startupTests()) checks.push_back(std::move(test));
  for (auto& test : gpuEngineTests()) checks.push_back(std::move(test));
  {
    // Synthetic identities only: these tests never open or terminate a process.
    std::vector<Process> family(5000);
    for (size_t index = 0; index < family.size(); ++index) { auto& process = family[index]; process.id = DWORD(900000 + index); process.parent = index ? process.id - 1 : 0; process.created = 1000 + index; }
    const auto root = family.front(); std::reverse(family.begin(), family.end());
    const auto deep = processTree(family, root);
    bool complete = deep.size() == family.size();
    for (size_t index = 0; complete && index < deep.size(); ++index) complete = deep[index].pid == family[index].id && deep[index].created == family[index].created;
    check("process_tree_deep_snapshot_complete_and_children_first", complete);
    std::vector<Identity> roots; for (const auto& process : family) roots.push_back({process.id, process.created});
    check("process_tree_overlapping_child_first_roots", processTree(family, roots) == deep);
    std::reverse(roots.begin(), roots.end());
    check("process_tree_overlapping_parent_first_roots", processTree(family, roots) == deep);
    for (auto& process : family) if (process.id != root.id) process.parent = root.id;
    const auto wide = processTree(family, root);
    std::unordered_set<DWORD> unique; for (const auto& identity : wide) unique.insert(identity.pid);
    check("process_tree_wide_snapshot_complete", wide.size() == family.size() && unique.size() == family.size() && wide.back() == Identity{root.id, root.created});
    std::vector<Process> malformed(5); malformed[0] = root; malformed[0].parent = root.id + 1;
    malformed[1].id = root.id + 1; malformed[1].parent = root.id; malformed[1].created = root.created;
    malformed[2] = malformed[1]; malformed[3].id = root.id + 2; malformed[3].parent = root.id; malformed[3].created = root.created - 1;
    malformed[4].id = root.id + 3; malformed[4].parent = malformed[3].id; malformed[4].created = root.created + 1;
    const auto cyclic = processTree(malformed, malformed[0]);
    check("process_tree_cycles_duplicates_and_reused_parent_excluded", cyclic.size() == 2 && cyclic.front() == Identity{root.id + 1, root.created} && cyclic.back() == Identity{root.id, root.created});
    const std::array<Identity, 3> repeatedRoots{{{root.id, root.created}, {root.id + 1, root.created}, {root.id, root.created}}};
    check("process_tree_overlapping_cycles_once", processTree(malformed, repeatedRoots) == cyclic);
    check("process_tree_root_without_snapshot", processTree({}, root) == std::vector<Identity>{{root.id, root.created}});
  }
  for (auto& test : networkSamplingTests()) checks.push_back(std::move(test));
  for (auto& test : cacheTimingTests()) checks.push_back(std::move(test));
  for (auto& test : servicePermissionTests()) checks.push_back(std::move(test));
  for (auto& test : processorGroupTests()) checks.push_back(std::move(test));
  for (auto& test : accountNameTests()) checks.push_back(std::move(test));
  for (auto& test : sessionIdentityTests()) checks.push_back(std::move(test));
  for (auto& test : reportEncodingTests()) checks.push_back(std::move(test));
  for (auto& test : hardwareInventoryTests()) checks.push_back(std::move(test));
  check("hexadecimal_bounds", hexadecimal(0) == L"0" && hexadecimal(UINT64_MAX) == L"ffffffffffffffff");
  check("counter_integer_decimal", counterInteger(L"0", 10) == 0u && counterInteger(L"0042", 10) == 42u && counterInteger(L"4294967295", 10) == UINT32_MAX);
  check("counter_integer_hexadecimal", counterInteger(L"0000002a", 16) == 42u && counterInteger(L"FFFFFFFF", 16) == UINT32_MAX);
  check("counter_integer_overflow", !counterInteger(L"4294967296", 10) && !counterInteger(L"100000000", 16));
  check("counter_integer_invalid", !counterInteger(L"", 10) && !counterInteger(L"-1", 10) && !counterInteger(L" 1", 10) && !counterInteger(L"1_", 10) && !counterInteger(L"a", 10) && !counterInteger(L"0x42", 16) && !counterInteger(L"1", 2));
  check("utf8_decode_empty", fromUtf8({}).empty());
  check("utf8_decode_ascii", fromUtf8("Access denied.\r\n") == L"Access denied.\r\n");
  check("utf8_decode_localized", fromUtf8("Accès refusé : C:\\资料\\Не найдено.txt") == L"Accès refusé : C:\\资料\\Не найдено.txt");
  check("utf8_decode_supplementary", fromUtf8("\xf0\x9f\x93\x84") == L"\U0001f4c4");
  check("utf8_decode_view_length", fromUtf8(std::string_view("prefix:suffix").substr(0, 6)) == L"prefix");
  check("utf8_decode_embedded_null", fromUtf8(std::string_view("before\0after", 12)) == std::wstring(L"before\0after", 12));
  check("utf8_decode_replacement", fromUtf8("before\xff" "after") == L"before\ufffdafter");
  const std::runtime_error localizedError("Accès refusé : C:\\资料\\report.txt");
  check("utf8_decode_exception_roundtrip", fromUtf8(utf8(L"Unable to terminate process.\n\n" + fromUtf8(localizedError.what()))) == L"Unable to terminate process.\n\nAccès refusé : C:\\资料\\report.txt");
  check("gpu_temperature_invalid_key", gpuTemperature(L"luid_0x100000000_0x0") == -1 && gpuTemperature(L"luid_0x0_0x1trailing") == -1 && gpuTemperature(L"invalid") == -1);
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
  {
    auto matches = [](const std::wstring& text, const std::wstring& file, const std::wstring& parameters) { const auto parsed = parseRunCommand(text); return parsed.file == file && parsed.parameters == parameters; };
    check("run_command_bare_program", matches(L"notepad.exe report.txt", L"notepad.exe", L"report.txt"));
    check("run_command_leading_whitespace", matches(L" \t notepad.exe \t report.txt", L"notepad.exe", L"report.txt"));
    check("run_command_tab_separator", matches(L"notepad.exe\t\"report name.txt\"", L"notepad.exe", L"\"report name.txt\""));
    check("run_command_quoted_path", matches(L" \t\"C:\\Missing App\\tool.exe\" \t \"report name.txt\"  --flag", L"C:\\Missing App\\tool.exe", L"\"report name.txt\"  --flag"));
    check("run_command_blank", matches(L" \t\r\n", L"", L""));
    bool unclosed = false; try { parseRunCommand(L"\"C:\\Missing App\\tool.exe"); } catch (const std::exception&) { unclosed = true; }
    check("run_command_unclosed_quote", unclosed);
    check("inventory_command_malformed_retained", commandExecutable(L"\"C:\\Missing App\\tool.exe").empty());
    check("inventory_command_missing_program", commandExecutable(L"\"C:\\Missing App\\tool.exe\" --flag") == L"C:\\Missing App\\tool.exe");
    wchar_t temporary[MAX_PATH]{}; const DWORD length = GetTempPathW(MAX_PATH, temporary);
    const auto suffix = std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    const auto directory = length > 0 && length < MAX_PATH ? std::wstring(temporary) + L"TaskManagerRun-" + suffix : L"";
    const bool created = !directory.empty() && CreateDirectoryW(directory.c_str(), nullptr) != FALSE;
    check("run_command_fixture_directory", created);
    if (created) {
      const auto prefix = directory + L"\\Example", spaced = directory + L"\\Example Folder", program = spaced + L"\\Example App.exe";
      const bool prefixCreated = CreateDirectoryW(prefix.c_str(), nullptr) != FALSE, spacedCreated = CreateDirectoryW(spaced.c_str(), nullptr) != FALSE;
      bool fileCreated = false;
      { Handle file(CreateFileW(program.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); fileCreated = file.value != INVALID_HANDLE_VALUE; }
      check("run_command_fixture_file", prefixCreated && spacedCreated && fileCreated);
      const auto variable = L"TASKMGR_RUN_TEST_" + suffix, token = L"%" + variable + L"%";
      bool environmentCreated = false;
      if (prefixCreated && spacedCreated && fileCreated) try {
        const std::wstring arguments = L"\"report name.txt\"  --flag=\"a b\"";
        check("run_command_unquoted_spaced_path", matches(program + L" " + arguments, program, arguments));
        check("run_command_spaced_path_without_arguments", matches(program, program, L""));
        check("run_command_inferred_extension", matches(program.substr(0, program.size() - 4) + L" " + arguments, program, arguments));
        check("run_command_existing_directory", matches(spaced, spaced, L""));
        check("inventory_command_spaced_path", commandExecutable(program) == program);
        check("inventory_command_tab_arguments", commandExecutable(program + L"\t--flag") == program);
        check("inventory_command_inferred_extension", commandExecutable(program.substr(0, program.size() - 4) + L" --flag") == program);
        environmentCreated = SetEnvironmentVariableW(variable.c_str(), spaced.c_str()) != FALSE;
        check("run_command_fixture_environment", environmentCreated);
        if (environmentCreated) {
          check("run_command_unquoted_environment_path", matches(token + L"\\Example App.exe " + arguments, program, arguments));
          check("run_command_quoted_environment_path", matches(L"\"" + token + L"\\Example App.exe\" " + arguments, program, arguments));
          check("run_command_argument_environment_preserved", matches(L"\"" + program + L"\" " + token, program, token));
          check("inventory_command_environment_path", commandExecutable(token + L"\\Example App.exe --flag") == program);
          const bool longEnvironment = SetEnvironmentVariableW(variable.c_str(), std::wstring(16384, L'x').c_str()) != FALSE;
          check("inventory_command_long_environment_fixture", longEnvironment);
          if (longEnvironment) check("inventory_command_expansion_bound", commandExecutable(token + token).empty());
        }
      } catch (const std::exception&) { check("run_command_fixture_parsing", false); }
      const bool environmentRemoved = !environmentCreated || SetEnvironmentVariableW(variable.c_str(), nullptr) != FALSE;
      const bool fileRemoved = !fileCreated || DeleteFileW(program.c_str()) != FALSE;
      const bool spacedRemoved = !spacedCreated || RemoveDirectoryW(spaced.c_str()) != FALSE;
      const bool prefixRemoved = !prefixCreated || RemoveDirectoryW(prefix.c_str()) != FALSE;
      const bool directoryRemoved = RemoveDirectoryW(directory.c_str()) != FALSE;
      check("run_command_fixture_cleanup", environmentRemoved && fileRemoved && spacedRemoved && prefixRemoved && directoryRemoved);
    }
  }
  check("service_group_caption", serviceGroupCaption(L"LocalSystemNetworkRestricted") == L"Local System (Network Restricted)" && serviceGroupCaption(L"custom") == L"custom");
  Sampler sampler; const auto sample = sampler.sample(); check("native_process_snapshot", !sample.processes.empty()); check("native_memory", sample.memory.total > 0 && sample.memory.available <= sample.memory.total); check("native_cpu_bounds", sample.cpu >= 0 && sample.cpu <= 100); check("native_resources", sample.memory.total > 0 && !sample.cores.empty()); check("own_process_present", std::any_of(sample.processes.begin(), sample.processes.end(), [](const Process& value) { return value.id == GetCurrentProcessId(); }));
  check("adaptive_refresh_fast_floor", adaptiveRefreshInterval(0, 0) == 250 && adaptiveRefreshInterval(3, 2) == 250);
  check("adaptive_refresh_includes_display_cost", adaptiveRefreshInterval(8, 2) == 500 && adaptiveRefreshInterval(20, 4) == 1200);
  check("adaptive_refresh_slow_ceiling", adaptiveRefreshInterval(100, 10) == 4000);
  check("adaptive_refresh_invalid_costs", adaptiveRefreshInterval(NAN, 0) == 4000 && adaptiveRefreshInterval(INFINITY, 0) == 4000 && adaptiveRefreshInterval(-1, 0) == 4000);
  {
    const auto retained = sample; const auto next = sampler.sample();
    const bool resourcesReused = next.resourcesSampledAt == sample.resourcesSampledAt;
    check("resource_counters_follow_cache_interval", cacheTimingMatches(sample.resourcesSampledAt, next.resourcesCheckedAt, next.resourcesSampledAt, std::chrono::seconds(1), next.cpuTopologyChanged) &&
      (resourcesReused ? next.cores == sample.cores && next.disks.size() == sample.disks.size() : next.resourcesSampledAt == next.resourcesCheckedAt));
    Sleep(1050); const auto later = sampler.sample();
    check("resource_counters_refresh_after_one_second", later.resourcesSampledAt > next.resourcesSampledAt);
    check("inventory_snapshots_follow_cache_interval", sample.inventory && next.inventory && cacheTimingMatches(sample.inventorySampledAt, next.inventoryCheckedAt, next.inventorySampledAt, std::chrono::seconds(5)) &&
      ((next.inventory == sample.inventory) == (next.inventorySampledAt == sample.inventorySampledAt)) && next.services.data() == next.inventory->services.data() && next.sessions.data() == next.inventory->sessions.data());
    sampler.invalidateInventory(); const auto refreshed = sampler.sample();
    check("manual_refresh_invalidates_resource_cache", refreshed.resourcesSampledAt > later.resourcesSampledAt);
    check("inventory_refresh_replaces_snapshot", refreshed.inventory && refreshed.inventory != sample.inventory && refreshed.services.data() == refreshed.inventory->services.data());
    check("inventory_retained_sample_lifetime", retained.inventory == sample.inventory && retained.services.data() == retained.inventory->services.data() && retained.sessions.data() == retained.inventory->sessions.data());
    Sample detached;
    { Sampler temporary; detached = temporary.sample(); }
    check("inventory_outlives_sampler", detached.inventory && detached.services.data() == detached.inventory->services.data() && detached.sessions.data() == detached.inventory->sessions.data());
  }
  std::wstring command = L"\"" + executable() + L"\" --test-child"; STARTUPINFOW start{sizeof(start)}; PROCESS_INFORMATION child{};
  const bool spawned = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &child) != FALSE; check("owned_child_spawn", spawned);
  if (spawned) {
    Handle childHandle(child.hProcess), thread(child.hThread); FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(childHandle.value, &created, &exited, &kernel, &user); Process owned; owned.id = child.dwProcessId; owned.created = ticks(created); owned.name = L"Owned test child";
    const auto withChild = sampler.sample();
    check("fast_process_snapshot_detects_new_child", std::any_of(withChild.processes.begin(), withChild.processes.end(), [&](const Process& process) { return process.id == owned.id && process.created == owned.created; }));
    {
      wchar_t directory[MAX_PATH]{}, destination[MAX_PATH]{};
      const DWORD length = GetTempPathW(MAX_PATH, directory);
      const bool destinationCreated = length > 0 && length < MAX_PATH && GetTempFileNameW(directory, L"tmd", 0, destination) != 0;
      bool temporaryCreated = false, written = false, refused = false, preserved = false;
      const auto temporary = destinationCreated ? std::wstring(destination) + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp" : L"";
      constexpr char sentinel[] = "Existing temporary file belongs to another operation.";
      if (destinationCreated) {
        { Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
          temporaryCreated = file.value != INVALID_HANDLE_VALUE; DWORD bytesWritten = 0;
          written = temporaryCreated && WriteFile(file.value, sentinel, sizeof(sentinel), &bytesWritten, nullptr) && bytesWritten == sizeof(sentinel); }
        if (written) {
          // CREATE_NEW must fail before MiniDumpWriteDump: this test never writes a dump.
          try { processAction(owned, Action::Dump, destination); }
          catch (const std::exception& error) { refused = std::string(error.what()) == utf8(winerror(ERROR_FILE_EXISTS)); }
          { Handle file(CreateFileW(temporary.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
            std::array<char, sizeof(sentinel) + 1> contents{}; DWORD bytesRead = 0;
            preserved = file.value != INVALID_HANDLE_VALUE && ReadFile(file.value, contents.data(), DWORD(contents.size()), &bytesRead, nullptr) && bytesRead == sizeof(sentinel) && memcmp(contents.data(), sentinel, sizeof(sentinel)) == 0; }
        }
      }
      check("dump_collision_fixture_created", destinationCreated && temporaryCreated && written);
      check("dump_collision_error_reported", refused);
      check("dump_collision_existing_file_preserved", preserved);
      const bool temporaryRemoved = !temporaryCreated || DeleteFileW(temporary.c_str()) != FALSE || GetLastError() == ERROR_FILE_NOT_FOUND;
      const bool destinationRemoved = !destinationCreated || DeleteFileW(destination) != FALSE;
      check("dump_collision_fixture_cleanup", temporaryRemoved && destinationRemoved);
    }
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
      efficiencyAction(owned, true);
      PROCESS_POWER_THROTTLING_STATE external{}; external.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
      external.ControlMask = before.control | PROCESS_POWER_THROTTLING_EXECUTION_SPEED; external.StateMask = before.state & ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
      const bool changed = SetProcessInformation(childHandle.value, ProcessPowerThrottling, &external, sizeof(external)) != FALSE;
      check("owned_child_efficiency_external_disable", changed);
      if (changed) {
        efficiencyAction(owned, false); const auto afterExternal = powerPolicy(owned);
        check("owned_child_efficiency_restore_after_external_disable", afterExternal.priority == BELOW_NORMAL_PRIORITY_CLASS && afterExternal.control == before.control && afterExternal.state == before.state);
        processAction(owned, Action::Normal); efficiencyAction(owned, true); efficiencyAction(owned, false);
        check("owned_child_efficiency_releases_saved_policy", powerPolicy(owned).priority == NORMAL_PRIORITY_CLASS);
        processAction(owned, Action::BelowNormal); efficiencyAction(owned, true); processAction(owned, Action::Normal);
        const bool changedAgain = SetProcessInformation(childHandle.value, ProcessPowerThrottling, &external, sizeof(external)) != FALSE;
        check("owned_child_efficiency_external_priority_setup", changedAgain);
        if (changedAgain) {
          efficiencyAction(owned, false);
          check("owned_child_efficiency_preserves_external_priority", powerPolicy(owned).priority == NORMAL_PRIORITY_CLASS);
          efficiencyAction(owned, true); efficiencyAction(owned, false);
          check("owned_child_efficiency_external_priority_becomes_new_baseline", powerPolicy(owned).priority == NORMAL_PRIORITY_CLASS);
        }
      }
      auto stalePower = owned; ++stalePower.created; bool rejected = false; try { efficiencyAction(stalePower, true); } catch (...) { rejected = true; }
      check("efficiency_stale_identity_refused", rejected);
      processAction(owned, Action::Normal);
    } catch (...) { check("owned_child_efficiency", false); }
    try { processAction(owned, Action::Idle); check("owned_child_priority_idle", GetPriorityClass(childHandle.value) == IDLE_PRIORITY_CLASS); processAction(owned, Action::BelowNormal); check("owned_child_priority_below", GetPriorityClass(childHandle.value) == BELOW_NORMAL_PRIORITY_CLASS); processAction(owned, Action::Normal); check("owned_child_priority_restore", GetPriorityClass(childHandle.value) == NORMAL_PRIORITY_CLASS); DWORD_PTR affinity = 0, system = 0; GetProcessAffinityMask(childHandle.value, &affinity, &system); processAction(owned, Action::Affinity, hexadecimal(affinity & (~affinity + 1))); DWORD_PTR selected = 0; GetProcessAffinityMask(childHandle.value, &selected, &system); check("owned_child_affinity", selected == (affinity & (~affinity + 1))); processAction(owned, Action::Affinity, hexadecimal(affinity)); check("owned_child_wait_chain", !waitChain(owned).empty()); { std::vector<Process> family(3); family[0] = owned; family[1].id = 900001; family[1].parent = owned.id; family[1].created = owned.created + 1; family[2].id = 900002; family[2].parent = owned.id; family[2].created = owned.created - 1; const auto tree = processTree(family, owned); check("process_tree_descendants", tree.size() == 2 && tree.back().pid == owned.id && tree.front().pid == 900001); } auto stale = owned; ++stale.created; bool refused = false; try { processAction(stale, Action::Idle); } catch (...) { refused = true; } check("stale_identity_refused", refused); processAction(owned, Action::End); check("owned_child_end", WaitForSingleObject(childHandle.value, 5000) == WAIT_OBJECT_0); }
    catch (...) { check("owned_child_actions", false); }
    const auto withoutChild = sampler.sample();
    check("fast_process_snapshot_removes_exited_child", std::none_of(withoutChild.processes.begin(), withoutChild.processes.end(), [&](const Process& process) { return process.id == owned.id && process.created == owned.created; }));
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
static int samplerBenchmark(const std::vector<std::wstring>& arguments, const std::wstring& output) {
  const auto initialization = Clock::now(); Sampler sampler;
  const double initializationMilliseconds = milliseconds(initialization);
  const int samples = std::clamp(std::stoi(option(arguments, L"--samples", L"30")), 2, 1000);
  const int interval = std::clamp(std::stoi(option(arguments, L"--interval", L"100")), 0, 4000);
  const int warmup = std::clamp(std::stoi(option(arguments, L"--warmup-samples", L"5")), 0, 100);
  for (int index = 0; index < warmup; ++index) { sampler.sample(); if (interval) Sleep(DWORD(interval)); }
  std::vector<double> times; times.reserve(size_t(samples)); std::array<std::vector<double>, 6> stages;
  for (auto& values : stages) values.reserve(size_t(samples));
  FILETIME created{}, exited{}, kernelStart{}, userStart{}, kernelEnd{}, userEnd{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelStart, &userStart)) return 1;
  const auto began = Clock::now(); size_t minimumProcesses = SIZE_MAX, maximumProcesses = 0;
  for (int index = 0; index < samples; ++index) {
    const auto sample = sampler.sample(); times.push_back(sample.duration);
    minimumProcesses = std::min(minimumProcesses, sample.processes.size()); maximumProcesses = std::max(maximumProcesses, sample.processes.size());
    for (size_t stage = 0; stage < stages.size(); ++stage) stages[stage].push_back(sample.stages[stage]);
    if (index + 1 < samples && interval) Sleep(DWORD(interval));
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - began).count();
  PROCESS_MEMORY_COUNTERS_EX memory{sizeof(memory)}; DWORD handles = 0;
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernelEnd, &userEnd) ||
      !GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)) ||
      !GetProcessHandleCount(GetCurrentProcess(), &handles)) return 1;
  const double cpuSeconds = double(ticks(kernelEnd) + ticks(userEnd) - ticks(kernelStart) - ticks(userStart)) / 10000000;
  Report stream{output}; stream << "{\"native\":true,\"passed\":true,\"initializationMilliseconds\":" << initializationMilliseconds
    << ",\"warmupSamples\":" << warmup << ",\"intervalMilliseconds\":" << interval << ",\"elapsedSeconds\":" << elapsed
    << ",\"processCpuSeconds\":" << cpuSeconds << ",\"processCpuPercentOneCore\":" << cpuSeconds * 100 / std::max(.001, elapsed)
    << ",\"cpuMillisecondsPerSample\":" << cpuSeconds * 1000 / samples << ",\"privateBytes\":" << memory.PrivateUsage
    << ",\"workingSetBytes\":" << memory.WorkingSetSize << ",\"peakWorkingSetBytes\":" << memory.PeakWorkingSetSize
    << ",\"handles\":" << handles << ",\"minimumProcesses\":" << minimumProcesses << ",\"maximumProcesses\":" << maximumProcesses << ',';
  metric(stream, "sampler", times);
  static constexpr const char* names[] = {"processInventory", "cpuAndMemory", "pdhAndDisk", "network", "gpuAggregation", "servicesSessionsStartup"};
  for (size_t stage = 0; stage < stages.size(); ++stage) { stream << ','; metric(stream, names[stage], stages[stage]); }
  stream << '}'; return stream ? 0 : 1;
}
int diagnostics(const std::vector<std::wstring>& arguments) {
  if (flag(arguments, L"--test-child")) { Sleep(60000); return 0; }
  const auto output = option(arguments, L"--output", L"native-report.json");
  if (flag(arguments, L"--self-test")) return selfTest(output);
  if (flag(arguments, L"--startup-inventory")) return startupReport(output);
  if (flag(arguments, L"--component-benchmark")) return components(output);
  if (flag(arguments, L"--ui-benchmark")) { RunOptions options; options.hidden = true; options.theme = flag(arguments, L"--dark") ? 1 : 0; options.benchmarkSeconds = std::clamp(std::stoi(option(arguments, L"--seconds", L"15")), 5, 600); options.output = output; options.interval = std::clamp(std::stoi(option(arguments, L"--interval", L"1000")), 0, 4000); options.tab = std::clamp(std::stoi(option(arguments, L"--tab", L"-1")), -1, 6); options.minimized = flag(arguments, L"--minimized"); options.idle = flag(arguments, L"--idle"); options.sortSpam = flag(arguments, L"--sort-spam"); options.tabSpam = flag(arguments, L"--tab-spam"); options.warmup = std::clamp(std::stoi(option(arguments, L"--warmup", L"2")), 0, 60); return runApplication(options); }
  if (flag(arguments, L"--screenshots")) { RunOptions options; options.theme = flag(arguments, L"--dark") ? 1 : 0; options.screenshots = option(arguments, L"--screenshots", L"screenshots"); std::error_code error; std::filesystem::create_directories(options.screenshots, error); return runApplication(options); }
  if (flag(arguments, L"--benchmark")) return samplerBenchmark(arguments, output);
  return 2;
}
}
