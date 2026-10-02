#include "core.hpp"
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
std::wstring executable() { std::wstring path(32768, L'\0'); path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()))); return path; }
std::wstring windowsDirectory() { wchar_t path[MAX_PATH]{}; GetWindowsDirectoryW(path, MAX_PATH); return path; }
// The program a command line starts: quoted or up to the first space, with environment variables expanded.
std::wstring commandExecutable(const std::wstring& command) {
  std::wstring expanded(32768, L'\0'); expanded.resize(std::max<DWORD>(1, ExpandEnvironmentStringsW(command.c_str(), expanded.data(), DWORD(expanded.size()))) - 1);
  while (!expanded.empty() && iswspace(expanded.front())) expanded.erase(expanded.begin());
  if (expanded.empty()) return L"";
  if (expanded.front() == L'"') { const auto close = expanded.find(L'"', 1); return expanded.substr(1, close == expanded.npos ? expanded.npos : close - 1); }
  for (size_t space = expanded.find(L' '); space != expanded.npos; space = expanded.find(L' ', space + 1)) { auto candidate = expanded.substr(0, space); if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate; if (GetFileAttributesW((candidate + L".exe").c_str()) != INVALID_FILE_ATTRIBUTES) return candidate + L".exe"; }
  return expanded.substr(0, expanded.find(L' '));
}
Counter::Counter() { PdhOpenQueryW(nullptr, 0, &query); }
Counter::~Counter() { if (query) PdhCloseQuery(query); }
void Counter::add(const std::wstring& key, const std::wstring& path) { PDH_HCOUNTER counter = nullptr; if (query && PdhAddEnglishCounterW(query, path.c_str(), 0, &counter) == ERROR_SUCCESS) counters.emplace(key, counter); }
void Counter::collect() { if (query) PdhCollectQueryData(query); }
std::vector<std::pair<std::wstring, double>> Counter::values(const std::wstring& key) const {
  std::vector<std::pair<std::wstring, double>> result;
  auto found = counters.find(key);
  if (found == counters.end()) return result;
  DWORD size = 0, count = 0;
  if (PdhGetFormattedCounterArrayW(found->second, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA || size > 16 * 1024 * 1024) return result;
  std::vector<std::byte> data(size);
  auto items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(data.data());
  if (PdhGetFormattedCounterArrayW(found->second, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) return result;
  result.reserve(count);
  for (DWORD index = 0; index < count; ++index) if (items[index].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[index].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) result.emplace_back(items[index].szName, std::max(0.0, items[index].FmtValue.doubleValue));
  return result;
}
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
struct NativeProcessor { int64_t idle, kernel, user, dpc, interrupt; ULONG interrupts; };
static_assert(sizeof(NativeProcessor) == 48);
using QuerySystem = LONG (WINAPI*)(ULONG, void*, ULONG, ULONG*);
static QuerySystem querySystem() { static auto query = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")); return query; }
Sampler::Sampler(bool attribution, std::function<void()> startupNotification) : buffer(64 * 1024) {
  startupNotify = std::move(startupNotification);
  cpuInfo = readCpu(); memoryHardware = readMemoryHardware(); gpuDescriptors = readGpus();
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
  Sample result; result.cpuInfo = cpuInfo;
  const bool refreshResources = resourcesAt == Time{} || started - resourcesAt >= std::chrono::seconds(1);
  const double resourceElapsed = cachedResources.resourcesSampledAt == Time{} ? 0 : std::chrono::duration<double>(started - cachedResources.resourcesSampledAt).count();
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
  ++generation; result.processes.reserve(previous.size() + 64);
  const unsigned logical = std::max(1u, cpuInfo.logical);
  size_t offset = 0;
  for (;;) {
    if (offset + sizeof(NativeProcess) > buffer.size()) throw std::runtime_error("Invalid process inventory extent");
    const auto& native = *reinterpret_cast<const NativeProcess*>(buffer.data() + offset);
    Process process; process.id = DWORD(reinterpret_cast<ULONG_PTR>(native.pid)); process.parent = DWORD(reinterpret_cast<ULONG_PTR>(native.parent));
    process.session = native.session; process.created = native.created; process.cpuTicks = native.user + native.kernel; process.priority = native.priority;
    process.threads = native.threads; process.handles = native.handles; process.working = native.working; process.privateBytes = native.privateBytes; process.peakWorking = native.peakWorking; process.pageFaults = native.faults;
    process.privateWorking = uint64_t(std::max<int64_t>(0, native.privateWorking));
    process.readBytes = uint64_t(std::max<int64_t>(0, native.readBytes)); process.writeBytes = uint64_t(std::max<int64_t>(0, native.writeBytes));
    process.name = native.name.buffer && native.name.length ? std::wstring(native.name.buffer, native.name.length / sizeof(wchar_t)) : process.id == 0 ? L"System Idle Process" : L"System";
    // A process whose every thread waits in the Suspended state is shown as suspended, as UWP apps in the background are.
    if (native.threads && offset + sizeof(NativeProcess) + size_t(native.threads) * sizeof(NativeThread) <= buffer.size()) {
      const auto threads = reinterpret_cast<const NativeThread*>(buffer.data() + offset + sizeof(NativeProcess)); bool suspended = process.id > 4;
      for (ULONG index = 0; index < native.threads && suspended; ++index) suspended = threads[index].state == 5 && threads[index].reason == 5;
      process.suspended = suspended;
    }
    result.threads += native.threads; result.handles += native.handles;
    auto [found, inserted] = previous.try_emplace(process.id);
    if (!inserted && found->second.created == process.created && result.elapsed > 0) {
      const auto& old = found->second;
      process.cpu = process.cpuTicks >= old.cpuTicks ? std::clamp(double(process.cpuTicks - old.cpuTicks) / (result.elapsed * 100000 * logical), 0.0, 100.0) : 0;
      const double readRate = process.readBytes >= old.readBytes ? double(process.readBytes - old.readBytes) / result.elapsed : 0;
      const double writeRate = process.writeBytes >= old.writeBytes ? double(process.writeBytes - old.writeBytes) / result.elapsed : 0;
      process.ioRate = readRate + writeRate;
    }
    if (process.name == L"Memory Compression") result.memory.compressed = process.working;
    found->second = {process.created, process.cpuTicks, process.readBytes, process.writeBytes, generation}; result.processes.push_back(std::move(process));
    if (!native.next) break;
    if (native.next < sizeof(NativeProcess) || native.next > buffer.size() - offset) throw std::runtime_error("Invalid process inventory link");
    offset += native.next;
  }
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
  FILETIME currentIdle{}, kernel{}, user{};
  if (GetSystemTimes(&currentIdle, &kernel, &user)) {
    const uint64_t currentTotal = ticks(kernel) + ticks(user), idleTicks = ticks(currentIdle);
    if (total && currentTotal > total && idleTicks >= idle) result.cpu = std::clamp(100.0 * (1.0 - double(idleTicks - idle) / double(currentTotal - total)), 0.0, 100.0);
    total = currentTotal; idle = idleTicks;
  }
  if (refreshResources) {
  std::vector<NativeProcessor> processors(logical); ULONG returned = 0;
  if (query(8, processors.data(), ULONG(processors.size() * sizeof(NativeProcessor)), &returned) >= 0) {
    processors.resize(returned / sizeof(NativeProcessor)); corePrevious.resize(processors.size()); result.cores.resize(processors.size());
    double interruptTicks = 0, totalTicks = 0;
    for (size_t index = 0; index < processors.size(); ++index) {
      const auto& core = processors[index]; auto& old = corePrevious[index];
      const uint64_t busy = uint64_t(core.kernel + core.user), idleTime = uint64_t(core.idle), extra = uint64_t(core.dpc + core.interrupt);
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
    auto hardware = diskHardware.find(disk.index); if (hardware == diskHardware.end()) hardware = diskHardware.emplace(disk.index, readDiskHardware(disk.index)).first;
    disk.model = hardware->second.model; disk.type = hardware->second.type; disk.capacity = hardware->second.capacity;
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
  }
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[2] = milliseconds(phase); phase = Clock::now();
#endif
  const bool refreshInventory = inventoryAt == Time{} || milliseconds(inventoryAt) >= 5000;
  if (refreshInventory) {
    adapterAddresses.clear(); ULONG size = 32768; std::vector<std::byte> addresses;
    for (int attempt = 0; attempt < 3; ++attempt) { addresses.resize(size); const auto code = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(addresses.data()), &size); if (code == ERROR_BUFFER_OVERFLOW && size < 4 * 1024 * 1024) continue; if (code != NO_ERROR) addresses.clear(); break; }
    for (auto adapter = addresses.empty() ? nullptr : reinterpret_cast<IP_ADAPTER_ADDRESSES*>(addresses.data()); adapter; adapter = adapter->Next) {
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
  PMIB_IF_TABLE2 interfaces = nullptr;
  if (GetIfTable2(&interfaces) == NO_ERROR) {
    for (ULONG index = 0; index < interfaces->NumEntries; ++index) {
      const auto& adapter = interfaces->Table[index];
      if (adapter.OperStatus != IfOperStatusUp || adapter.Type == IF_TYPE_SOFTWARE_LOOPBACK || !adapter.InterfaceAndOperStatusFlags.HardwareInterface) continue;
      NetworkInfo network; network.luid = adapter.InterfaceLuid.Value; network.alias = adapter.Alias; network.description = adapter.Description; network.speed = std::max(adapter.ReceiveLinkSpeed, adapter.TransmitLinkSpeed);
      network.type = adapter.Type == IF_TYPE_IEEE80211 ? L"Wi-Fi" : adapter.Type == IF_TYPE_ETHERNET_CSMACD ? L"Ethernet" : adapter.Type == IF_TYPE_WWANPP || adapter.Type == IF_TYPE_WWANPP2 ? L"Cellular" : L"Network";
      if (const auto found = adapterAddresses.find(network.luid); found != adapterAddresses.end()) { if (!found->second[0].empty()) network.description = found->second[0]; network.ipv4 = found->second[1]; network.ipv6 = found->second[2]; }
      auto found = networkPrevious.find(network.luid);
      if (found != networkPrevious.end() && resourceElapsed > 0) { network.receive = adapter.InOctets >= found->second.first ? double(adapter.InOctets - found->second.first) / resourceElapsed : 0; network.send = adapter.OutOctets >= found->second.second ? double(adapter.OutOctets - found->second.second) / resourceElapsed : 0; }
      networkPrevious[network.luid] = {adapter.InOctets, adapter.OutOctets};
      result.networks.push_back(std::move(network));
    }
    FreeMibTable(interfaces);
    std::erase_if(networkPrevious, [&](const auto& item) { return std::none_of(result.networks.begin(), result.networks.end(), [&](const auto& network) { return network.luid == item.first; }); });
  }
  }
  if (trace && trace->active()) { result.networkAttribution = true; const auto rates = trace->rates(result.elapsed); for (auto& process : result.processes) { const auto found = rates.find(process.id); process.network = found == rates.end() ? 0 : found->second; } }
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[3] = milliseconds(phase); phase = Clock::now();
#endif
  if (refreshResources) {
  const auto engines = counters.values(L"gpu"), dedicated = counters.values(L"gpuDedicated"), shared = counters.values(L"gpuShared");
  struct ProcessGpu { double value = 0; std::wstring engine; };
  std::unordered_map<DWORD, ProcessGpu> processGpu;
  for (auto gpu : gpuDescriptors) {
    std::map<std::wstring, double> engineTotals; std::unordered_map<DWORD, std::map<std::wstring, double>> perProcess;
    for (const auto& [name, value] : engines) {
      if (name.find(gpu.key) == std::wstring::npos) continue;
      const auto engine = name.find(L"_engtype_"); if (engine == std::wstring::npos) continue;
      const auto engineNumber = name.find(L"_eng_"); const auto type = name.substr(engine + 9); const auto instance = (engineNumber == std::wstring::npos ? L"" : name.substr(engineNumber + 5, engine - engineNumber - 5)) + L"|" + type;
      engineTotals[instance] += value;
      if (name.starts_with(L"pid_")) {
        const auto end = name.find(L'_', 4);
        if (end != name.npos) if (const auto pid = counterInteger(std::wstring_view(name).substr(4, end - 4), 10)) perProcess[*pid][type] += value;
      }
    }
    std::map<std::wstring, double> byType;
    for (const auto& [instance, value] : engineTotals) { const auto type = instance.substr(instance.find(L'|') + 1); auto& slot = byType[type]; slot = std::max(slot, std::min(100.0, value)); gpu.usage = std::max(gpu.usage, std::min(100.0, value)); }
    for (const auto& [type, value] : byType) gpu.engines.emplace_back(type, value);
    for (const auto& [pid, types] : perProcess) for (const auto& [type, value] : types) { auto& slot = processGpu[pid]; if (value > slot.value) { slot.value = std::min(100.0, value); slot.engine = L"GPU " + std::to_wstring(gpu.index) + L" - " + type; } }
    for (const auto& [name, value] : dedicated) if (name.find(gpu.key) != std::wstring::npos) gpu.dedicated += uint64_t(value);
    for (const auto& [name, value] : shared) if (name.find(gpu.key) != std::wstring::npos) gpu.shared += uint64_t(value);
    gpu.temperature = gpuTemperature(gpu.key);
    result.gpus.push_back(std::move(gpu));
  }
  for (auto& process : result.processes) if (auto found = processGpu.find(process.id); found != processGpu.end()) { process.gpu = found->second.value; process.gpuEngine = found->second.engine; }
  cachedProcessGpu.clear();
  for (const auto& process : result.processes) if (process.gpu > 0 || !process.gpuEngine.empty()) cachedProcessGpu.emplace(Identity{process.id, process.created}, std::pair{process.gpu, process.gpuEngine});
  cachedResources.cpuSpeed = result.cpuSpeed; cachedResources.interrupts = result.interrupts; cachedResources.memory = result.memory;
  cachedResources.cores = result.cores; cachedResources.disks = result.disks; cachedResources.networks = result.networks; cachedResources.gpus = result.gpus;
  resourcesAt = started;
  cachedResources.resourcesSampledAt = started;
  } else {
    for (auto& process : result.processes) if (auto found = cachedProcessGpu.find(Identity{process.id, process.created}); found != cachedProcessGpu.end()) { process.gpu = found->second.first; process.gpuEngine = found->second.second; }
  }
  result.resourcesSampledAt = resourcesAt;
#ifdef TASKMGR_DIAGNOSTICS
  result.stages[4] = milliseconds(phase); phase = Clock::now();
#endif
  if (refreshInventory) { cachedInventory = std::make_shared<const ServiceInventory>(ServiceInventory{readServices(), readSessions()}); readUsers(); inventoryAt = Clock::now(); }
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
std::vector<Entry> readServices() {
  std::vector<Entry> result;
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
  if (!manager) return result;
  DWORD needed = 0, count = 0, resume = 0;
  EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, nullptr, 0, &needed, &count, &resume, nullptr);
  if (needed && needed <= 16 * 1024 * 1024) {
    std::vector<BYTE> data(needed); resume = 0;
    if (EnumServicesStatusExW(manager, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, data.data(), DWORD(data.size()), &needed, &count, &resume, nullptr)) {
      auto records = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(data.data()); result.reserve(count);
      static const wchar_t* states[] = {L"Unknown", L"Stopped", L"Starting", L"Stopping", L"Running", L"Continuing", L"Pausing", L"Paused"};
      for (DWORD index = 0; index < count; ++index) {
        const auto& service = records[index]; const DWORD state = service.ServiceStatusProcess.dwCurrentState; const DWORD pid = state == SERVICE_STOPPED ? 0 : service.ServiceStatusProcess.dwProcessId;
        Entry entry{service.lpServiceName, {service.lpServiceName, pid ? std::to_wstring(pid) : L"", service.lpDisplayName, states[state <= 7 ? state : 0], L""}, pid};
        entry.group = serviceGroup(service.lpServiceName); entry.cells[4] = entry.group; entry.description = service.lpDisplayName;
        result.push_back(std::move(entry));
      }
    }
  }
  CloseServiceHandle(manager);
  std::sort(result.begin(), result.end(), [](const Entry& left, const Entry& right) { return CompareStringOrdinal(left.key.c_str(), -1, right.key.c_str(), -1, TRUE) == CSTR_LESS_THAN; });
  return result;
}
std::vector<Entry> readSessions() {
  std::vector<Entry> result; PWTS_SESSION_INFOW sessions = nullptr; DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return result;
  for (DWORD index = 0; index < count; ++index) {
    wchar_t* user = nullptr; DWORD size = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[index].SessionId, WTSUserName, &user, &size)) {
      if (user && *user) { Entry entry; entry.key = L"session:" + std::to_wstring(sessions[index].SessionId); entry.session = sessions[index].SessionId; entry.cells = {user, sessions[index].State == WTSActive ? L"" : L"Disconnected", std::to_wstring(sessions[index].SessionId), sessions[index].pWinStationName ? sessions[index].pWinStationName : L""}; result.push_back(std::move(entry)); }
      WTSFreeMemory(user);
    }
  }
  WTSFreeMemory(sessions); return result;
}
}
