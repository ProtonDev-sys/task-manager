#include "core.hpp"
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <dxgi.h>
#include <shlobj.h>
#include <iomanip>

namespace taskmgr {
std::wstring number(double value, int precision) { std::wostringstream stream; stream << std::fixed << std::setprecision(precision) << value; return stream.str(); }
std::wstring bytes(double value) {
  static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  unsigned unit = 0;
  while (value >= 1024 && unit < 4) { value /= 1024; ++unit; }
  return number(value, unit == 0 ? 0 : 1) + L" " + units[unit];
}
std::wstring winerror(DWORD code) {
  wchar_t* text = nullptr;
  FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
  std::wstring result = text ? text : L"Windows error " + std::to_wstring(code);
  if (text) LocalFree(text);
  return result;
}
std::string utf8(const std::wstring& value) {
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
  std::string result(size_t(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), size, nullptr, nullptr);
  return result;
}
std::wstring executable() { std::wstring path(32768, L'\0'); path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()))); return path; }
bool matches(const Process& process, const Metadata& metadata, const std::wstring& query) {
  return contains(process.name, query) || contains(metadata.description, query) || contains(metadata.publisher, query) || contains(std::to_wstring(process.id), query);
}
int findNext(std::span<const std::wstring> names, std::wstring prefix, int start) {
  if (names.empty() || prefix.empty()) return -1;
  prefix = lower(std::move(prefix));
  const int count = int(names.size());
  for (int offset = 0; offset < count; ++offset) {
    const int index = ((start % count + count) % count + offset) % count;
    if (lower(names[size_t(index)]).starts_with(prefix)) return index;
  }
  return -1;
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
using QuerySystem = LONG (WINAPI*)(ULONG, void*, ULONG, ULONG*);
Sampler::Sampler() : buffer(1024 * 1024) {
  SYSTEM_INFO system{}; GetNativeSystemInfo(&system); logical = std::max(1UL, system.dwNumberOfProcessors);
  wchar_t name[512]{}; DWORD size = sizeof(name);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS) cpuName = name;
  counters.add(L"disk", L"\\PhysicalDisk(*)\\% Disk Time");
  counters.add(L"diskRead", L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
  counters.add(L"diskWrite", L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
  counters.add(L"gpu", L"\\GPU Engine(*)\\Utilization Percentage");
  counters.add(L"gpuMemory", L"\\GPU Adapter Memory(*)\\Dedicated Usage");
  IDXGIFactory1* factory = nullptr;
  if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
    for (UINT index = 0; index < 32; ++index) {
      IDXGIAdapter1* adapter = nullptr; if (factory->EnumAdapters1(index, &adapter) != S_OK) break;
      DXGI_ADAPTER_DESC1 description{};
      if (SUCCEEDED(adapter->GetDesc1(&description)) && !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
        wchar_t key[64]{}; swprintf_s(key, L"luid_0x%08x_0x%08x", unsigned(description.AdapterLuid.HighPart), description.AdapterLuid.LowPart);
        gpuDescriptors.push_back({key, L"GPU " + std::to_wstring(gpuDescriptors.size()), description.Description, L"%", 0, 100, double(description.DedicatedVideoMemory), RGB(0, 120, 215)});
      }
      adapter->Release();
    }
    factory->Release();
  }
  counters.collect();
}
Sample Sampler::sample() {
  const auto started = Clock::now();
  auto phase = started;
  Sample result; result.timestamp = started; result.logical = logical; result.cpuName = cpuName;
  result.elapsed = last == Time{} ? 0 : std::chrono::duration<double>(started - last).count();
  static auto query = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) throw std::runtime_error("Native process query unavailable");
  ULONG required = 0; LONG status = 0;
  for (;;) {
    status = query(5, buffer.data(), ULONG(buffer.size()), &required);
    if (status != LONG(0xc0000004)) break;
    if (required > 64 * 1024 * 1024 || buffer.size() >= 64 * 1024 * 1024) throw std::runtime_error("Process inventory exceeds safety bound");
    buffer.resize(std::min<size_t>(64 * 1024 * 1024, std::max<size_t>(required + 65536, buffer.size() * 2)));
  }
  if (status < 0) throw std::runtime_error("Native process query failed");
  std::unordered_map<DWORD, Process> next; next.reserve(previous.size() + 64);
  size_t offset = 0;
  for (;;) {
    if (offset + sizeof(NativeProcess) > buffer.size()) throw std::runtime_error("Invalid process inventory extent");
    const auto& native = *reinterpret_cast<const NativeProcess*>(buffer.data() + offset);
    Process process; process.id = DWORD(reinterpret_cast<ULONG_PTR>(native.pid)); process.parent = DWORD(reinterpret_cast<ULONG_PTR>(native.parent));
    process.session = native.session; process.created = native.created; process.cpuTicks = native.user + native.kernel;
    process.threads = native.threads; process.handles = native.handles; process.working = native.working; process.privateBytes = native.privateBytes;
    process.ioBytes = uint64_t(std::max<int64_t>(0, native.readBytes)) + uint64_t(std::max<int64_t>(0, native.writeBytes));
    process.name = native.name.buffer && native.name.length ? std::wstring(native.name.buffer, native.name.length / sizeof(wchar_t)) : process.id == 0 ? L"System Idle Process" : L"System";
    auto found = previous.find(process.id);
    if (found != previous.end() && found->second.created == process.created && result.elapsed > 0) {
      const auto& old = found->second;
      process.cpu = process.cpuTicks >= old.cpuTicks ? std::clamp(double(process.cpuTicks - old.cpuTicks) / (result.elapsed * 100000 * logical), 0.0, 100.0) : 0;
      process.ioRate = process.ioBytes >= old.ioBytes ? double(process.ioBytes - old.ioBytes) / result.elapsed : 0;
    }
    next.emplace(process.id, process); result.processes.push_back(std::move(process));
    if (!native.next) break;
    if (native.next < sizeof(NativeProcess) || native.next > buffer.size() - offset) throw std::runtime_error("Invalid process inventory link");
    offset += native.next;
  }
  std::unordered_set<DWORD> applications;
  EnumWindows([](HWND window, LPARAM context) -> BOOL { if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr && GetWindowTextLengthW(window) > 0) { DWORD pid = 0; GetWindowThreadProcessId(window, &pid); reinterpret_cast<std::unordered_set<DWORD>*>(context)->insert(pid); } return TRUE; }, reinterpret_cast<LPARAM>(&applications));
  for (auto& process : result.processes) process.app = applications.contains(process.id);
  result.stages[0] = milliseconds(phase); phase = Clock::now();
  FILETIME currentIdle{}, kernel{}, user{};
  if (GetSystemTimes(&currentIdle, &kernel, &user)) {
    const uint64_t currentTotal = ticks(kernel) + ticks(user), idleTicks = ticks(currentIdle);
    if (total && currentTotal > total && idleTicks >= idle) result.cpu = std::clamp(100.0 * (1.0 - double(idleTicks - idle) / double(currentTotal - total)), 0.0, 100.0);
    total = currentTotal; idle = idleTicks;
  }
  MEMORYSTATUSEX memory{sizeof(memory)};
  if (GlobalMemoryStatusEx(&memory)) { result.memoryTotal = memory.ullTotalPhys; result.memoryUsed = memory.ullTotalPhys - memory.ullAvailPhys; }
  result.memory.cb = sizeof(result.memory); GetPerformanceInfo(&result.memory, sizeof(result.memory));
  result.resources.push_back({L"cpu", L"CPU", cpuName, L"%", result.cpu, 100, 0, RGB(0, 120, 215)});
  result.resources.push_back({L"memory", L"Memory", bytes(double(result.memoryUsed)) + L" / " + bytes(double(result.memoryTotal)), L"%", result.memoryTotal ? 100.0 * double(result.memoryUsed) / double(result.memoryTotal) : 0, 100, 0, RGB(139, 18, 174)});
  result.stages[1] = milliseconds(phase); phase = Clock::now();
  counters.collect();
  std::unordered_map<std::wstring, double> diskRead, diskWrite;
  for (const auto& [name, value] : counters.values(L"diskRead")) diskRead[name] = value;
  for (const auto& [name, value] : counters.values(L"diskWrite")) diskWrite[name] = value;
  for (const auto& [name, value] : counters.values(L"disk")) if (name != L"_Total") result.resources.push_back({L"disk/" + name, L"Disk " + name, L"Read " + bytes(diskRead[name]) + L"/s   Write " + bytes(diskWrite[name]) + L"/s", L"%", std::min(100.0, value), 100, 0, RGB(74, 158, 12)});
  result.stages[2] = milliseconds(phase); phase = Clock::now();
  PMIB_IF_TABLE2 interfaces = nullptr;
  if (GetIfTable2(&interfaces) == NO_ERROR) {
    for (ULONG index = 0; index < interfaces->NumEntries; ++index) {
      const auto& adapter = interfaces->Table[index];
      if (adapter.OperStatus != IfOperStatusUp || adapter.Type == IF_TYPE_SOFTWARE_LOOPBACK || !adapter.InterfaceAndOperStatusFlags.HardwareInterface) continue;
      const auto key = adapter.InterfaceLuid.Value;
      double receive = 0, send = 0;
      auto found = networkPrevious.find(key);
      if (found != networkPrevious.end() && result.elapsed > 0) { receive = adapter.InOctets >= found->second.first ? double(adapter.InOctets - found->second.first) / result.elapsed : 0; send = adapter.OutOctets >= found->second.second ? double(adapter.OutOctets - found->second.second) / result.elapsed : 0; }
      networkPrevious[key] = {adapter.InOctets, adapter.OutOctets};
      result.resources.push_back({L"net/" + std::to_wstring(key), adapter.Alias, L"Send " + bytes(send) + L"/s   Receive " + bytes(receive) + L"/s", L"bytes/s", receive + send, std::max(1024.0, (receive + send) * 1.2), 0, RGB(184, 86, 0)});
    }
    FreeMibTable(interfaces);
  }
  result.stages[3] = milliseconds(phase); phase = Clock::now();
  std::unordered_map<DWORD, double> processGpu;
  auto engines = counters.values(L"gpu");
  auto gpuMemory = counters.values(L"gpuMemory");
  for (auto resource : gpuDescriptors) {
    std::unordered_map<std::wstring, double> totals;
    for (const auto& [name, value] : engines) if (name.find(resource.key) != std::wstring::npos) {
      auto engine = name.find(L"_eng_"); if (engine != std::wstring::npos) totals[name.substr(engine)] += value;
      DWORD pid = 0; if (swscanf_s(name.c_str(), L"pid_%lu_", &pid) == 1) processGpu[pid] = std::max(processGpu[pid], std::min(100.0, value));
    }
    for (const auto& [name, value] : totals) { (void)name; resource.value = std::max(resource.value, std::min(100.0, value)); }
    double dedicated = 0; for (const auto& [name, value] : gpuMemory) if (name.find(resource.key) != std::wstring::npos) dedicated += value;
    resource.detail += L"   " + bytes(dedicated) + L" / " + bytes(resource.secondary); result.resources.push_back(std::move(resource));
  }
  for (auto& process : result.processes) if (auto found = processGpu.find(process.id); found != processGpu.end()) process.gpu = found->second;
  result.stages[4] = milliseconds(phase); phase = Clock::now();
  if (inventoryAt == Time{} || milliseconds(inventoryAt) >= 5000) { cachedServices = readServices(); cachedSessions = readSessions(); cachedStartup = readStartup(); inventoryAt = Clock::now(); }
  result.services = cachedServices; result.sessions = cachedSessions; result.startup = cachedStartup;
  result.stages[5] = milliseconds(phase);
  previous = std::move(next); last = started; result.duration = milliseconds(started); return result;
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
      for (DWORD index = 0; index < count; ++index) {
        const auto& service = records[index]; const DWORD state = service.ServiceStatusProcess.dwCurrentState;
        result.push_back({service.lpServiceName, {service.lpServiceName, service.ServiceStatusProcess.dwProcessId ? std::to_wstring(service.ServiceStatusProcess.dwProcessId) : L"", service.lpDisplayName, state == SERVICE_RUNNING ? L"Running" : state == SERVICE_STOPPED ? L"Stopped" : L"Pending"}, service.ServiceStatusProcess.dwProcessId});
      }
    }
  }
  CloseServiceHandle(manager); return result;
}
std::vector<Entry> readSessions() {
  std::vector<Entry> result; PWTS_SESSION_INFOW sessions = nullptr; DWORD count = 0;
  if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return result;
  for (DWORD index = 0; index < count; ++index) {
    wchar_t* user = nullptr; DWORD size = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[index].SessionId, WTSUserName, &user, &size)) {
      if (user && *user) { Entry entry; entry.key = std::to_wstring(sessions[index].SessionId); entry.session = sessions[index].SessionId; entry.cells = {user, entry.key, sessions[index].State == WTSActive ? L"Active" : L"Disconnected", L"", L""}; result.push_back(std::move(entry)); }
      WTSFreeMemory(user);
    }
  }
  WTSFreeMemory(sessions); return result;
}
std::vector<Entry> readStartup() {
  std::vector<Entry> result;
  for (bool machine : {false, true}) {
    HKEY key = nullptr; const auto root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    const wchar_t* run = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    if (RegOpenKeyExW(root, run, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
    for (DWORD index = 0;; ++index) {
      wchar_t name[512]{}, command[32768]{}; DWORD nameSize = DWORD(std::size(name)), size = sizeof(command), type = 0;
      auto status = RegEnumValueW(key, index, name, &nameSize, nullptr, &type, reinterpret_cast<BYTE*>(command), &size);
      if (status == ERROR_NO_MORE_ITEMS) break;
      if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) continue;
      BYTE approval[12]{}; DWORD approvalSize = sizeof(approval);
      RegGetValueW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run", name, RRF_RT_REG_BINARY, nullptr, approval, &approvalSize);
      Entry entry; entry.key = (machine ? L"HKLM/" : L"HKCU/") + std::wstring(name); entry.enabled = approval[0] != 3 && approval[0] != 7;
      entry.cells = {name, L"—", entry.enabled ? L"Enabled" : L"Disabled", L"Not measured"}; entry.path = command; entry.location = machine ? L"HKLM" : L"HKCU"; result.push_back(std::move(entry));
    }
    RegCloseKey(key);
  }
  for (bool common : {false, true}) {
    wchar_t directory[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, common ? 0x18 : 0x07, nullptr, 0, directory))) {
      std::error_code error;
      for (const auto& file : std::filesystem::directory_iterator(directory, error)) {
        if (!file.is_regular_file(error) || lower(file.path().filename().wstring()) == L"desktop.ini") continue;
        Entry entry; entry.key = file.path().wstring(); entry.path = entry.key; entry.location = common ? L"CommonStartup" : L"StartupFolder";
        BYTE approval[12]{}; DWORD size = sizeof(approval);
        RegGetValueW(common ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\StartupFolder", file.path().filename().c_str(), RRF_RT_REG_BINARY, nullptr, approval, &size);
        entry.enabled = approval[0] != 3 && approval[0] != 7; entry.cells = {file.path().filename().wstring(), L"—", entry.enabled ? L"Enabled" : L"Disabled", L"Not measured"}; result.push_back(std::move(entry));
      }
    }
  }
  return result;
}
}
