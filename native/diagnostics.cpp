#include "core.hpp"
#include <iomanip>

namespace taskmgr {
static std::wstring option(const std::vector<std::wstring>& arguments, const std::wstring& key, const std::wstring& fallback = L"") { const auto found = std::find(arguments.begin(), arguments.end(), key); return found != arguments.end() && found + 1 != arguments.end() ? *(found + 1) : fallback; }
static bool flag(const std::vector<std::wstring>& arguments, const std::wstring& key) { return std::find(arguments.begin(), arguments.end(), key) != arguments.end(); }
static void metric(std::ostream& stream, const std::string& name, std::vector<double> times) { std::sort(times.begin(), times.end()); stream << '"' << name << "\":{\"count\":" << times.size() << ",\"meanMilliseconds\":" << (times.empty() ? 0 : std::accumulate(times.begin(), times.end(), 0.0) / double(times.size())) << ",\"p95Milliseconds\":" << (times.empty() ? 0 : times[size_t(std::ceil(double(times.size()) * .95)) - 1]) << '}'; }
static int selfTest(const std::wstring& output) {
  std::vector<std::pair<std::string, bool>> checks; auto check = [&](std::string name, bool passed) { checks.emplace_back(std::move(name), passed); };
  const std::vector<std::wstring> names{L"Alpha", L"beta", L"Alpine", L"charlie", L""};
  check("letter_next", findNext(names, L"a", 1) == 2); check("letter_wrap", findNext(names, L"a", 3) == 0); check("letter_case", findNext(names, L"B", 0) == 1); check("prefix", findNext(names, L"alp", 1) == 2); check("missing_prefix", findNext(names, L"zzz", 0) == -1); check("empty_list", findNext({}, L"a", 0) == -1);
  check("letter_negative_start", findNext(names, L"a", -5) == 0); check("letter_large_start", findNext(names, L"a", 100001) == 2); check("letter_blank_prefix", findNext(names, L"", 0) == -1); check("letter_repeat_cycle", findNext(names, L"a", findNext(names, L"a", 1) + 1) == 0);
  Process process; process.id = 123; process.name = L"Example.exe"; Metadata metadata; metadata.publisher = L"Example company"; metadata.description = L"Editor";
  check("filter_name", matches(process, metadata, L"example")); check("filter_pid", matches(process, metadata, L"123")); check("filter_publisher", matches(process, metadata, L"company")); check("filter_description", matches(process, metadata, L"editor")); check("filter_missing", !matches(process, metadata, L"absent")); check("filter_empty", matches(process, metadata, L""));
  History history; history.add(0, 0); history.add(10, 100); history.add(65, 50); auto points = history.window(65); check("graph_left_boundary", !points.empty() && points.front().time == 5); check("graph_interpolation", !points.empty() && points.front().value == 50); check("graph_right_boundary", !points.empty() && points.back().time == 65);
  history.add(70, 40); points = history.window(75); check("graph_extends_current", points.back().time == 75 && points.back().value == 40); const auto count = history.size(); history.add(69, 1); history.add(NAN, 1); history.add(80, INFINITY); check("graph_rejects_bad_points", history.size() == count); history.add(70, 60); check("graph_replaces_same_time", history.window(70).back().value == 60);
  History startup; startup.add(9, 40); startup.add(10, 50); points = startup.window(10); check("graph_no_fabricated_history", points.front().time == 9 && points.back().time == 10); check("graph_invalid_duration", startup.window(10, 0).empty()); for (int index = 0; index < 10000; ++index) startup.add(11 + index * .01, index % 100); check("graph_bounded_storage", startup.size() <= 512);
  bool monotonic = true, bounded = true;
  for (int iteration = 0; iteration < 1000; ++iteration) { const double now = 111 + iteration * .01; startup.add(now, iteration % 100); const auto window = startup.window(now); for (size_t index = 0; index < window.size(); ++index) { bounded = bounded && window[index].time >= now - 60 && window[index].time <= now && std::isfinite(window[index].value); if (index) monotonic = monotonic && window[index].time > window[index - 1].time; } }
  check("graph_monotonic_property", monotonic); check("graph_boundary_property", bounded);
  Sampler sampler; const auto sample = sampler.sample(); check("native_process_snapshot", !sample.processes.empty()); check("native_memory", sample.memoryTotal > 0 && sample.memoryUsed <= sample.memoryTotal); check("native_cpu_bounds", sample.cpu >= 0 && sample.cpu <= 100); check("native_resources", sample.resources.size() >= 2); check("own_process_present", std::any_of(sample.processes.begin(), sample.processes.end(), [](const Process& value) { return value.id == GetCurrentProcessId(); }));
  std::wstring command = L"\"" + executable() + L"\" --test-child"; STARTUPINFOW start{sizeof(start)}; PROCESS_INFORMATION child{};
  const bool spawned = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &start, &child) != FALSE; check("owned_child_spawn", spawned);
  if (spawned) {
    Handle childHandle(child.hProcess), thread(child.hThread); FILETIME created{}, exited{}, kernel{}, user{}; GetProcessTimes(childHandle.value, &created, &exited, &kernel, &user); Process owned; owned.id = child.dwProcessId; owned.created = ticks(created); owned.name = L"Owned test child";
    try { processAction(owned, Action::Idle); check("owned_child_priority_idle", GetPriorityClass(childHandle.value) == IDLE_PRIORITY_CLASS); processAction(owned, Action::Normal); check("owned_child_priority_restore", GetPriorityClass(childHandle.value) == NORMAL_PRIORITY_CLASS); DWORD_PTR affinity = 0, system = 0; GetProcessAffinityMask(childHandle.value, &affinity, &system); std::wostringstream mask; mask << std::hex << (affinity & (~affinity + 1)); processAction(owned, Action::Affinity, mask.str()); DWORD_PTR selected = 0; GetProcessAffinityMask(childHandle.value, &selected, &system); check("owned_child_affinity", selected == (affinity & (~affinity + 1))); mask.str(L""); mask << std::hex << affinity; processAction(owned, Action::Affinity, mask.str()); check("owned_child_wait_chain", !waitChain(owned).empty()); auto stale = owned; ++stale.created; bool refused = false; try { processAction(stale, Action::Idle); } catch (...) { refused = true; } check("stale_identity_refused", refused); processAction(owned, Action::End); check("owned_child_end", WaitForSingleObject(childHandle.value, 5000) == WAIT_OBJECT_0); }
    catch (...) { check("owned_child_actions", false); }
    if (WaitForSingleObject(childHandle.value, 0) == WAIT_TIMEOUT) TerminateProcess(childHandle.value, 1);
  }
  const bool passed = std::all_of(checks.begin(), checks.end(), [](const auto& entry) { return entry.second; });
  if (!output.empty()) { std::ofstream stream{std::filesystem::path(output)}; stream << "{\"native\":true,\"passed\":" << (passed ? "true" : "false") << ",\"checks\":["; for (size_t index = 0; index < checks.size(); ++index) { if (index) stream << ','; stream << "{\"name\":\"" << checks[index].first << "\",\"passed\":" << (checks[index].second ? "true" : "false") << '}'; } stream << "]}"; }
  return passed ? 0 : 1;
}
static int components(const std::wstring& output) {
  std::ofstream stream{std::filesystem::path(output)}; stream << "{\"native\":true,\"passed\":true,";
  for (const int count : {1000, 5000, 20000}) {
    std::vector<Process> processes(size_t(count), Process{}); std::vector<std::wstring> names; Metadata metadata;
    for (int index = 0; index < count; ++index) { processes[size_t(index)].id = DWORD(index); processes[size_t(index)].name = L"process-" + std::to_wstring(index) + L".exe"; names.push_back(processes[size_t(index)].name); }
    std::vector<double> filtering, navigation, sorting; volatile size_t observed = 0;
    for (int run = 0; run < 35; ++run) { auto start = Clock::now(); size_t matched = 0; for (const auto& process : processes) if (matches(process, metadata, L"123")) ++matched; observed = matched; if (run >= 5) filtering.push_back(milliseconds(start)); start = Clock::now(); observed = size_t(findNext(names, L"zzz", count / 2)); if (run >= 5) navigation.push_back(milliseconds(start)); start = Clock::now(); auto sorted = names; std::sort(sorted.begin(), sorted.end()); observed = sorted.size(); if (run >= 5) sorting.push_back(milliseconds(start)); }
    static_cast<void>(observed); const auto suffix = std::to_string(count); metric(stream, "filter" + suffix, filtering); stream << ','; metric(stream, "navigation" + suffix, navigation); stream << ','; metric(stream, "sort" + suffix, sorting); stream << ',';
  }
  History history; for (int index = 0; index < 120; ++index) history.add(index, index % 100); HDC dc = GetDC(nullptr); HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, 1000, 600); auto old = SelectObject(memory, bitmap); std::vector<double> graphTimes, historyTimes;
  for (int run = 0; run < 105; ++run) { auto start = Clock::now(); auto points = history.window(119.5); if (points.empty()) return 1; if (run >= 5) historyTimes.push_back(milliseconds(start)); start = Clock::now(); drawGraph(memory, {0, 0, 1000, 600}, history, 119.5, 100, RGB(0, 120, 215)); if (run >= 5) graphTimes.push_back(milliseconds(start)); }
  SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(nullptr, dc); metric(stream, "graphPaint1000x600", graphTimes); stream << ','; metric(stream, "graphWindow", historyTimes); stream << '}'; return 0;
}
int diagnostics(const std::vector<std::wstring>& arguments) {
  if (flag(arguments, L"--test-child")) { Sleep(60000); return 0; }
  const auto output = option(arguments, L"--output", L"native-report.json");
  if (flag(arguments, L"--self-test")) return selfTest(output);
  if (flag(arguments, L"--component-benchmark")) return components(output);
  if (flag(arguments, L"--ui-benchmark")) return runApplication(true, std::clamp(std::stoi(option(arguments, L"--seconds", L"15")), 5, 600), output, std::clamp(std::stoi(option(arguments, L"--interval", L"1000")), 0, 4000), std::clamp(std::stoi(option(arguments, L"--tab", L"-1")), -1, 6), flag(arguments, L"--minimized"), flag(arguments, L"--idle"), std::clamp(std::stoi(option(arguments, L"--warmup", L"2")), 0, 60));
  if (flag(arguments, L"--benchmark")) { Sampler sampler; std::vector<double> times; const int samples = std::clamp(std::stoi(option(arguments, L"--samples", L"30")), 2, 1000), interval = std::clamp(std::stoi(option(arguments, L"--interval", L"100")), 0, 4000); for (int index = 0; index < samples; ++index) { const auto sample = sampler.sample(); times.push_back(sample.duration); if (index + 1 < samples) Sleep(DWORD(interval)); } std::ofstream stream{std::filesystem::path(output)}; stream << "{\"native\":true,\"passed\":true,"; metric(stream, "sampler", times); stream << '}'; return 0; }
  return 2;
}
}
