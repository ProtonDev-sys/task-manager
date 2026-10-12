#include "core.hpp"
#include <limits>
#include <ws2ipdef.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <shlobj.h>
#include <sddl.h>

namespace taskmgr {
std::wstring hexadecimal(uint64_t value) { wchar_t text[17]{}; swprintf_s(text, L"%llx", static_cast<unsigned long long>(value)); return text; }
std::optional<uint32_t> counterInteger(std::wstring_view text, unsigned base) {
  if (text.empty() || (base != 10 && base != 16)) return std::nullopt;
  uint32_t value = 0;
  for (const wchar_t character : text) {
    const unsigned digit = character >= L'0' && character <= L'9' ? unsigned(character - L'0') : character >= L'a' && character <= L'f' ? unsigned(character - L'a') + 10 : character >= L'A' && character <= L'F' ? unsigned(character - L'A') + 10 : base;
    if (digit >= base || value > (UINT32_MAX - digit) / base) return std::nullopt;
    value = value * base + digit;
  }
  return value;
}
std::vector<std::wstring_view> splitFields(std::wstring_view text, wchar_t delimiter) {
  std::vector<std::wstring_view> fields;
  while (!text.empty()) { const size_t end = text.find(delimiter); fields.push_back(text.substr(0, end)); if (end == text.npos) break; text.remove_prefix(end + 1); }
  return fields;
}
static void cacheProcessGpu(std::span<const Process> processes, std::unordered_map<DWORD, ProcessGpu>& values, ProcessGpuCache& cache) {
  cache.clear();
  for (const auto& process : processes) if (const auto found = values.find(process.id); found != values.end() && found->second.value > 0)
    cache.emplace(Identity{process.id, process.created}, std::make_shared<const ProcessGpu>(std::move(found->second)));
}
static void applyProcessGpu(std::span<Process> processes, const ProcessGpuCache& cache) {
  for (auto& process : processes) {
    const auto found = cache.find(Identity{process.id, process.created}); process.gpuUsage = found == cache.end() ? nullptr : found->second;
    process.gpu = process.gpuUsage ? process.gpuUsage->value : 0; process.gpuEngine = process.gpuUsage ? process.gpuUsage->engine : L"";
  }
}
static void aggregateGpuEngines(GpuInfo& gpu, std::span<const std::pair<std::wstring, double>> engines, std::unordered_map<DWORD, ProcessGpu>& processGpu) {
  std::map<std::wstring, double> engineTotals;
  std::unordered_map<DWORD, std::map<std::wstring, double>> perProcess;
  for (const auto& [name, value] : engines) {
    if (name.find(gpu.key) == std::wstring::npos) continue;
    const auto engine = name.find(L"_engtype_"); if (engine == std::wstring::npos) continue;
    const auto engineNumber = name.find(L"_eng_"), physicalAdapter = name.find(L"_phys_"); const auto type = name.substr(engine + 9);
    // Linked physical adapters share a LUID but can reuse engine numbers.
    const auto instanceStart = physicalAdapter < engineNumber ? physicalAdapter : engineNumber;
    const auto instance = (instanceStart == std::wstring::npos ? L"" : name.substr(instanceStart, engine - instanceStart)) + L"|" + type;
    engineTotals[instance] += value;
    if (name.starts_with(L"pid_")) {
      const auto end = name.find(L'_', 4);
      if (end != name.npos) if (const auto pid = counterInteger(std::wstring_view(name).substr(4, end - 4), 10)) perProcess[*pid][instance] += value;
    }
  }
  std::map<std::wstring, double> byType;
  for (const auto& [instance, value] : engineTotals) { const auto type = instance.substr(instance.find(L'|') + 1); auto& slot = byType[type]; slot = std::max(slot, std::min(100.0, value)); gpu.usage = std::max(gpu.usage, std::min(100.0, value)); }
  for (const auto& [type, value] : byType) gpu.engines.emplace_back(type, value);
  // Independent engines can share a type. A process's utilization is its busiest engine,
  // across all GPUs; summing engines of the same type overstates simultaneous activity.
  for (const auto& [pid, instances] : perProcess) for (const auto& [instance, value] : instances) {
    auto& slot = processGpu[pid];
    const double usage = std::min(100.0, value); const auto label = L"GPU " + std::to_wstring(gpu.index) + L" - " + instance.substr(instance.find(L'|') + 1);
    if (usage > 0) slot.engines.push_back({gpu.key + L"|" + instance, label, usage});
    if (usage > slot.value) { slot.value = usage; slot.engine = label; }
  }
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> gpuEngineTests() {
  std::vector<std::pair<std::string, bool>> checks;
  const std::wstring firstKey = L"luid_0x00000000_0x00000001", secondKey = L"luid_0x00000000_0x00000002";
  const auto name = [](DWORD pid, const std::wstring& key, unsigned engine, const wchar_t* type, unsigned physical = 0) { return L"pid_" + std::to_wstring(pid) + L"_" + key + L"_phys_" + std::to_wstring(physical) + L"_eng_" + std::to_wstring(engine) + L"_engtype_" + type; };
  {
    GpuInfo gpu; gpu.key = firstKey;
    std::unordered_map<DWORD, ProcessGpu> processes;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D"), 40}, {name(10, firstKey, 1, L"3D"), 50}};
    aggregateGpuEngines(gpu, engines, processes);
    checks.emplace_back("gpu_process_uses_busiest_engine_of_same_type", processes.at(10).value == 50 && processes.at(10).engine == L"GPU 0 - 3D");
    checks.emplace_back("gpu_adapter_uses_busiest_engine_of_same_type", gpu.usage == 50 && gpu.engines == std::vector<std::pair<std::wstring, double>>{{L"3D", 50}});
    const auto& details = processes.at(10).engines;
    checks.emplace_back("gpu_process_retains_distinct_engine_contributions", details.size() == 2 && details[0].key != details[1].key && details[0].value == 40 && details[1].value == 50);
  }
  {
    GpuInfo gpu; gpu.key = firstKey;
    std::unordered_map<DWORD, ProcessGpu> processes;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D", 0), 40}, {name(10, firstKey, 0, L"3D", 1), 50}};
    aggregateGpuEngines(gpu, engines, processes);
    checks.emplace_back("gpu_process_distinguishes_linked_physical_adapters", processes.at(10).value == 50 && processes.at(10).engine == L"GPU 0 - 3D");
    checks.emplace_back("gpu_adapter_distinguishes_linked_physical_adapters", gpu.usage == 50 && gpu.engines == std::vector<std::pair<std::wstring, double>>{{L"3D", 50}});
    const auto& details = processes.at(10).engines;
    checks.emplace_back("gpu_process_retains_physical_adapter_identity", details.size() == 2 && details[0].key != details[1].key);
  }
  {
    GpuInfo gpu; gpu.key = firstKey;
    std::unordered_map<DWORD, ProcessGpu> processes;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D"), 10}, {name(10, firstKey, 0, L"3D"), 15}, {name(20, firstKey, 0, L"3D"), 35}, {name(10, firstKey, 1, L"Copy"), 45}};
    aggregateGpuEngines(gpu, engines, processes);
    checks.emplace_back("gpu_shared_engine_adds_process_contributions", gpu.usage == 60 && gpu.engines == std::vector<std::pair<std::wstring, double>>{{L"3D", 60}, {L"Copy", 45}});
    checks.emplace_back("gpu_processes_keep_individual_busiest_engine", processes.at(10).value == 45 && processes.at(10).engine == L"GPU 0 - Copy" && processes.at(20).value == 35 && processes.at(20).engine == L"GPU 0 - 3D");
    const auto& first = processes.at(10).engines; const auto& second = processes.at(20).engines;
    checks.emplace_back("gpu_process_contributions_share_actual_engine_identity", first.size() == 2 && second.size() == 1 && first[0].key == second[0].key && first[0].value == 25 && second[0].value == 35);
  }
  {
    GpuInfo first; first.key = firstKey; GpuInfo second; second.key = secondKey; second.index = 1;
    std::unordered_map<DWORD, ProcessGpu> processes;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D"), 40}, {name(10, firstKey, 1, L"3D"), 50}, {name(10, secondKey, 0, L"3D"), 80}};
    aggregateGpuEngines(first, engines, processes); aggregateGpuEngines(second, engines, processes);
    checks.emplace_back("gpu_process_selects_busiest_engine_across_adapters", processes.at(10).value == 80 && processes.at(10).engine == L"GPU 1 - 3D" && first.usage == 50 && second.usage == 80);
    const auto& details = processes.at(10).engines;
    checks.emplace_back("gpu_process_retains_logical_adapter_identity", details.size() == 3 && details[0].key != details[2].key);
  }
  {
    GpuInfo gpu; gpu.key = firstKey;
    std::unordered_map<DWORD, ProcessGpu> processes;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D"), 120}, {name(20, firstKey, 0, L"3D"), 10}, {name(30, firstKey, 1, L"Copy"), 0}};
    aggregateGpuEngines(gpu, engines, processes);
    checks.emplace_back("gpu_usage_capped_at_one_hundred", processes.at(10).value == 100 && gpu.usage == 100 && gpu.engines.front().second == 100);
    checks.emplace_back("gpu_idle_process_has_no_busy_engine", processes.at(30).value == 0 && processes.at(30).engine.empty());
  }
  {
    GpuInfo gpu; gpu.key = firstKey;
    std::unordered_map<DWORD, ProcessGpu> values;
    const std::vector<std::pair<std::wstring, double>> engines{{name(10, firstKey, 0, L"3D"), 25}, {name(10, firstKey, 1, L"Copy"), 45}, {name(20, firstKey, 0, L"3D"), 35}};
    aggregateGpuEngines(gpu, engines, values);
    std::vector<Process> processes(2); processes[0].id = 10; processes[0].created = 100; processes[1].id = 20; processes[1].created = 200;
    ProcessGpuCache cache; cacheProcessGpu(processes, values, cache); applyProcessGpu(processes, cache);
    const auto retained = processes[0].gpuUsage;
    checks.emplace_back("gpu_fresh_sample_owns_engine_contributions", retained && retained->engines.size() == 2 && processes[0].gpu == 45 && processes[0].gpuEngine == L"GPU 0 - Copy" && processes[1].gpu == 35);
    applyProcessGpu(processes, cache);
    checks.emplace_back("gpu_cached_sample_reuses_immutable_contributions", processes[0].gpuUsage == retained);
    processes[0].created = 101; applyProcessGpu(processes, cache);
    checks.emplace_back("gpu_cached_sample_rejects_reused_pid", !processes[0].gpuUsage && processes[0].gpu == 0 && processes[0].gpuEngine.empty() && processes[1].gpu == 35);
    values.clear(); cacheProcessGpu(processes, values, cache); applyProcessGpu(processes, cache);
    checks.emplace_back("gpu_missing_counters_clear_cached_contributions", cache.empty() && !processes[1].gpuUsage && processes[1].gpu == 0 && processes[1].gpuEngine.empty());
    checks.emplace_back("gpu_previous_sample_keeps_owned_contributions", retained && retained->engines.size() == 2 && retained->engines[0].value == 25 && retained->engines[1].value == 45);
  }
  return checks;
}
#endif
std::wstring number(double value, int precision) { wchar_t text[384]{}; swprintf_s(text, L"%.*f", std::clamp(precision, 0, 9), value); return text; }
std::wstring grouped(double value, int precision) {
  auto text = number(value, precision); const auto point = text.find(L'.'); auto end = point == text.npos ? text.size() : point; const size_t start = text[0] == L'-' ? 1 : 0;
  while (end > start + 3) { end -= 3; text.insert(end, 1, L','); }
  return text;
}
std::wstring bytes(double value) {
  static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  unsigned unit = 0;
  while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
  return number(value, unit == 0 ? 0 : 1) + L" " + units[unit];
}
std::wstring bits(double value) {
  value *= 8; if (value < 1000) return number(value, 0) + L" bps";
  static const wchar_t* units[] = {L"Kbps", L"Mbps", L"Gbps"}; unsigned unit = 0; value /= 1000;
  while (value >= 1000 && unit < 2) { value /= 1000; ++unit; }
  return number(value, 1) + L" " + units[unit];
}
std::wstring duration(uint64_t seconds) { wchar_t text[64]{}; swprintf_s(text, L"%llu:%02llu:%02llu:%02llu", seconds / 86400, seconds / 3600 % 24, seconds / 60 % 60, seconds % 60); return text; }
std::wstring winerror(DWORD code) {
  wchar_t* text = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring result = text ? text : L"Windows error " + std::to_wstring(code);
  if (text) LocalFree(text);
  while (!result.empty() && iswspace(result.back())) result.pop_back();
  return result;
}
std::string utf8(const std::wstring& value) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring fromUtf8(std::string_view value) {
  if (value.empty()) return {};
  if (value.size() > size_t(std::numeric_limits<int>::max())) return L"Error message is too long to display.";
  const int length = int(value.size());
  const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(), length, nullptr, 0);
  if (needed <= 0) return L"Unable to decode the error message.";
  std::wstring result(size_t(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, value.data(), length, result.data(), needed) != needed) return L"Unable to decode the error message.";
  return result;
}
std::wstring executable() { std::wstring path(32768, L'\0'); path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()))); return path; }
std::wstring windowsDirectory() { wchar_t path[MAX_PATH]{}; GetWindowsDirectoryW(path, MAX_PATH); return path; }
// Inventory entries may be malformed or refer to missing programs; retain the
// registration even when its executable cannot be extracted safely.
std::wstring commandExecutable(const std::wstring& command) {
  try { return parseRunCommand(command).file; }
  catch (const std::exception&) { return L""; }
}
Counter::Counter() { PdhOpenQueryW(nullptr, 0, &query); }
Counter::~Counter() { if (query) PdhCloseQuery(query); }
void Counter::add(const std::wstring& key, const std::wstring& path) { PDH_HCOUNTER counter = nullptr; if (query && PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS) counters.emplace(key, counter); }
void Counter::collect() { if (query) PdhCollectQueryData(query); }
template <typename Read> static std::vector<std::pair<std::wstring, double>> formattedCounterValues(std::vector<std::byte>& data, Read read) {
  std::vector<std::pair<std::wstring, double>> result;
  constexpr DWORD limit = 16 * 1024 * 1024;
  const auto resize = [&] {
    DWORD size = 0, count = 0;
    if (read(&size, &count, nullptr) != PDH_MORE_DATA || size == 0 || size > limit) return false;
    if (data.size() < size) data.resize(size);
    return true;
  };
  if (data.empty() && !resize()) return result;
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    DWORD size = DWORD(data.size()), count = 0;
    auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(data.data());
    const auto status = read(&size, &count, items);
    if (status == ERROR_SUCCESS) {
      if (size > data.size() || count > size / sizeof(*items)) return result;
      result.reserve(count);
      for (DWORD index = 0; index < count; ++index) {
        const auto& item = items[index];
        if (item.szName && (item.FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || item.FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) && std::isfinite(item.FmtValue.doubleValue))
          result.emplace_back(item.szName, std::max(0.0, item.FmtValue.doubleValue));
      }
      return result;
    }
    // PDH does not guarantee the required size after a failed nonempty-buffer call.
    // Probe again, then retry the read; live wildcard instances can grow in between.
    if ((status != PDH_MORE_DATA && status != PDH_INVALID_ARGUMENT) || attempt == 2 || !resize()) return result;
  }
  return result;
}
std::vector<std::pair<std::wstring, double>> Counter::values(const std::wstring& key) {
  const auto found = counters.find(key);
  if (found == counters.end()) return {};
  // One retained buffer serves every counter; returned names and values own their storage.
  return formattedCounterValues(valuesBuffer, [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W* items) {
    return PdhGetFormattedCounterArrayW(found->second, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, size, count, items);
  });
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> counterSamplingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  std::vector<std::byte> data;
  unsigned probes = 0, reads = 0;
  const auto read = [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W* items) {
    if (!items) { ++probes; *size = sizeof(*items); return LONG(PDH_MORE_DATA); }
    ++reads; *size = sizeof(*items); *count = 1; items[0] = {}; items[0].szName = const_cast<wchar_t*>(L"example"); items[0].FmtValue.CStatus = PDH_CSTATUS_VALID_DATA; items[0].FmtValue.doubleValue = 42;
    return LONG(ERROR_SUCCESS);
  };
  const auto first = formattedCounterValues(data, read); const auto allocation = data.data();
  const auto second = formattedCounterValues(data, read);
  checks.emplace_back("counter_reuses_buffer_and_avoids_repeated_size_probe", probes == 1 && reads == 2 && data.data() == allocation && first == second && second == std::vector<std::pair<std::wstring, double>>{{L"example", 42}});
  {
    unsigned calls = 0;
    const auto grown = formattedCounterValues(data, [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W* items) {
      ++calls;
      if (calls == 1) { *size = UINT32_MAX; return LONG(PDH_MORE_DATA); }
      if (!items) { *size = 2 * sizeof(*items); return LONG(PDH_MORE_DATA); }
      *size = 2 * sizeof(*items); *count = 1; items[0] = {}; items[0].szName = const_cast<wchar_t*>(L"grown"); items[0].FmtValue.CStatus = PDH_CSTATUS_NEW_DATA; items[0].FmtValue.doubleValue = 7;
      return LONG(ERROR_SUCCESS);
    });
    checks.emplace_back("counter_growth_reprobes_unreliable_required_size", calls == 3 && grown == std::vector<std::pair<std::wstring, double>>{{L"grown", 7}});
    checks.emplace_back("counter_reused_buffer_does_not_change_prior_values", first == std::vector<std::pair<std::wstring, double>>{{L"example", 42}});
  }
  {
    unsigned calls = 0;
    const auto recovered = formattedCounterValues(data, [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W* items) {
      if (++calls == 1) return LONG(PDH_INVALID_ARGUMENT);
      return read(size, count, items);
    });
    checks.emplace_back("counter_legacy_small_buffer_error_retries", calls == 3 && recovered == first);
  }
  {
    unsigned calls = 0;
    const auto failed = formattedCounterValues(data, [&](DWORD* size, DWORD*, PDH_FMT_COUNTERVALUE_ITEM_W* items) { ++calls; *size = items ? UINT32_MAX : DWORD(data.size()); return LONG(PDH_MORE_DATA); });
    checks.emplace_back("counter_churning_instances_have_bounded_retries", calls == 5 && failed.empty());
    calls = 0;
    const auto oversized = formattedCounterValues(data, [&](DWORD* size, DWORD*, PDH_FMT_COUNTERVALUE_ITEM_W*) { ++calls; *size = 16 * 1024 * 1024 + 1; return LONG(PDH_MORE_DATA); });
    checks.emplace_back("counter_bounds_required_allocation", calls == 2 && oversized.empty() && data.size() < 16 * 1024 * 1024);
    calls = 0;
    const auto unavailable = formattedCounterValues(data, [&](DWORD*, DWORD*, PDH_FMT_COUNTERVALUE_ITEM_W*) { ++calls; return LONG(PDH_INVALID_HANDLE); });
    checks.emplace_back("counter_unavailable_data_is_not_reused", calls == 1 && unavailable.empty());
  }
  {
    data.resize(6 * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
    const auto filtered = formattedCounterValues(data, [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W* items) {
      *size = DWORD(data.size()); *count = 6;
      for (DWORD index = 0; index < *count; ++index) { items[index] = {}; items[index].szName = const_cast<wchar_t*>(L"finite"); items[index].FmtValue.CStatus = PDH_CSTATUS_VALID_DATA; }
      items[0].FmtValue.doubleValue = -1; items[1].FmtValue.doubleValue = std::numeric_limits<double>::quiet_NaN(); items[2].FmtValue.doubleValue = std::numeric_limits<double>::infinity();
      items[3].FmtValue.CStatus = PDH_CSTATUS_INVALID_DATA; items[4].szName = nullptr; items[5].FmtValue.doubleValue = 150;
      return LONG(ERROR_SUCCESS);
    });
    checks.emplace_back("counter_filters_invalid_values_without_capping_valid_usage", filtered == std::vector<std::pair<std::wstring, double>>{{L"finite", 0}, {L"finite", 150}});
    const auto invalid = formattedCounterValues(data, [&](DWORD* size, DWORD* count, PDH_FMT_COUNTERVALUE_ITEM_W*) { *size = DWORD(data.size()); *count = 7; return LONG(ERROR_SUCCESS); });
    checks.emplace_back("counter_rejects_invalid_array_extent", invalid.empty());
  }
  return checks;
}
#endif
template <typename Read> static std::vector<std::byte> readAdapterAddressBuffer(Read read) {
  ULONG size = 32768; std::vector<std::byte> addresses;
  for (int attempt = 0; attempt < 3; ++attempt) {
    addresses.resize(size);
    const auto code = read(reinterpret_cast<IP_ADAPTER_ADDRESSES*>(addresses.data()), &size);
    if (code == NO_ERROR) return addresses;
    if (code != ERROR_BUFFER_OVERFLOW || size >= 4 * 1024 * 1024) break;
  }
  // Failed calls do not provide a valid linked list, including the final overflow.
  return {};
}
template <typename Query, typename Release> static bool readNetworkInterfaces(Time sampledAt, Time& previousAt,
    std::unordered_map<ULONG64, std::pair<ULONG64, ULONG64>>& previous, const std::unordered_map<ULONG64, std::array<std::wstring, 3>>& addresses,
    std::vector<NetworkInfo>& networks, Query query, Release release) {
  PMIB_IF_TABLE2 interfaces = nullptr;
  if (query(&interfaces) != NO_ERROR) return false;
  const double elapsed = previousAt == Time{} ? 0 : std::chrono::duration<double>(sampledAt - previousAt).count();
  for (ULONG index = 0; index < interfaces->NumEntries; ++index) {
    const auto& adapter = interfaces->Table[index];
    if (adapter.OperStatus != IfOperStatusUp || adapter.Type == IF_TYPE_SOFTWARE_LOOPBACK || !adapter.InterfaceAndOperStatusFlags.HardwareInterface) continue;
    NetworkInfo network; network.luid = adapter.InterfaceLuid.Value; network.alias = adapter.Alias; network.description = adapter.Description; network.speed = std::max(adapter.ReceiveLinkSpeed, adapter.TransmitLinkSpeed);
    network.type = adapter.Type == IF_TYPE_IEEE80211 ? L"Wi-Fi" : adapter.Type == IF_TYPE_ETHERNET_CSMACD ? L"Ethernet" : adapter.Type == IF_TYPE_WWANPP || adapter.Type == IF_TYPE_WWANPP2 ? L"Cellular" : L"Network";
    if (const auto found = addresses.find(network.luid); found != addresses.end()) { if (!found->second[0].empty()) network.description = found->second[0]; network.ipv4 = found->second[1]; network.ipv6 = found->second[2]; }
    auto found = previous.find(network.luid);
    if (found != previous.end() && elapsed > 0) { network.receive = adapter.InOctets >= found->second.first ? double(adapter.InOctets - found->second.first) / elapsed : 0; network.send = adapter.OutOctets >= found->second.second ? double(adapter.OutOctets - found->second.second) / elapsed : 0; }
    previous[network.luid] = {adapter.InOctets, adapter.OutOctets};
    networks.push_back(std::move(network));
  }
  release(interfaces);
  std::erase_if(previous, [&](const auto& item) { return std::none_of(networks.begin(), networks.end(), [&](const auto& network) { return network.luid == item.first; }); });
  // Keep the timestamp with the successful byte baseline, not unrelated resource samples.
  previousAt = sampledAt;
  return true;
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> networkSamplingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  {
    int calls = 0;
    const auto addresses = readAdapterAddressBuffer([&](IP_ADAPTER_ADDRESSES*, ULONG* size) { ++calls; *size += 32768; return ULONG(ERROR_BUFFER_OVERFLOW); });
    checks.emplace_back("network_address_retry_exhaustion_discards_failed_buffer", calls == 3 && addresses.empty());
    calls = 0;
    const auto recovered = readAdapterAddressBuffer([&](IP_ADAPTER_ADDRESSES* buffer, ULONG* size) { if (++calls < 3) { *size += 32768; return ULONG(ERROR_BUFFER_OVERFLOW); } buffer->Luid.Value = 7; buffer->Next = nullptr; return ULONG(NO_ERROR); });
    checks.emplace_back("network_address_retry_recovers_before_limit", calls == 3 && !recovered.empty() && reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(recovered.data())->Luid.Value == 7);
    calls = 0;
    const auto excessive = readAdapterAddressBuffer([&](IP_ADAPTER_ADDRESSES*, ULONG* size) { ++calls; *size = 4 * 1024 * 1024; return ULONG(ERROR_BUFFER_OVERFLOW); });
    checks.emplace_back("network_address_retry_bounds_allocation", calls == 1 && excessive.empty());
  }
  {
    MIB_IF_TABLE2 table{}; table.NumEntries = 1; auto& adapter = table.Table[0];
    adapter.InterfaceLuid.Value = 7; adapter.OperStatus = IfOperStatusUp; adapter.Type = IF_TYPE_ETHERNET_CSMACD; adapter.InterfaceAndOperStatusFlags.HardwareInterface = true;
    adapter.InOctets = 100; adapter.OutOctets = 50;
    std::unordered_map<ULONG64, std::pair<ULONG64, ULONG64>> previous;
    const std::unordered_map<ULONG64, std::array<std::wstring, 3>> addresses;
    Time previousAt{}; const Time initial = Time{} + std::chrono::seconds(1); bool fail = false; int released = 0;
    const auto query = [&](PMIB_IF_TABLE2* output) { if (fail) return ULONG(ERROR_NOT_ENOUGH_MEMORY); *output = &table; return ULONG(NO_ERROR); };
    const auto release = [&](PMIB_IF_TABLE2 output) { if (output == &table) ++released; };
    std::vector<NetworkInfo> networks;
    readNetworkInterfaces(initial, previousAt, previous, addresses, networks, query, release);
    checks.emplace_back("network_first_sample_establishes_baseline", networks.size() == 1 && networks.front().receive == 0 && networks.front().send == 0 && previousAt == initial);
    networks.clear(); fail = true;
    const bool failed = !readNetworkInterfaces(initial + std::chrono::seconds(1), previousAt, previous, addresses, networks, query, release);
    checks.emplace_back("network_failed_sample_preserves_baseline_and_time", failed && networks.empty() && released == 1 && previousAt == initial && previous.at(7) == std::pair<ULONG64, ULONG64>{100, 50});
    fail = false; adapter.InOctets = 300; adapter.OutOctets = 150;
    readNetworkInterfaces(initial + std::chrono::seconds(2), previousAt, previous, addresses, networks, query, release);
    checks.emplace_back("network_recovery_uses_elapsed_since_success", networks.size() == 1 && networks.front().receive == 100 && networks.front().send == 50 && released == 2);
    networks.clear(); adapter.InOctets = 1; adapter.OutOctets = 2;
    readNetworkInterfaces(initial + std::chrono::seconds(3), previousAt, previous, addresses, networks, query, release);
    checks.emplace_back("network_counter_reset_does_not_underflow", networks.size() == 1 && networks.front().receive == 0 && networks.front().send == 0);
    networks.clear(); table.NumEntries = 0;
    readNetworkInterfaces(initial + std::chrono::seconds(4), previousAt, previous, addresses, networks, query, release);
    table.NumEntries = 1; adapter.InOctets = 500; adapter.OutOctets = 250;
    readNetworkInterfaces(initial + std::chrono::seconds(5), previousAt, previous, addresses, networks, query, release);
    checks.emplace_back("network_reappearing_interface_starts_new_baseline", networks.size() == 1 && networks.front().receive == 0 && networks.front().send == 0);
  }
  return checks;
}
#endif
struct UnicodeString { USHORT length, maximum; wchar_t* buffer; };
struct NativeProcess {
  ULONG next, threads;
  int64_t privateWorking;
  ULONG hardFaults, maximumThreads;
  uint64_t cycles, created, user, kernel;
  UnicodeString name;
  LONG priority;
  HANDLE pid, parent;
  ULONG handles, session;
  ULONG_PTR key;
  SIZE_T peakVirtual, virtualSize;
  ULONG faults;
  SIZE_T peakWorking, working, peakPaged, paged, peakNonpaged, nonpaged, pagefile, peakPagefile, privateBytes;
  int64_t reads, writes, others, readBytes, writeBytes, otherBytes;
};
static_assert(sizeof(NativeProcess) == 256);
struct NativeThread { int64_t kernel, user, created; ULONG wait; void* start; HANDLE process, thread; LONG priority, basePriority; ULONG switches, state, reason; };
static_assert(sizeof(NativeThread) == 80);
template <typename Visit> static void visitNativeProcesses(std::span<const std::byte> data, Visit visit) {
  size_t offset = 0;
  for (;;) {
    if (data.size() - offset < sizeof(NativeProcess) || reinterpret_cast<uintptr_t>(data.data() + offset) % alignof(NativeProcess)) throw std::runtime_error("Invalid process inventory extent");
    const auto& native = *reinterpret_cast<const NativeProcess*>(data.data() + offset);
    const size_t extent = native.next ? native.next : data.size() - offset;
    if (extent < sizeof(NativeProcess) || extent > data.size() - offset || (native.next && native.next % alignof(NativeProcess))) throw std::runtime_error("Invalid process inventory link");
    if (native.threads > (extent - sizeof(NativeProcess)) / sizeof(NativeThread)) throw std::runtime_error("Invalid process thread extent");
    std::wstring_view name;
    if (native.name.length) {
      const auto address = reinterpret_cast<uintptr_t>(native.name.buffer), begin = reinterpret_cast<uintptr_t>(data.data());
      if (!native.name.buffer || native.name.length % sizeof(wchar_t) || native.name.maximum < native.name.length || address < begin || address - begin > data.size() || native.name.length > data.size() - (address - begin) || address % alignof(wchar_t))
        throw std::runtime_error("Invalid process name extent");
      name = {native.name.buffer, native.name.length / sizeof(wchar_t)};
    }
    visit(native, std::span<const NativeThread>{reinterpret_cast<const NativeThread*>(data.data() + offset + sizeof(NativeProcess)), native.threads}, name);
    if (!native.next) return;
    offset += native.next;
  }
}
template <typename Previous> static void applyProcessRates(Process& process, const Previous& previous, Time sampledAt, unsigned logical, bool topologyChanged) {
  const double elapsed = previous.sampledAt == Time{} ? 0 : std::chrono::duration<double>(sampledAt - previous.sampledAt).count();
  if (previous.created != process.created || elapsed <= 0) return;
  process.cpu = !topologyChanged && process.cpuTicks >= previous.cpuTicks ? std::clamp(double(process.cpuTicks - previous.cpuTicks) / (elapsed * 100000 * std::max(1u, logical)), 0.0, 100.0) : 0;
  const double readRate = process.readBytes >= previous.readBytes ? double(process.readBytes - previous.readBytes) / elapsed : 0;
  const double writeRate = process.writeBytes >= previous.writeBytes ? double(process.writeBytes - previous.writeBytes) / elapsed : 0;
  process.ioRate = readRate + writeRate;
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> processSamplingTests() {
  std::vector<std::pair<std::string, bool>> checks;
  {
    constexpr size_t firstExtent = sizeof(NativeProcess) + sizeof(NativeThread) + 16;
    std::vector<std::byte> data(firstExtent + sizeof(NativeProcess));
    auto& first = *reinterpret_cast<NativeProcess*>(data.data());
    auto& thread = *reinterpret_cast<NativeThread*>(data.data() + sizeof(NativeProcess));
    first.next = ULONG(firstExtent); first.threads = 1; first.pid = reinterpret_cast<HANDLE>(ULONG_PTR(10)); thread.state = thread.reason = 5;
    const wchar_t name[] = L"app"; auto* nameBuffer = reinterpret_cast<wchar_t*>(data.data() + sizeof(NativeProcess) + sizeof(NativeThread)); memcpy(nameBuffer, name, sizeof(name));
    first.name = {USHORT(3 * sizeof(wchar_t)), USHORT(sizeof(name)), nameBuffer};
    unsigned visited = 0;
    visitNativeProcesses(data, [&](const NativeProcess& process, std::span<const NativeThread> threads, std::wstring_view text) { if (visited++ == 0) checks.emplace_back("process_inventory_preserves_name_and_thread_extent", process.pid == first.pid && threads.size() == 1 && threads[0].state == 5 && text == L"app"); });
    checks.emplace_back("process_inventory_visits_complete_chain", visited == 2);
    const auto rejected = [&](std::span<const std::byte> bytes) { try { visitNativeProcesses(bytes, [](const auto&, auto, auto) {}); return false; } catch (const std::runtime_error&) { return true; } };
    checks.emplace_back("process_inventory_rejects_truncated_returned_length", rejected(std::span<const std::byte>(data).first(data.size() - 1)));
    first.threads = 2;
    checks.emplace_back("process_threads_cannot_cross_next_process_record", rejected(data)); first.threads = 1;
    first.next = ULONG(firstExtent - 1);
    checks.emplace_back("process_inventory_rejects_unaligned_record_link", rejected(data)); first.next = ULONG(firstExtent);
    first.next = ULONG(data.size() + 1);
    checks.emplace_back("process_inventory_rejects_link_beyond_returned_length", rejected(data)); first.next = ULONG(firstExtent);
    first.name.buffer = reinterpret_cast<wchar_t*>(data.data() + data.size());
    checks.emplace_back("process_name_cannot_escape_returned_buffer", rejected(data)); first.name.buffer = nameBuffer;
    ++first.name.length;
    checks.emplace_back("process_name_requires_complete_characters", rejected(data)); --first.name.length;
    first.name.maximum = 0;
    checks.emplace_back("process_name_rejects_inconsistent_length", rejected(data)); first.name.maximum = USHORT(sizeof(name));
    first.name.buffer = nullptr;
    checks.emplace_back("process_name_rejects_missing_buffer", rejected(data));
    checks.emplace_back("process_inventory_rejects_empty_success", rejected({}));
  }
  {
    const Time initial = Time{} + std::chrono::seconds(1);
    struct Baseline { uint64_t created, cpuTicks, readBytes, writeBytes; Time sampledAt; } previous{100, 10000000, 100, 50, initial};
    const auto process = [&] { Process value; value.created = previous.created; value.cpuTicks = previous.cpuTicks + 10000000; value.readBytes = 300; value.writeBytes = 150; return value; };
    auto current = process(); applyProcessRates(current, previous, initial + std::chrono::seconds(2), 4, false);
    checks.emplace_back("process_rates_use_matching_counter_timestamp", current.cpu == 12.5 && current.ioRate == 150);
    // The process baseline may advance even if a later resource query fails.
    previous = {current.created, current.cpuTicks, current.readBytes, current.writeBytes, initial + std::chrono::seconds(2)};
    current = process(); current.readBytes += 300; current.writeBytes += 150; applyProcessRates(current, previous, initial + std::chrono::seconds(3), 4, false);
    checks.emplace_back("process_rates_recover_after_later_stage_failure", current.cpu == 25 && current.ioRate == 450);
    current = process(); ++current.created; applyProcessRates(current, previous, initial + std::chrono::seconds(3), 4, false);
    checks.emplace_back("process_rates_reject_reused_pid", current.cpu == 0 && current.ioRate == 0);
    current = process(); current.cpuTicks = current.readBytes = current.writeBytes = 0; applyProcessRates(current, previous, initial + std::chrono::seconds(3), 4, false);
    checks.emplace_back("process_counter_resets_do_not_underflow", current.cpu == 0 && current.ioRate == 0);
    current = process(); applyProcessRates(current, previous, initial, 4, false);
    checks.emplace_back("process_rates_reject_nonpositive_elapsed", current.cpu == 0 && current.ioRate == 0);
    current = process(); applyProcessRates(current, previous, initial + std::chrono::seconds(3), 4, true);
    checks.emplace_back("process_topology_change_discards_cpu_interval", current.cpu == 0);
    previous.sampledAt = {}; current = process(); applyProcessRates(current, previous, initial, 4, false);
    checks.emplace_back("process_first_sample_starts_new_baseline", current.cpu == 0 && current.ioRate == 0);
  }
  return checks;
}
#endif
struct NativeProcessor { int64_t idle, kernel, user, dpc, interrupt; ULONG interrupts; };
static_assert(sizeof(NativeProcessor) == 48);
using QuerySystem = LONG (WINAPI*)(ULONG, void*, ULONG, ULONG*);
static QuerySystem querySystem() { static auto query = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")); return query; }
using QuerySystemEx = LONG (WINAPI*)(ULONG, void*, ULONG, void*, ULONG, ULONG*);
static QuerySystemEx querySystemEx() { static auto query = reinterpret_cast<QuerySystemEx>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformationEx")); return query; }
static std::optional<unsigned> processorCount(std::span<const unsigned> groups) {
  if (groups.empty() || groups.size() > UINT16_MAX) return std::nullopt;
  constexpr unsigned limit = 64 * 1024 * 1024 / sizeof(NativeProcessor);
  unsigned total = 0;
  for (const auto count : groups) { if (count == 0 || count > 64 || total > limit - count) return std::nullopt; total += count; }
  return total;
}
static std::vector<unsigned> readProcessorTopology() {
  const WORD count = GetActiveProcessorGroupCount();
  std::vector<unsigned> groups; groups.reserve(count);
  for (WORD group = 0; group < count; ++group) groups.push_back(GetActiveProcessorCount(group));
  if (GetActiveProcessorGroupCount() != count || !processorCount(groups)) return {};
  return groups;
}
template <typename Query, typename Topology> static std::optional<std::vector<NativeProcessor>> readProcessorGroups(std::span<const unsigned> groups, Query query, Topology topology) {
  const auto count = processorCount(groups); if (!count) return std::nullopt;
  std::vector<NativeProcessor> processors(*count); size_t offset = 0;
  for (size_t index = 0; index < groups.size(); ++index) {
    const ULONG size = ULONG(groups[index] * sizeof(NativeProcessor)); ULONG returned = 0;
    if (query(WORD(index), processors.data() + offset, size, &returned) < 0 || returned != size) return std::nullopt;
    offset += groups[index];
  }
  const auto current = topology();
  if (current.size() != groups.size() || !std::equal(current.begin(), current.end(), groups.begin())) return std::nullopt;
  return processors;
}
static std::optional<std::vector<NativeProcessor>> readGroupProcessorData(std::span<const unsigned> groups) {
  const auto query = querySystemEx(); if (!query) return std::nullopt;
  // The Ex query selects a group with a USHORT input; a legacy query only reads the calling thread's group.
  return readProcessorGroups(groups, [&](WORD group, NativeProcessor* data, ULONG size, ULONG* returned) { return query(8, &group, sizeof(group), data, size, returned); }, readProcessorTopology);
}
static std::optional<std::array<uint64_t, 2>> processorTotals(std::span<const NativeProcessor> processors) {
  if (processors.empty()) return std::nullopt;
  uint64_t idle = 0, total = 0;
  for (const auto& processor : processors) {
    if (processor.idle < 0 || processor.kernel < 0 || processor.user < 0 || processor.dpc < 0 || processor.interrupt < 0) return std::nullopt;
    const uint64_t currentIdle = uint64_t(processor.idle), currentTotal = uint64_t(processor.kernel) + uint64_t(processor.user);
    if (currentIdle > currentTotal || idle > UINT64_MAX - currentIdle || total > UINT64_MAX - currentTotal) return std::nullopt;
    idle += currentIdle; total += currentTotal;
  }
  return std::array<uint64_t, 2>{idle, total};
}
static double processorUsage(uint64_t currentIdle, uint64_t currentTotal, uint64_t& previousIdle, uint64_t& previousTotal) {
  double usage = 0;
  if (previousTotal && currentTotal > previousTotal && currentIdle >= previousIdle) usage = std::clamp(100.0 * (1.0 - double(currentIdle - previousIdle) / double(currentTotal - previousTotal)), 0.0, 100.0);
  previousIdle = currentIdle; previousTotal = currentTotal;
  return usage;
}
static bool applyProcessorSample(std::optional<std::vector<NativeProcessor>> data, uint64_t& idle, uint64_t& total, double& usage, std::vector<NativeProcessor>& processors) {
  if (!data) return false;
  const auto times = processorTotals(*data); if (!times) return false;
  usage = processorUsage((*times)[0], (*times)[1], idle, total);
  processors = std::move(*data);
  return true;
}
static bool updateProcessorTopology(const std::vector<unsigned>& groups, std::vector<unsigned>& previous, uint64_t& idle, uint64_t& total, double& usage, std::vector<std::array<uint64_t, 3>>& cores) {
  if (groups.empty() || groups == previous) return false;
  previous = groups; idle = total = 0; usage = 0; cores.clear();
  return true;
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> processorGroupTests() {
  std::vector<std::pair<std::string, bool>> checks;
  const std::vector<unsigned> groups{64, 64};
  checks.emplace_back("cpu_counts_all_processor_groups", processorCount(groups) == 128u && 640000000.0 / (100000 * *processorCount(groups)) == 50);
  checks.emplace_back("cpu_rejects_invalid_group_counts", !processorCount({}) && !processorCount(std::vector<unsigned>{64, 0}) && !processorCount(std::vector<unsigned>{UINT32_MAX}) && !processorCount(std::vector<unsigned>(UINT16_MAX + 1u, 1)));
  unsigned epoch = 1; bool fail = false; int extent = 0; std::vector<WORD> visited;
  const auto query = [&](WORD group, NativeProcessor* processors, ULONG size, ULONG* returned) {
    visited.push_back(group);
    if (fail && group == 1) return LONG(-1);
    for (size_t index = 0; index < size / sizeof(NativeProcessor); ++index) processors[index] = {group == 0 ? int64_t(epoch * 1000) : 0, int64_t(epoch * 1000), 0, 0, 0, 0};
    *returned = extent == 1 ? size - 1 : extent == 2 ? size + ULONG(sizeof(NativeProcessor)) : size;
    return LONG(0);
  };
  const auto topology = [&] { return groups; };
  uint64_t idle = 0, total = 0; double usage = 0; std::vector<NativeProcessor> processors;
  const bool initial = applyProcessorSample(readProcessorGroups(groups, query, topology), idle, total, usage, processors);
  checks.emplace_back("cpu_queries_every_group_once", initial && processors.size() == 128 && visited == std::vector<WORD>{0, 1} && processors.front().idle == 1000 && processors.back().idle == 0);
  const auto firstIdle = idle, firstTotal = total;
  fail = true; epoch = 2;
  const bool failed = !applyProcessorSample(readProcessorGroups(groups, query, topology), idle, total, usage, processors);
  checks.emplace_back("cpu_group_failure_preserves_complete_baseline", failed && idle == firstIdle && total == firstTotal && processors.size() == 128);
  fail = false; epoch = 3;
  const bool recovered = applyProcessorSample(readProcessorGroups(groups, query, topology), idle, total, usage, processors);
  checks.emplace_back("cpu_group_recovery_uses_matching_counter_interval", recovered && usage == 50 && idle == 192000 && total == 384000);
  extent = 1; const bool partial = !readProcessorGroups(groups, query, topology); extent = 2;
  checks.emplace_back("cpu_rejects_partial_and_oversized_group_results", partial && !readProcessorGroups(groups, query, topology));
  extent = 0;
  checks.emplace_back("cpu_rejects_topology_change_during_query", !readProcessorGroups(groups, query, [] { return std::vector<unsigned>{64, 32}; }));
  const std::vector<unsigned> changed{64, 32};
  const auto resized = readProcessorGroups(changed, query, [&] { return changed; });
  checks.emplace_back("cpu_hot_added_or_removed_processors_resize_sample", processorCount(changed) == 96u && resized && resized->size() == 96);
  {
    auto previousGroups = groups; auto changedIdle = idle, changedTotal = total; double changedUsage = usage;
    std::vector<std::array<uint64_t, 3>> coreBaseline(128, {100, 50, 10}); std::vector<NativeProcessor> changedProcessors;
    const bool reset = updateProcessorTopology(changed, previousGroups, changedIdle, changedTotal, changedUsage, coreBaseline);
    checks.emplace_back("cpu_topology_change_resets_all_cpu_baselines", reset && previousGroups == changed && changedIdle == 0 && changedTotal == 0 && changedUsage == 0 && coreBaseline.empty());
    const bool established = applyProcessorSample(resized, changedIdle, changedTotal, changedUsage, changedProcessors);
    checks.emplace_back("cpu_new_topology_starts_with_fresh_counter_interval", established && changedUsage == 0 && changedProcessors.size() == 96 && changedTotal == 288000);
    const auto retainedTotal = changedTotal;
    checks.emplace_back("cpu_stable_or_failed_topology_keeps_baseline", !updateProcessorTopology(changed, previousGroups, changedIdle, changedTotal, changedUsage, coreBaseline) && !updateProcessorTopology({}, previousGroups, changedIdle, changedTotal, changedUsage, coreBaseline) && changedTotal == retainedTotal);
  }
  auto invalid = processors; invalid.front().kernel = -1;
  const auto beforeIdle = idle, beforeTotal = total; const auto beforeUsage = usage;
  const bool negative = !applyProcessorSample(invalid, idle, total, usage, processors);
  const std::vector<NativeProcessor> overflow(2, NativeProcessor{0, INT64_MAX, INT64_MAX, 0, 0, 0});
  checks.emplace_back("cpu_rejects_invalid_and_overflowing_totals", negative && !processorTotals(overflow) && idle == beforeIdle && total == beforeTotal && usage == beforeUsage);
  checks.emplace_back("cpu_counter_reset_starts_new_baseline", processorUsage(100, 200, idle, total) == 0 && idle == 100 && total == 200);
  const auto nativeTopology = readProcessorTopology();
  if (querySystemEx() && !nativeTopology.empty()) {
    const auto native = readGroupProcessorData(nativeTopology);
    checks.emplace_back("cpu_native_group_query_contract", native && native->size() == *processorCount(nativeTopology) && processorTotals(*native).has_value());
  }
  return checks;
}
#endif
template <typename ReadGpus> static void refreshHardwareInventory(bool& invalidated, std::unordered_map<int, DiskInfo>& disks, std::vector<GpuInfo>& gpus, ReadGpus read) {
  // Device discovery runs on explicit refresh, independently of periodic counter sampling.
  if (!invalidated) return;
  auto refreshed = read();
  disks.clear(); gpus = std::move(refreshed); invalidated = false;
}
template <typename ReadDisk> static const DiskInfo& cachedDiskHardware(std::unordered_map<int, DiskInfo>& disks, int index, ReadDisk read) {
  const auto found = disks.find(index);
  return found != disks.end() ? found->second : disks.emplace(index, read(index)).first->second;
}
template <typename Read> static void refreshServiceEntries(ServiceInventory& inventory, const ServiceInventory* previous, Read read) {
  auto services = read(&inventory.servicesError);
  if (inventory.servicesError != ERROR_SUCCESS && previous) inventory.services = previous->services;
  else inventory.services = std::move(services);
}
static void pruneDiskHardware(std::unordered_map<int, DiskInfo>& hardware, std::span<const DiskInfo> disks) {
  // Disk snapshots are sorted by physical index before pruning cached metadata.
  std::erase_if(hardware, [&](const auto& item) {
    const auto found = std::lower_bound(disks.begin(), disks.end(), item.first, [](const DiskInfo& disk, int index) { return disk.index < index; });
    return found == disks.end() || found->index != item.first;
  });
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> hardwareInventoryTests() {
  std::vector<std::pair<std::string, bool>> checks;
  const auto check = [&](const char* name, bool pass) { checks.emplace_back(name, pass); };
  GpuInfo firstGpu; firstGpu.key = L"anonymous-gpu-a"; firstGpu.name = L"Example GPU A";
  GpuInfo addedGpu; addedGpu.key = L"anonymous-gpu-b"; addedGpu.name = L"Example GPU B";
  std::unordered_map<int, DiskInfo> disks; std::vector<GpuInfo> gpus{firstGpu}; bool invalidated = false, replacement = false;
  unsigned diskReads = 0, gpuReads = 0;
  const auto readDisk = [&](int index) { ++diskReads; DiskInfo disk; disk.index = index; disk.model = replacement ? L"Example replacement" : L"Example original"; disk.type = replacement ? L"SSD" : L"HDD"; disk.capacity = replacement ? 2000 : 1000; return disk; };
  const auto readGpuFixture = [&] { ++gpuReads; return std::vector<GpuInfo>{firstGpu, addedGpu}; };
  const auto original = cachedDiskHardware(disks, 7, readDisk);
  replacement = true;
  refreshHardwareInventory(invalidated, disks, gpus, readGpuFixture);
  const auto cached = cachedDiskHardware(disks, 7, readDisk);
  check("hardware_ordinary_samples_reuse_inventory", diskReads == 1 && gpuReads == 0 && cached.model == original.model && gpus.size() == 1);
  invalidated = true; refreshHardwareInventory(invalidated, disks, gpus, readGpuFixture);
  check("hardware_explicit_refresh_adds_gpu", !invalidated && gpuReads == 1 && gpus.size() == 2 && gpus.back().key == addedGpu.key);
  check("hardware_explicit_refresh_discards_disk_metadata", disks.empty());
  const auto replaced = cachedDiskHardware(disks, 7, readDisk);
  check("hardware_refresh_replaces_same_disk_index", diskReads == 2 && replaced.index == original.index && replaced.model == L"Example replacement" && replaced.type == L"SSD" && replaced.capacity == 2000);
  refreshHardwareInventory(invalidated, disks, gpus, readGpuFixture); (void)cachedDiskHardware(disks, 7, readDisk);
  check("hardware_refresh_discovery_runs_once", gpuReads == 1 && diskReads == 2);
  const auto retained = cachedDiskHardware(disks, 9, readDisk);
  const std::array<DiskInfo, 1> visible{retained}; pruneDiskHardware(disks, visible);
  check("hardware_prunes_disappeared_disk", !disks.contains(7) && disks.contains(9) && diskReads == 3);
  replacement = false; const auto reappeared = cachedDiskHardware(disks, 7, readDisk);
  check("hardware_reappeared_index_reads_new_metadata", diskReads == 4 && reappeared.model == original.model && reappeared.capacity == original.capacity);
  pruneDiskHardware(disks, {}); check("hardware_empty_disk_snapshot_releases_metadata", disks.empty());
  invalidated = true; refreshHardwareInventory(invalidated, disks, gpus, [&] { ++gpuReads; return std::vector<GpuInfo>{addedGpu}; });
  check("hardware_refresh_removes_departed_gpu", gpuReads == 2 && gpus.size() == 1 && gpus.front().key == addedGpu.key);
  return checks;
}
#endif
Sampler::Sampler(bool attribution, std::function<void()> startupNotification) : buffer(64 * 1024) {
  startupNotify = std::move(startupNotification);
  cpuInfo = readCpu(); memoryHardware = readMemoryHardware(); gpuDescriptors = readGpus();
  processorGroups = readProcessorTopology();
  counters.add(L"idle", L"\\PhysicalDisk(*)\\% Idle Time");
  counters.add(L"diskRead", L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
  counters.add(L"diskWrite", L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
  counters.add(L"diskResponse", L"\\PhysicalDisk(*)\\Avg. Disk sec/Transfer");
  counters.add(L"performance", L"\\Processor Information(_Total)\\% Processor Performance");
  counters.add(L"gpu", L"\\GPU Engine(*)\\Utilization Percentage");
  counters.add(L"gpuDedicated", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
  counters.add(L"gpuShared", L"\\GPU Adapter Memory(*)\\Shared Usage");
  counters.collect();
  if (attribution) trace = std::make_unique<NetworkTrace>();
}
Sampler::~Sampler() = default;
// Account names for every process, including ones this user cannot open.
void Sampler::readUsers() {
  auto users = std::make_shared<std::unordered_map<DWORD, std::wstring>>();
  DWORD level = 1, count = 0; WTS_PROCESS_INFO_EXW* information = nullptr;
  if (WTSEnumerateProcessesExW(WTS_CURRENT_SERVER_HANDLE, &level, WTS_ANY_SESSION, reinterpret_cast<LPWSTR*>(&information), &count)) {
    for (DWORD index = 0; index < count; ++index) {
      const auto sid = information[index].pUserSid; if (!sid) continue;
      wchar_t* text = nullptr; if (!ConvertSidToStringSidW(sid, &text)) continue; std::wstring key = text; LocalFree(text);
      auto found = accounts.find(key);
      if (found == accounts.end()) { wchar_t name[256]{}, domain[256]{}; DWORD nameSize = 256, domainSize = 256; SID_NAME_USE use{}; found = accounts.emplace(key, LookupAccountSidW(nullptr, sid, name, &nameSize, domain, &domainSize, &use) ? std::wstring(name) : key).first; }
      users->emplace(information[index].ProcessId, found->second);
    }
    WTSFreeMemoryExW(WTSTypeProcessInfoLevel1, information, count);
  }
  cachedUsers = std::move(users);
}
Sample Sampler::sample(bool startupVisible) {
  const auto started = Clock::now();
#ifdef TASKMGR_DIAGNOSTICS
  auto phase = started;
#endif
  const auto topology = readProcessorTopology();
  const bool topologyChanged = updateProcessorTopology(topology, processorGroups, idle, total, cpuUsage, corePrevious);
  if (topologyChanged) { cpuInfo = readCpu(); cachedResources.cores.clear(); cachedResources.interrupts = 0; }
  if (const auto count = processorCount(topology)) cpuInfo.logical = *count;
  Sample result; result.cpuInfo = cpuInfo;
  const bool refreshResources = topologyChanged || resourcesAt == Time{} || started - resourcesAt >= std::chrono::seconds(1);
#ifdef TASKMGR_DIAGNOSTICS
  result.resourcesCheckedAt = started;
  result.cpuTopologyChanged = topologyChanged;
#endif
  if (!refreshResources) {
    result.cpuSpeed = cachedResources.cpuSpeed; result.interrupts = cachedResources.interrupts;
    result.memory = cachedResources.memory; result.cores = cachedResources.cores;
    result.disks = cachedResources.disks; result.networks = cachedResources.networks; result.gpus = cachedResources.gpus;
  }
  result.memory.compressed = 0;
  result.elapsed = last == Time{} ? 0 : std::chrono::duration<double>(started - last).count();
  const auto query = querySystem();
  if (!query) throw std::runtime_error("Native process query unavailable");
  ULONG required = 0; LONG status = 0;
  for (;;) {
    status = query(5, buffer.data(), ULONG(buffer.size()), &required);
    if (status != LONG(0xc0000004)) break;
    if (required > 64 * 1024 * 1024 || buffer.size() >= 64 * 1024 * 1024) throw std::runtime_error("Process inventory exceeds safety bound");
    buffer.resize(std::min<size_t>(64 * 1024 * 1024, std::max<size_t>(required + 65536, buffer.size() * 2)));
  }
  if (status < 0) throw std::runtime_error("Native process query failed");
  if (required > buffer.size()) throw std::runtime_error("Invalid process inventory size");
  const auto processesSampledAt = Clock::now();
  ++generation; result.processes.reserve(previous.size() + 64);
  const unsigned logical = std::max(1u, cpuInfo.logical);
  visitNativeProcesses(std::span<const std::byte>(buffer).first(required), [&](const NativeProcess& native, std::span<const NativeThread> threads, std::wstring_view name) {
    Process process; process.id = DWORD(reinterpret_cast<ULONG_PTR>(native.pid)); process.parent = DWORD(reinterpret_cast<ULONG_PTR>(native.parent));
    process.session = native.session; process.created = native.created; process.cpuTicks = native.user + native.kernel; process.priority = native.priority;
    process.threads = native.threads; process.handles = native.handles; process.working = native.working; process.privateBytes = native.privateBytes; process.peakWorking = native.peakWorking; process.pageFaults = native.faults;
    process.privateWorking = uint64_t(std::max<int64_t>(0, native.privateWorking));
    process.readBytes = uint64_t(std::max<int64_t>(0, native.readBytes)); process.writeBytes = uint64_t(std::max<int64_t>(0, native.writeBytes));
    process.name = !name.empty() ? std::wstring(name) : process.id == 0 ? L"System Idle Process" : L"System";
    // A process whose every thread waits in the Suspended state is shown as suspended, as UWP apps in the background are.
    process.suspended = process.id > 4 && !threads.empty() && std::all_of(threads.begin(), threads.end(), [](const NativeThread& thread) { return thread.state == 5 && thread.reason == 5; });
    result.threads += native.threads; result.handles += native.handles;
    auto [found, inserted] = previous.try_emplace(process.id);
    if (!inserted) applyProcessRates(process, found->second, processesSampledAt, logical, topologyChanged);
    if (process.name == L"Memory Compression") result.memory.compressed = process.working;
    found->second = {process.created, process.cpuTicks, process.readBytes, process.writeBytes, generation, processesSampledAt}; result.processes.push_back(std::move(process));
  });
  struct Windows { std::unordered_set<DWORD> apps, hung; std::vector<AppWindow> list; } visible;
  EnumWindows([](HWND window, LPARAM context) -> BOOL {
    if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr && GetWindowTextLengthW(window) > 0 && !(GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) {
      BOOL cloaked = FALSE; using Attribute = HRESULT (WINAPI*)(HWND, DWORD, void*, DWORD); static auto attribute = reinterpret_cast<Attribute>(GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmGetWindowAttribute"));
      if (attribute && SUCCEEDED(attribute(window, 14, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
      DWORD pid = 0; GetWindowThreadProcessId(window, &pid); auto& state = *reinterpret_cast<Windows*>(context); state.apps.insert(pid); if (IsHungAppWindow(window)) state.hung.insert(pid);
      wchar_t title[512]{}; InternalGetWindowText(window, title, 512); if (state.list.size() < 4096) state.list.push_back({window, pid, title});
    }
    return TRUE;
  }, reinterpret_cast<LPARAM>(&visible));
  for (auto& process : result.processes) { process.app = visible.apps.contains(process.id); process.hung = visible.hung.contains(process.id); }
  result.windows = std::move(visible.list);
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[0] = milliseconds(phase); phase = Clock::now();
#endif
  std::vector<NativeProcessor> processors;
  if (topology.size() > 1) {
    applyProcessorSample(readGroupProcessorData(topology), idle, total, cpuUsage, processors);
    // Keep the last complete sample if an Ex query is unavailable or a group fails;
    // primary-group totals must never stand in for whole-machine activity.
  } else if (topology.size() == 1) {
    FILETIME currentIdle{}, kernel{}, user{};
    if (GetSystemTimes(&currentIdle, &kernel, &user)) cpuUsage = processorUsage(ticks(currentIdle), ticks(kernel) + ticks(user), idle, total);
  }
  result.cpu = cpuUsage;
  if (refreshResources) {
  if (topology.size() == 1) {
    processors.resize(logical); ULONG returned = 0; const ULONG size = ULONG(processors.size() * sizeof(NativeProcessor));
    if (query(8, processors.data(), size, &returned) < 0 || returned != size || !processorTotals(processors)) processors.clear();
  }
  result.cores = cachedResources.cores; result.interrupts = cachedResources.interrupts;
  if (!processors.empty()) {
    corePrevious.resize(processors.size()); result.cores.assign(processors.size(), 0); result.interrupts = 0;
    double interruptTicks = 0, totalTicks = 0;
    for (size_t index = 0; index < processors.size(); ++index) {
      const auto& core = processors[index]; auto& old = corePrevious[index];
      const uint64_t busy = uint64_t(core.kernel) + uint64_t(core.user), idleTime = uint64_t(core.idle), extra = uint64_t(core.dpc) + uint64_t(core.interrupt);
      if (old[0] && busy > old[0] && idleTime >= old[1]) { result.cores[index] = std::clamp(100.0 * (1.0 - double(idleTime - old[1]) / double(busy - old[0])), 0.0, 100.0); interruptTicks += double(extra - std::min(extra, old[2])); totalTicks += double(busy - old[0]); }
      old = {busy, idleTime, extra};
    }
    if (totalTicks > 0) result.interrupts = std::clamp(100.0 * interruptTicks / totalTicks, 0.0, 100.0);
  }
  PERFORMANCE_INFORMATION performance{sizeof(performance)};
  const auto compressed = result.memory.compressed; result.memory = memoryHardware; result.memory.compressed = compressed;
  if (GetPerformanceInfo(&performance, sizeof(performance))) {
    const double page = double(performance.PageSize); result.memory.committed = uint64_t(double(performance.CommitTotal) * page); result.memory.commitLimit = uint64_t(double(performance.CommitLimit) * page);
    result.memory.cached = uint64_t(double(performance.SystemCache) * page); result.memory.paged = uint64_t(double(performance.KernelPaged) * page); result.memory.nonpaged = uint64_t(double(performance.KernelNonpaged) * page);
  }
  }
  MEMORYSTATUSEX memory{sizeof(memory)};
  if (GlobalMemoryStatusEx(&memory)) { result.memory.total = memory.ullTotalPhys; result.memory.available = memory.ullAvailPhys; }
  result.uptime = GetTickCount64() / 1000;
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[1] = milliseconds(phase); phase = Clock::now();
#endif
  if (refreshResources) {
  refreshHardwareInventory(hardwareInvalidated, diskHardware, gpuDescriptors, readGpus);
  counters.collect();
  for (const auto& [name, value] : counters.values(L"performance")) { (void)name; result.cpuSpeed = cpuInfo.baseMhz * value / 100 / 1000; }
  if (result.cpuSpeed <= 0) result.cpuSpeed = cpuInfo.baseMhz / 1000;
  std::unordered_map<std::wstring, double> diskRead, diskWrite, diskResponse;
  for (const auto& [name, value] : counters.values(L"diskRead")) diskRead[name] = value;
  for (const auto& [name, value] : counters.values(L"diskWrite")) diskWrite[name] = value;
  for (const auto& [name, value] : counters.values(L"diskResponse")) diskResponse[name] = value * 1000;
  wchar_t systemDrive[4] = L"C:"; { const auto windows = windowsDirectory(); if (windows.size() >= 2) systemDrive[0] = windows[0]; }
  for (const auto& [name, value] : counters.values(L"idle")) {
    if (name == L"_Total") continue;
    DiskInfo disk; disk.key = name; disk.index = _wtoi(name.c_str());
    const auto& hardware = cachedDiskHardware(diskHardware, disk.index, readDiskHardware);
    disk.model = hardware.model; disk.type = hardware.type; disk.capacity = hardware.capacity;
    const auto space = name.find(L' '); disk.letters = space == name.npos ? L"" : name.substr(space + 1);
    disk.title = L"Disk " + std::to_wstring(disk.index) + (disk.letters.empty() ? L"" : L" (" + disk.letters + L")");
    for (size_t position = 0; position + 1 < disk.letters.size(); ++position) if (disk.letters[position + 1] == L':') {
      const std::wstring root{disk.letters[position], L':', L'\\'}; ULARGE_INTEGER totalBytes{};
      if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &totalBytes, nullptr)) disk.formatted += totalBytes.QuadPart;
      if (towupper(disk.letters[position]) == towupper(systemDrive[0])) disk.system = true;
      if (GetFileAttributesW((root + L"pagefile.sys").c_str()) != INVALID_FILE_ATTRIBUTES) disk.pagefile = true;
    }
    disk.active = std::clamp(100 - value, 0.0, 100.0); disk.read = diskRead[name]; disk.write = diskWrite[name]; disk.response = diskResponse[name];
    result.disks.push_back(std::move(disk));
  }
  std::sort(result.disks.begin(), result.disks.end(), [](const DiskInfo& left, const DiskInfo& right) { return left.index < right.index; });
  pruneDiskHardware(diskHardware, result.disks);
  }
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[2] = milliseconds(phase); phase = Clock::now();
#endif
  const auto inventoryCheckedAt = Clock::now();
  const bool refreshInventory = inventoryAt == Time{} || inventoryCheckedAt - inventoryAt >= std::chrono::seconds(5);
  if (refreshInventory) {
    adapterAddresses.clear();
    const auto addresses = readAdapterAddressBuffer([](IP_ADAPTER_ADDRESSES* buffer, ULONG* size) { return GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, buffer, size); });
    for (auto adapter = addresses.empty() ? nullptr : reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(addresses.data()); adapter; adapter = adapter->Next) {
      std::array<std::wstring, 3> entry{adapter->Description ? adapter->Description : L"", L"", L""};
      for (auto unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
        wchar_t text[64]{}; DWORD length = 64; const auto family = unicast->Address.lpSockaddr->sa_family;
        if (WSAAddressToStringW(unicast->Address.lpSockaddr, unicast->Address.iSockaddrLength, nullptr, text, &length) != 0) continue;
        std::wstring value = text; if (const auto percent = value.find(L'%'); percent != value.npos) value.resize(percent);
        if (family == AF_INET && entry[1].empty()) entry[1] = value; else if (family == AF_INET6 && entry[2].empty()) entry[2] = value;
      }
      adapterAddresses[adapter->Luid.Value] = std::move(entry);
    }
  }
  if (refreshResources) {
  // Slow disk/address discovery above must not become part of another byte interval.
  readNetworkInterfaces(Clock::now(), networkAt, networkPrevious, adapterAddresses, result.networks, GetIfTable2, FreeMibTable);
  }
  if (trace) result.networkAttribution = trace->sample(result.processes);
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[3] = milliseconds(phase); phase = Clock::now();
#endif
  if (refreshResources) {
  const auto engines = counters.values(L"gpu"), dedicated = counters.values(L"gpuDedicated"), shared = counters.values(L"gpuShared");
  std::unordered_map<DWORD, ProcessGpu> processGpu;
  for (auto gpu : gpuDescriptors) {
    aggregateGpuEngines(gpu, engines, processGpu);
    for (const auto& [name, value] : dedicated) if (name.find(gpu.key) != std::wstring::npos) gpu.dedicated += uint64_t(value);
    for (const auto& [name, value] : shared) if (name.find(gpu.key) != std::wstring::npos) gpu.shared += uint64_t(value);
    gpu.temperature = gpuTemperature(gpu.key);
    result.gpus.push_back(std::move(gpu));
  }
  cacheProcessGpu(result.processes, processGpu, cachedProcessGpu);
  cachedResources.cpuSpeed = result.cpuSpeed; cachedResources.interrupts = result.interrupts; cachedResources.memory = result.memory;
  cachedResources.cores = result.cores; cachedResources.disks = result.disks; cachedResources.networks = result.networks; cachedResources.gpus = result.gpus;
  resourcesAt = started;
  cachedResources.resourcesSampledAt = started;
  }
  applyProcessGpu(result.processes, cachedProcessGpu);
  result.resourcesSampledAt = resourcesAt;
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[4] = milliseconds(phase); phase = Clock::now();
#endif
  if (refreshInventory) { auto inventory = std::make_shared<ServiceInventory>(); refreshServiceEntries(*inventory, cachedInventory.get(), readServices); inventory->sessions = readSessions(); cachedInventory = std::move(inventory); readUsers(); inventoryAt = Clock::now(); }
  if (startupProgressReady.load()) { std::lock_guard lock(startupMutex); if (startupProgressReady.exchange(false)) cachedStartup = std::move(startupProgress); }
  if (startupJob.valid() && startupJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    try { cachedStartup = std::make_shared<StartupInventory>(startupJob.get()); }
    catch (...) { auto failed = std::make_shared<StartupInventory>(); failed->warnings.push_back(L"Startup inventory failed; refresh to retry."); Entry entry; entry.key = L"coverage:failed"; entry.cells = {failed->warnings.front(), L"", L"Unavailable", L"—", L"Coverage", L"", L"", L"", failed->warnings.front()}; failed->entries.push_back(std::move(entry)); cachedStartup = std::move(failed); }
    startupAt = Clock::now();
  }
  if (startupVisible && !startupJob.valid() && (startupInvalidated || startupAt == Time{} || milliseconds(startupAt) >= 60000)) {
    startupInvalidated = false;
    startupJob = std::async(std::launch::async, [this] { const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED); StartupInventory result; try { result = readStartup([this](const StartupInventory& inventory) { { std::lock_guard lock(startupMutex); startupProgress = std::make_shared<StartupInventory>(inventory); startupProgressReady = true; } if (startupNotify) startupNotify(); }); } catch (...) { if (SUCCEEDED(initialized)) CoUninitialize(); throw; } if (SUCCEEDED(initialized)) CoUninitialize(); return result; });
  }
  result.inventory = cachedInventory; result.services = result.inventory->services; result.sessions = result.inventory->sessions; result.startup = cachedStartup; result.users = cachedUsers;
#ifdef TASKMGR_DIAGNOSTICS
  result.inventoryCheckedAt = inventoryCheckedAt; result.inventorySampledAt = inventoryAt;
  result.stages[5] = milliseconds(phase);
  result.duration = milliseconds(started);
#endif
  std::erase_if(previous, [&](const auto& item) { return item.second.seen != generation; }); last = started; return result;
}
std::wstring serviceGroupCaption(const std::wstring& group) {
  static const std::unordered_map<std::wstring, std::wstring> captions{{L"netsvcs", L"Local System"}, {L"localsystemnetworkrestricted", L"Local System (Network Restricted)"}, {L"localservice", L"Local Service"},
    {L"localservicenetworkrestricted", L"Local Service (Network Restricted)"}, {L"localservicenonetwork", L"Local Service (No Network)"}, {L"localservicenonetworkfirewall", L"Local Service (No Network Firewall)"},
    {L"localserviceandnoimpersonation", L"Local Service (No Impersonation)"}, {L"networkservice", L"Network Service"}, {L"networkservicenetworkrestricted", L"Network Service (Network Restricted)"},
    {L"networkserviceandnoimpersonation", L"Network Service (No Impersonation)"}, {L"rpcss", L"Remote Procedure Call"}, {L"dcomlaunch", L"DCOM Server Process Launcher"}, {L"unistacksvcgroup", L"Unistack Service Group"},
    {L"wbiosvcgroup", L"Windows Biometric"}, {L"wersvcgroup", L"Windows Error Reporting"}, {L"print", L"Print Workflow"}, {L"appmodel", L"appmodel"}, {L"utcsvc", L"Connected User Experiences and Telemetry"}};
  const auto found = captions.find(lower(group)); return found == captions.end() ? group : found->second;
}
// The svchost group ("-k netsvcs") from a service's image path; blank for services in their own process.
static std::wstring serviceGroup(const std::wstring& service) {
  static std::unordered_map<std::wstring, std::wstring> cache; static std::mutex gate; std::lock_guard lock(gate);
  if (const auto found = cache.find(service); found != cache.end()) return found->second;
  wchar_t image[4096]{}; DWORD size = sizeof(image); std::wstring group;
  if (RegGetValueW(HKEY_LOCAL_MACHINE, (L"SYSTEM\\CurrentControlSet\\Services\\" + service).c_str(), L"ImagePath", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, image, &size) == ERROR_SUCCESS) {
    const auto text = lower(image); const auto marker = text.find(L" -k "); if (marker != text.npos) { group = std::wstring(image).substr(marker + 4); group = group.substr(0, group.find(L' ')); }
  }
  if (cache.size() < 4096) cache.emplace(service, group);
  return group;
}
struct ServiceEnumeration { std::vector<Entry> entries; DWORD error = ERROR_SUCCESS; };
template <typename Read, typename Group> static ServiceEnumeration enumerateServices(Read read, Group group) {
  constexpr DWORD bufferLimit = 256 * 1024, entryLimit = 65536;
  std::vector<BYTE> data(32768); ServiceEnumeration result; DWORD resume = 0;
  for (unsigned page = 0; page < 256; ++page) {
    DWORD needed = 0, count = 0; const DWORD previous = resume;
    const auto status = read(data.data(), DWORD(data.size()), &needed, &count, &resume);
    if (status != ERROR_SUCCESS && status != ERROR_MORE_DATA) return {{}, status};
    if (count > data.size() / sizeof(ENUM_SERVICE_STATUS_PROCESSW)) return {{}, ERROR_INVALID_DATA};
    if (count > entryLimit - result.entries.size()) return {{}, ERROR_MORE_DATA};
    if (result.entries.empty()) result.entries.reserve(count);
    const auto records = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(data.data());
    static const wchar_t* states[] = {L"Unknown", L"Stopped", L"Starting", L"Stopping", L"Running", L"Continuing", L"Pausing", L"Paused"};
    for (DWORD index = 0; index < count; ++index) {
      const auto& service = records[index];
      if (!service.lpServiceName || !*service.lpServiceName || !service.lpDisplayName) return {{}, ERROR_INVALID_DATA};
      const DWORD state = service.ServiceStatusProcess.dwCurrentState, pid = state == SERVICE_STOPPED ? 0 : service.ServiceStatusProcess.dwProcessId;
      Entry entry{service.lpServiceName, {service.lpServiceName, pid ? std::to_wstring(pid) : L"", service.lpDisplayName, states[state <= 7 ? state : 0], L""}, pid};
      entry.group = group(service.lpServiceName); entry.cells[4] = entry.group; entry.description = service.lpDisplayName;
      result.entries.push_back(std::move(entry));
    }
    if (status == ERROR_SUCCESS) {
      std::sort(result.entries.begin(), result.entries.end(), [](const Entry& left, const Entry& right) { return CompareStringOrdinal(left.key.c_str(), -1, right.key.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
      // Service creation/deletion during paging can repeat a name at a resume boundary.
      result.entries.erase(std::unique(result.entries.begin(), result.entries.end(), [](const Entry& left, const Entry& right) { return CompareStringOrdinal(left.key.c_str(), -1, right.key.c_str(), -1, TRUE) == CSTR_EQUAL; }), result.entries.end());
      return result;
    }
    if (count && (resume == 0 || resume == previous)) return {{}, ERROR_INVALID_DATA};
    const size_t nextSize = std::min<size_t>(bufferLimit, std::max<size_t>(needed, data.size()));
    if (!count && nextSize <= data.size()) return {{}, ERROR_MORE_DATA};
    if (nextSize > data.size()) data.resize(nextSize);
  }
  return {{}, ERROR_MORE_DATA};
}
std::vector<Entry> readServices(DWORD* error) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
  if (!manager) { if (error) *error = GetLastError(); return {}; }
  struct Close { SC_HANDLE handle; ~Close() { CloseServiceHandle(handle); } } close{manager};
  auto result = enumerateServices([&](BYTE* data, DWORD size, DWORD* needed, DWORD* count, DWORD* resume) {
    return EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, data, size, needed, count, resume, nullptr) ? DWORD(ERROR_SUCCESS) : GetLastError();
  }, serviceGroup);
  if (error) *error = result.error;
  return std::move(result.entries);
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> serviceInventoryTests() {
  std::vector<std::pair<std::string, bool>> checks;
  {
    ServiceInventory previous; Entry service; service.key = L"existing"; previous.services.push_back(service);
    ServiceInventory unavailable;
    const auto fail = [](DWORD* error) { *error = ERROR_ACCESS_DENIED; return std::vector<Entry>{}; };
    refreshServiceEntries(unavailable, &previous, fail);
    checks.emplace_back("services_refresh_failure_retains_last_complete_inventory", unavailable.servicesError == ERROR_ACCESS_DENIED && unavailable.services.size() == 1 && unavailable.services[0].key == L"existing" && previous.servicesError == ERROR_SUCCESS);
    ServiceInventory initial; refreshServiceEntries(initial, nullptr, fail);
    checks.emplace_back("services_initial_failure_is_distinct_from_empty_success", initial.services.empty() && initial.servicesError == ERROR_ACCESS_DENIED);
    ServiceInventory recovered; refreshServiceEntries(recovered, &unavailable, [](DWORD* error) { *error = ERROR_SUCCESS; return std::vector<Entry>{}; });
    checks.emplace_back("services_successful_empty_snapshot_clears_stale_inventory", recovered.services.empty() && recovered.servicesError == ERROR_SUCCESS && unavailable.services.size() == 1);
  }
  const auto group = [](const wchar_t*) { return std::wstring(L"fixture-group"); };
  const auto record = [](BYTE* data, const wchar_t* name, DWORD state = SERVICE_RUNNING, DWORD pid = 42) {
    auto& service = *reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(data); service = {};
    service.lpServiceName = const_cast<wchar_t*>(name); service.lpDisplayName = const_cast<wchar_t*>(L"Example service"); service.ServiceStatusProcess.dwCurrentState = state; service.ServiceStatusProcess.dwProcessId = pid;
  };
  {
    unsigned calls = 0;
    const auto complete = enumerateServices([&](BYTE* data, DWORD size, DWORD*, DWORD* count, DWORD* resume) { ++calls; checks.emplace_back("services_initial_buffer_is_bounded", size == 32768 && *resume == 0); record(data, L"alpha", SERVICE_STOPPED); *count = 1; return DWORD(ERROR_SUCCESS); }, group);
    checks.emplace_back("services_single_page_avoids_size_probe", calls == 1 && complete.error == ERROR_SUCCESS && complete.entries.size() == 1 && complete.entries[0].key == L"alpha" && complete.entries[0].pid == 0 && complete.entries[0].cells[3] == L"Stopped" && complete.entries[0].group == L"fixture-group");
  }
  {
    unsigned calls = 0; bool resumed = true, bounded = true;
    const auto partial = enumerateServices([&](BYTE* data, DWORD size, DWORD* needed, DWORD* count, DWORD* resume) {
      bounded = bounded && size <= 256 * 1024; ++calls; *count = 1;
      if (calls == 1) { record(data, L"zeta"); *needed = 1024 * 1024; *resume = 7; return DWORD(ERROR_MORE_DATA); }
      resumed = resumed && *resume == 7; record(data, L"alpha"); *resume = 0; return DWORD(ERROR_SUCCESS);
    }, group);
    checks.emplace_back("services_more_data_preserves_partial_pages_and_resume", calls == 2 && resumed && partial.error == ERROR_SUCCESS && partial.entries.size() == 2 && partial.entries[0].key == L"alpha" && partial.entries[1].key == L"zeta");
    checks.emplace_back("services_large_inventory_respects_native_buffer_cap", bounded);
  }
  {
    unsigned calls = 0;
    const auto churn = enumerateServices([&](BYTE* data, DWORD, DWORD* needed, DWORD* count, DWORD* resume) {
      ++calls; record(data, calls == 1 ? L"Example" : L"example"); *count = 1;
      if (calls == 1) { *resume = 1; *needed = 100; return DWORD(ERROR_MORE_DATA); }
      *resume = 0; return DWORD(ERROR_SUCCESS);
    }, group);
    checks.emplace_back("services_paging_churn_deduplicates_case_insensitive_names", calls == 2 && churn.error == ERROR_SUCCESS && churn.entries.size() == 1);
  }
  {
    unsigned calls = 0;
    const auto grown = enumerateServices([&](BYTE* data, DWORD size, DWORD* needed, DWORD* count, DWORD* resume) {
      ++calls; if (calls == 1) { *count = 0; *needed = 65536; return DWORD(ERROR_MORE_DATA); }
      checks.emplace_back("services_empty_page_growth_keeps_resume", size == 65536 && *resume == 0); record(data, L"alpha"); *count = 1; return DWORD(ERROR_SUCCESS);
    }, group);
    checks.emplace_back("services_initial_buffer_race_recovers", calls == 2 && grown.error == ERROR_SUCCESS && grown.entries.size() == 1);
    calls = 0;
    const auto bounded = enumerateServices([&](BYTE*, DWORD, DWORD* needed, DWORD* count, DWORD*) { ++calls; *needed = UINT32_MAX; *count = 0; return DWORD(ERROR_MORE_DATA); }, group);
    checks.emplace_back("services_unserviceable_record_has_bounded_retries", calls == 2 && bounded.error == ERROR_MORE_DATA && bounded.entries.empty());
  }
  {
    unsigned calls = 0;
    const auto failed = enumerateServices([&](BYTE* data, DWORD, DWORD* needed, DWORD* count, DWORD* resume) {
      if (++calls == 1) { record(data, L"alpha"); *count = 1; *resume = 1; *needed = 100; return DWORD(ERROR_MORE_DATA); }
      return DWORD(ERROR_ACCESS_DENIED);
    }, group);
    checks.emplace_back("services_failure_reports_error_without_partial_inventory", calls == 2 && failed.error == ERROR_ACCESS_DENIED && failed.entries.empty());
    calls = 0;
    const auto stuck = enumerateServices([&](BYTE* data, DWORD, DWORD*, DWORD* count, DWORD*) { ++calls; record(data, L"alpha"); *count = 1; return DWORD(ERROR_MORE_DATA); }, group);
    checks.emplace_back("services_nonadvancing_resume_is_rejected", calls == 1 && stuck.error == ERROR_INVALID_DATA && stuck.entries.empty());
    calls = 0;
    const auto endless = enumerateServices([&](BYTE* data, DWORD, DWORD*, DWORD* count, DWORD* resume) { ++calls; record(data, L"alpha"); *count = 1; ++*resume; return DWORD(ERROR_MORE_DATA); }, group);
    checks.emplace_back("services_endless_paging_is_bounded", calls == 256 && endless.error == ERROR_MORE_DATA && endless.entries.empty());
    const auto invalid = enumerateServices([](BYTE*, DWORD size, DWORD*, DWORD* count, DWORD*) { *count = size; return DWORD(ERROR_SUCCESS); }, group);
    checks.emplace_back("services_invalid_array_extent_is_rejected", invalid.error == ERROR_INVALID_DATA && invalid.entries.empty());
  }
  return checks;
}
#endif
std::vector<Entry> readSessions() {
  std::vector<Entry> result; PWTS_SESSION_INFOW sessions = nullptr; DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return result;
  for (DWORD index = 0; index < count; ++index) {
    wchar_t* user = nullptr; DWORD size = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[index].SessionId, WTSUserName, &user, &size)) {
      if (user && *user) {
        Entry entry; entry.key = L"session:" + std::to_wstring(sessions[index].SessionId); entry.session = sessions[index].SessionId;
        entry.cells = {user, sessions[index].State == WTSActive ? L"" : L"Disconnected", std::to_wstring(sessions[index].SessionId), sessions[index].pWinStationName ? sessions[index].pWinStationName : L""};
        wchar_t* domain = nullptr; DWORD domainSize = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, entry.session, WTSDomainName, &domain, &domainSize)) { if (domain) entry.accountDomain = domain; WTSFreeMemory(domain); }
        // Enrichment runs with inventory sampling, before the UI receives this immutable entry.
        entry.description = fullAccountName(entry.cells[0], entry.accountDomain);
        result.push_back(std::move(entry));
      }
      WTSFreeMemory(user);
    }
  }
  WTSFreeMemory(sessions); return result;
}
bool sameSessionAccount(const Entry& expected, const Entry& current) {
  return expected.session == current.session && !expected.cells.empty() && !current.cells.empty() && !expected.cells[0].empty() && !expected.accountDomain.empty() && !current.accountDomain.empty() &&
    CompareStringOrdinal(expected.cells[0].c_str(), -1, current.cells[0].c_str(), -1, TRUE) == CSTR_EQUAL && CompareStringOrdinal(expected.accountDomain.c_str(), -1, current.accountDomain.c_str(), -1, TRUE) == CSTR_EQUAL;
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> sessionIdentityTests() {
  std::vector<std::pair<std::string, bool>> checks;
  Entry expected; expected.session = 7; expected.cells = {L"alex"}; expected.accountDomain = L"DESKTOP"; auto current = expected;
  checks.emplace_back("session_identity_accepts_same_account", sameSessionAccount(expected, current));
  current.accountDomain = L"CORPORATE";
  checks.emplace_back("session_identity_rejects_same_username_in_other_domain", !sameSessionAccount(expected, current));
  current = expected; current.accountDomain = L"desktop"; current.cells[0] = L"Alex";
  checks.emplace_back("session_identity_comparison_ignores_case", sameSessionAccount(expected, current));
  current = expected; current.session = 8;
  checks.emplace_back("session_identity_rejects_other_session", !sameSessionAccount(expected, current));
  current = expected; current.cells[0] = L"other";
  checks.emplace_back("session_identity_rejects_other_username", !sameSessionAccount(expected, current));
  current = expected; current.accountDomain.clear();
  checks.emplace_back("session_identity_rejects_missing_domain", !sameSessionAccount(expected, current) && !sameSessionAccount(current, current));
  current = expected; current.cells.clear();
  checks.emplace_back("session_identity_rejects_missing_username", !sameSessionAccount(expected, current));
  return checks;
}
#endif
}
