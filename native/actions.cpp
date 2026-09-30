#include "core.hpp"
#include <dbghelp.h>
#include <wct.h>
#include <tlhelp32.h>
#include <iomanip>

namespace taskmgr {
static void verify(HANDLE handle, const Process& process, bool mutation) {
  if (!handle || handle == INVALID_HANDLE_VALUE) throw std::runtime_error(utf8(winerror()));
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(handle, &created, &exited, &kernel, &user) || ticks(created) != process.created) throw std::runtime_error("Process exited or PID was reused. Refresh and select it again.");
  if (mutation) {
    if (process.id <= 4 || process.id == GetCurrentProcessId()) throw std::runtime_error("System/self actions are refused.");
    BOOL critical = FALSE;
    if (!IsProcessCritical(handle, &critical)) throw std::runtime_error(utf8(winerror()));
    if (critical) throw std::runtime_error("Windows-critical process action refused.");
  }
}
void processAction(const Process& process, Action action, const std::wstring& argument) {
  DWORD access = PROCESS_QUERY_LIMITED_INFORMATION;
  if (action == Action::End) access |= PROCESS_TERMINATE;
  else if (action == Action::Dump) access |= PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
  else access |= PROCESS_SET_INFORMATION;
  Handle handle(OpenProcess(access, FALSE, process.id)); verify(handle.value, process, true);
  BOOL success = FALSE;
  switch (action) {
  case Action::End: success = TerminateProcess(handle.value, 1); break;
  case Action::Idle: success = SetPriorityClass(handle.value, IDLE_PRIORITY_CLASS); break;
  case Action::Normal: success = SetPriorityClass(handle.value, NORMAL_PRIORITY_CLASS); break;
  case Action::AboveNormal: success = SetPriorityClass(handle.value, ABOVE_NORMAL_PRIORITY_CLASS); break;
  case Action::High: success = SetPriorityClass(handle.value, HIGH_PRIORITY_CLASS); break;
  case Action::Affinity: {
    size_t consumed = 0; DWORD_PTR mask = std::stoull(argument, &consumed, 16), own = 0, system = 0;
    if (!mask || consumed != argument.size() || !GetProcessAffinityMask(handle.value, &own, &system) || (mask & ~system)) throw std::runtime_error("Choose a valid nonzero hexadecimal processor mask.");
    success = SetProcessAffinityMask(handle.value, mask); break;
  }
  case Action::EfficiencyOn:
  case Action::EfficiencyOff: {
    PROCESS_POWER_THROTTLING_STATE old{PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0};
    bool read = GetProcessInformation(handle.value, ProcessPowerThrottling, &old, sizeof(old)) != FALSE;
    if (!read) {
      using Query = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
      auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess")); ULONG returned = 0;
      read = query && query(handle.value, 77, &old, sizeof(old), &returned) >= 0 && returned == sizeof(old) && old.Version == PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    }
    if (!read) throw std::runtime_error("Power throttling query unavailable.");
    auto desired = old; desired.ControlMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    if (action == Action::EfficiencyOn) desired.StateMask |= PROCESS_POWER_THROTTLING_EXECUTION_SPEED; else desired.StateMask &= ~PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    if (!SetProcessInformation(handle.value, ProcessPowerThrottling, &desired, sizeof(desired))) throw std::runtime_error(utf8(winerror()));
    success = SetPriorityClass(handle.value, action == Action::EfficiencyOn ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
    if (!success) { const DWORD error = GetLastError(); SetProcessInformation(handle.value, ProcessPowerThrottling, &old, sizeof(old)); throw std::runtime_error(utf8(winerror(error))); }
    break;
  }
  case Action::Dump: {
    static std::mutex gate; std::lock_guard lock(gate);
    const auto temporary = argument + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    try {
      { Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)); if (file.value == INVALID_HANDLE_VALUE || !MiniDumpWriteDump(handle.value, process.id, file.value, MiniDumpWithFullMemory, nullptr, nullptr, nullptr)) throw std::runtime_error(utf8(winerror())); }
      if (!MoveFileExW(temporary.c_str(), argument.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) throw std::runtime_error(utf8(winerror()));
    } catch (...) { DeleteFileW(temporary.c_str()); throw; }
    success = TRUE; break;
  }
  default: throw std::runtime_error("Unsupported action");
  }
  if (!success) throw std::runtime_error(utf8(winerror()));
}
std::wstring waitChain(const Process& process) {
  Handle handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process.id)); verify(handle.value, process, false);
  auto session = OpenThreadWaitChainSession(0, nullptr); if (!session) throw std::runtime_error(utf8(winerror()));
  std::wostringstream result;
  Handle threads(CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)); THREADENTRY32 thread{sizeof(thread)}; unsigned inspected = 0;
  if (threads.value != INVALID_HANDLE_VALUE && Thread32First(threads.value, &thread)) do {
    if (thread.th32OwnerProcessID != process.id) continue;
    if (++inspected > 128) { result << L"Additional threads omitted (128-thread bound).\r\n"; break; }
    std::array<WAITCHAIN_NODE_INFO, WCT_MAX_NODE_COUNT> nodes{}; DWORD count = DWORD(nodes.size()); BOOL cycle = FALSE;
    result << L"Thread " << thread.th32ThreadID << L": ";
    if (!GetThreadWaitChain(session, 0, 0, thread.th32ThreadID, &count, nodes.data(), &cycle)) { result << L"Unavailable: " << winerror() << L"\r\n"; continue; }
    if (count > nodes.size()) { result << L"Chain exceeds safety bound\r\n"; continue; }
    bool stale = false;
    for (DWORD index = 0; index < count; ++index) if (nodes[index].ObjectType == WctThreadType && nodes[index].ThreadObject.ThreadId == thread.th32ThreadID && nodes[index].ThreadObject.ProcessId != process.id) stale = true;
    if (stale) { result << L"Thread exited during analysis\r\n"; continue; }
    for (DWORD index = 0; index < count; ++index) { if (index) result << L" → "; const auto& node = nodes[index]; if (node.ObjectType == WctThreadType) result << L"Process " << node.ThreadObject.ProcessId << L", thread " << node.ThreadObject.ThreadId; else result << L"Wait object " << node.ObjectType; result << L" (status " << node.ObjectStatus << L")"; }
    if (cycle) result << L" — possible deadlock";
    result << L"\r\n";
  } while (Thread32Next(threads.value, &thread));
  CloseThreadWaitChainSession(session); return result.str().empty() ? L"No accessible threads." : result.str();
}
void serviceAction(const Entry& entry, int action) {
  SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
  if (!manager) throw std::runtime_error(utf8(winerror()));
  SC_HANDLE service = OpenServiceW(manager, entry.key.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS);
  if (!service) { const DWORD error = GetLastError(); CloseServiceHandle(manager); throw std::runtime_error(utf8(winerror(error))); }
  try {
    if (action != 0) {
      SERVICE_STATUS status{};
      if (!ControlService(service, SERVICE_CONTROL_STOP, &status) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) throw std::runtime_error(utf8(winerror()));
      const auto started = Clock::now();
      while (status.dwCurrentState != SERVICE_STOPPED && milliseconds(started) < 10000) { Sleep(100); if (!QueryServiceStatus(service, &status)) throw std::runtime_error(utf8(winerror())); }
      if (status.dwCurrentState != SERVICE_STOPPED) throw std::runtime_error("Service did not stop within ten seconds.");
    }
    if (action != 1 && !StartServiceW(service, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) throw std::runtime_error(utf8(winerror()));
  } catch (...) { CloseServiceHandle(service); CloseServiceHandle(manager); throw; }
  CloseServiceHandle(service); CloseServiceHandle(manager);
}
void startupAction(const Entry& entry) {
  const bool folder = entry.location == L"StartupFolder" || entry.location == L"CommonStartup";
  const bool machine = entry.location == L"HKLM" || entry.location == L"CommonStartup";
  const auto root = machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  const auto name = folder ? std::filesystem::path(entry.path).filename().wstring() : entry.cells[0];
  if (folder) { if (!std::filesystem::exists(entry.path)) throw std::runtime_error("Startup item no longer exists."); }
  else {
    wchar_t command[32768]{}; DWORD size = sizeof(command);
    if (RegGetValueW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", name.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, command, &size) != ERROR_SUCCESS || entry.path != command) throw std::runtime_error("Startup entry changed. Refresh it first.");
  }
  const std::wstring subkey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\" + std::wstring(folder ? L"StartupFolder" : L"Run");
  HKEY key = nullptr; const auto opened = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (opened != ERROR_SUCCESS) throw std::runtime_error(utf8(winerror(opened)));
  std::array<BYTE, 12> approval{}; approval[0] = entry.enabled ? 3 : 2;
  if (entry.enabled) { FILETIME now{}; GetSystemTimeAsFileTime(&now); memcpy(approval.data() + 4, &now, sizeof(now)); }
  const auto written = RegSetValueExW(key, name.c_str(), 0, REG_BINARY, approval.data(), DWORD(approval.size())); RegCloseKey(key);
  if (written != ERROR_SUCCESS) throw std::runtime_error(utf8(winerror(written)));
}
void sessionAction(const Entry& entry, bool logoff) {
  const auto sessions = readSessions();
  if (std::none_of(sessions.begin(), sessions.end(), [&](const Entry& current) { return current.session == entry.session && current.cells[0] == entry.cells[0]; })) throw std::runtime_error("Session changed. Refresh it first.");
  if (!(logoff ? WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, entry.session, FALSE) : WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, entry.session, FALSE))) throw std::runtime_error(utf8(winerror()));
}
}
