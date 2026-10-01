#include "core.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <taskschd.h>
#include <xmllite.h>
#include <wrl/client.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Foundation.Collections.h>

namespace taskmgr {
using Microsoft::WRL::ComPtr;
namespace {
constexpr const wchar_t* approvalBase = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
struct RegistryKey {
  HKEY value = nullptr;
  ~RegistryKey() { if (value) RegCloseKey(value); }
  RegistryKey() = default;
  RegistryKey(const RegistryKey&) = delete;
  RegistryKey& operator=(const RegistryKey&) = delete;
};
struct Bstr {
  BSTR value = nullptr;
  Bstr() = default;
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  ~Bstr() { SysFreeString(value); }
  std::wstring text() const { return value ? std::wstring(value, SysStringLen(value)) : L""; }
};
struct ServiceHandle {
  SC_HANDLE value = nullptr;
  explicit ServiceHandle(SC_HANDLE handle) : value(handle) {}
  ~ServiceHandle() { if (value) CloseServiceHandle(value); }
};
struct Scan {
  StartupInventory inventory;
  std::unordered_set<std::wstring> identities;
  std::unordered_map<std::wstring, std::wstring> publishers;
  void warning(const std::wstring& source, DWORD error) {
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) inventory.warnings.push_back(source + L": " + winerror(error));
  }
  void add(Entry entry) {
    if (!identities.insert(lower(entry.key)).second) return;
    if (!entry.path.empty() && entry.cells[1].empty()) {
      const auto key = lower(entry.path); auto found = publishers.find(key);
      if (found == publishers.end()) {
        std::wstring publisher;
        DWORD unused = 0; const DWORD size = GetFileVersionInfoSizeW(entry.path.c_str(), &unused);
        if (size && size <= 4 * 1024 * 1024) {
          std::vector<std::byte> version(size);
          struct Translation { WORD language, codepage; }; Translation* translations = nullptr; UINT length = 0;
          if (GetFileVersionInfoW(entry.path.c_str(), 0, size, version.data()) && VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) && length >= sizeof(Translation)) {
            wchar_t query[128]{}; swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\CompanyName", translations[0].language, translations[0].codepage);
            wchar_t* text = nullptr; UINT textLength = 0;
            if (VerQueryValueW(version.data(), query, reinterpret_cast<void**>(&text), &textLength) && text && textLength) publisher = text;
          }
        }
        found = publishers.emplace(key, std::move(publisher)).first;
      }
      entry.cells[1] = found->second;
    }
    inventory.entries.push_back(std::move(entry));
  }
};
std::wstring registryString(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view = 0) {
  DWORD size = 0;
  const DWORD flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0);
  if (RegGetValueW(root, key.c_str(), name, flags, nullptr, nullptr, &size) != ERROR_SUCCESS || size > 1024 * 1024) return L"";
  std::vector<wchar_t> value(size / sizeof(wchar_t) + 1, 0);
  if (RegGetValueW(root, key.c_str(), name, flags, nullptr, value.data(), &size) != ERROR_SUCCESS) return L"";
  return value.data();
}
void replaceInsensitive(std::wstring& text, const std::wstring& token, const std::wstring& value) {
  size_t offset = 0;
  while ((offset = lower(text).find(lower(token), offset)) != std::wstring::npos) { text.replace(offset, token.size(), value); offset += value.size(); }
}
std::wstring expandForProfile(std::wstring text, const std::wstring& profile, HKEY hive, bool current) {
  if (!profile.empty()) {
    replaceInsensitive(text, L"%USERPROFILE%", profile);
    replaceInsensitive(text, L"%HOMEDRIVE%", std::filesystem::path(profile).root_name().wstring());
    replaceInsensitive(text, L"%HOMEPATH%", L"\\" + std::filesystem::path(profile).relative_path().wstring());
    const auto folders = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
    for (const auto& item : {std::pair{L"%APPDATA%", L"AppData"}, std::pair{L"%LOCALAPPDATA%", L"Local AppData"}}) {
      auto directory = hive ? registryString(hive, folders, item.second) : L"";
      if (directory.empty()) directory = profile + (wcscmp(item.second, L"AppData") == 0 ? L"\\AppData\\Roaming" : L"\\AppData\\Local");
      replaceInsensitive(directory, L"%USERPROFILE%", profile); replaceInsensitive(text, item.first, directory);
    }
  }
  if (current) {
    const DWORD size = ExpandEnvironmentStringsW(text.c_str(), nullptr, 0);
    if (size && size <= 32768) { std::wstring expanded(size, L'\0'); if (ExpandEnvironmentStringsW(text.c_str(), expanded.data(), size)) { expanded.resize(size - 1); return expanded; } }
  } else {
    for (const auto token : {L"SystemRoot", L"windir", L"ProgramFiles", L"ProgramFiles(x86)", L"ProgramW6432", L"ProgramData", L"SystemDrive"}) {
      wchar_t value[32768]{}; const DWORD size = GetEnvironmentVariableW(token, value, DWORD(std::size(value)));
      if (size && size < std::size(value)) replaceInsensitive(text, L"%" + std::wstring(token) + L"%", value);
    }
  }
  return text;
}
struct Approval { bool enabled = true, known = true; };
Approval readApproval(HKEY root, const std::wstring& key, const std::wstring& name) {
  if (!root || key.empty()) return {};
  std::array<BYTE, 12> value{}; DWORD size = DWORD(value.size());
  const auto status = RegGetValueW(root, key.c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, value.data(), &size);
  if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return {};
  if (status != ERROR_SUCCESS || size != value.size()) return {false, false};
  if (value[0] == 2 || value[0] == 6) return {true, true};
  if (value[0] == 3 || value[0] == 7) return {false, true};
  return {false, false};
}
void scanRegistry(Scan& scan, HKEY root, const std::wstring& keyPath, REGSAM view, const std::wstring& identity,
                  const std::wstring& owner, const std::wstring& profile, bool current, const std::wstring& kind,
                  const std::wstring& approval, bool editable = false) {
  RegistryKey key; const auto opened = RegOpenKeyExW(root, keyPath.c_str(), 0, KEY_QUERY_VALUE | view, &key.value);
  if (opened != ERROR_SUCCESS) { scan.warning(identity + L"\\" + keyPath, opened); return; }
  DWORD maxName = 0, maxData = 0;
  const auto queried = RegQueryInfoKeyW(key.value, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxName, &maxData, nullptr, nullptr);
  if (queried != ERROR_SUCCESS) { scan.warning(identity + L"\\" + keyPath, queried); return; }
  if (maxData > 1024 * 1024) { scan.inventory.warnings.push_back(identity + L": oversized registry value; source skipped."); return; }
  std::vector<wchar_t> name(size_t(maxName) + 2), data(size_t(maxData) / sizeof(wchar_t) + 2);
  for (DWORD index = 0;; ++index) {
    DWORD nameSize = DWORD(name.size()), size = DWORD(data.size() * sizeof(wchar_t) - sizeof(wchar_t)), type = 0;
    std::fill(data.begin(), data.end(), L'\0');
    const auto status = RegEnumValueW(key.value, index, name.data(), &nameSize, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &size);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) { scan.warning(identity + L"\\" + keyPath, status); continue; }
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || !data.front()) continue;
    const std::wstring valueName(name.data(), nameSize), command(data.data()); const auto state = readApproval(root, approval, valueName);
    Entry entry; entry.key = identity + L"\\" + keyPath + L"\\" + valueName; entry.location = identity; entry.group = command;
    entry.enabled = state.enabled; entry.startupEditable = editable && state.known;
    const auto expanded = expandForProfile(command, profile, root, current);
    if (expanded.find(L'%') == std::wstring::npos) entry.path = commandExecutable(expanded);
    entry.cells = {valueName.empty() ? L"(Default)" : valueName, L"", state.known ? state.enabled ? L"Enabled" : L"Disabled" : L"Unknown", L"Not measured", kind, command, valueName, owner, identity + L"\\" + keyPath};
    scan.add(std::move(entry));
  }
}
std::wstring shortcutTarget(const std::wstring& path, const std::wstring& profile, HKEY hive, bool current) {
  if (lower(std::filesystem::path(path).extension().wstring()) != L".lnk") return path;
  ComPtr<IShellLinkW> link; ComPtr<IPersistFile> file;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) || FAILED(file->Load(path.c_str(), STGM_READ))) return path;
  std::wstring target(32768, L'\0');
  if (FAILED(link->GetPath(target.data(), int(target.size()), nullptr, SLGP_RAWPATH)) || !target.front()) return path;
  target.resize(wcslen(target.c_str())); return expandForProfile(std::move(target), profile, hive, current);
}
void scanFolder(Scan& scan, const std::wstring& directory, HKEY hive, const std::wstring& owner,
                const std::wstring& profile, bool current, bool common = false) {
  if (directory.empty()) return;
  if (directory.find(L'%') != std::wstring::npos) { scan.inventory.warnings.push_back(owner + L": startup folder contains unresolved environment variables."); return; }
  std::error_code error; std::filesystem::directory_iterator iterator(directory, error), finish;
  if (error) { if (error != std::errc::no_such_file_or_directory) scan.inventory.warnings.push_back(owner + L": startup folder unavailable (" + std::to_wstring(error.value()) + L")."); return; }
  for (; iterator != finish; iterator.increment(error)) {
    if (error) { scan.inventory.warnings.push_back(owner + L": startup folder enumeration interrupted."); break; }
    const auto& file = *iterator;
    if (!file.is_regular_file(error)) { if (error) scan.inventory.warnings.push_back(owner + L": startup file could not be inspected."); error.clear(); continue; }
    const auto name = file.path().filename().wstring(); if (lower(name) == L"desktop.ini") continue;
    const auto state = readApproval(hive, std::wstring(approvalBase) + L"StartupFolder", name);
    Entry entry; entry.group = file.path().wstring(); entry.key = L"folder:" + owner + L":" + entry.group; entry.location = common ? L"CommonStartup" : current ? L"StartupFolder" : owner;
    entry.enabled = state.enabled; entry.startupEditable = hive && (current || common) && state.known; entry.path = shortcutTarget(entry.group, profile, hive, current);
    entry.cells = {file.path().stem().wstring(), L"", hive ? state.known ? state.enabled ? L"Enabled" : L"Disabled" : L"Unknown" : L"Unknown", L"Not measured", L"Folder", entry.group, name, owner, directory}; scan.add(std::move(entry));
  }
  if (error) scan.inventory.warnings.push_back(owner + L": startup folder enumeration incomplete.");
}
std::wstring currentSid() {
  Handle token; if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return L"";
  DWORD size = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &size); std::vector<std::byte> buffer(size);
  if (!GetTokenInformation(token.value, TokenUser, buffer.data(), size, &size)) return L"";
  wchar_t* text = nullptr; if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text)) return L"";
  const std::wstring result(text); LocalFree(text); return result;
}
std::wstring ownerName(const std::wstring& sid) {
  PSID parsed = nullptr; if (!ConvertStringSidToSidW(sid.c_str(), &parsed)) return sid;
  wchar_t name[512]{}, domain[512]{}; DWORD nameSize = DWORD(std::size(name)), domainSize = DWORD(std::size(domain)); SID_NAME_USE use{};
  const bool resolved = LookupAccountSidW(nullptr, parsed, name, &nameSize, domain, &domainSize, &use) != FALSE; LocalFree(parsed);
  return resolved ? (*domain ? std::wstring(domain) + L"\\" + name : std::wstring(name)) : sid;
}
void scanUser(Scan& scan, HKEY hive, const std::wstring& sid, const std::wstring& profile, bool current) {
  const auto owner = ownerName(sid), identity = current ? L"HKCU" : L"HKU\\" + sid;
  if (hive) {
    scanRegistry(scan, hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, identity, owner, profile, current, L"Registry", std::wstring(approvalBase) + L"Run", current);
    scanRegistry(scan, hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", 0, identity, owner, profile, current, L"Run once", L"");
    scanRegistry(scan, hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", 0, identity, owner, profile, current, L"Policy", L"");
  }
  auto directory = hive ? registryString(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders", L"Startup") : L"";
  if (directory.empty() && hive) directory = registryString(hive, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Startup");
  if (directory.empty() && !profile.empty()) directory = profile + L"\\AppData\\Roaming\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
  if (!directory.empty()) scanFolder(scan, expandForProfile(directory, profile, hive, current), hive, owner, profile, current);
}
void scanProfiles(Scan& scan) {
  const auto activeSid = currentSid();
  if (activeSid.empty()) scan.inventory.warnings.push_back(L"Current user identity could not be read.");
  wchar_t profilePath[32768]{}; GetEnvironmentVariableW(L"USERPROFILE", profilePath, DWORD(std::size(profilePath)));
  scanUser(scan, HKEY_CURRENT_USER, activeSid, profilePath, true);
  std::map<std::wstring, std::wstring> profiles; RegistryKey list;
  auto opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList", 0, KEY_READ | KEY_WOW64_64KEY, &list.value);
  if (opened == ERROR_SUCCESS) {
    for (DWORD index = 0;; ++index) {
      wchar_t name[512]{}; DWORD size = DWORD(std::size(name)); const auto status = RegEnumKeyExW(list.value, index, name, &size, nullptr, nullptr, nullptr, nullptr);
      if (status == ERROR_NO_MORE_ITEMS) break;
      if (status != ERROR_SUCCESS) { scan.warning(L"ProfileList", status); continue; }
      PSID sid = nullptr; if (!ConvertStringSidToSidW(name, &sid)) continue; LocalFree(sid);
      profiles[name] = expandForProfile(registryString(list.value, name, L"ProfileImagePath"), L"", nullptr, false);
    }
  } else scan.warning(L"ProfileList", opened);
  for (DWORD index = 0;; ++index) {
    wchar_t name[512]{}; DWORD size = DWORD(std::size(name)); const auto status = RegEnumKeyExW(HKEY_USERS, index, name, &size, nullptr, nullptr, nullptr, nullptr);
    if (status == ERROR_NO_MORE_ITEMS) break;
    if (status != ERROR_SUCCESS) { scan.warning(L"HKEY_USERS", status); continue; }
    PSID sid = nullptr; if (!ConvertStringSidToSidW(name, &sid)) continue; LocalFree(sid); profiles.try_emplace(name);
  }
  for (const auto& [sid, profile] : profiles) {
    if (sid == activeSid) continue;
    RegistryKey hive; auto status = RegOpenKeyExW(HKEY_USERS, sid.c_str(), 0, KEY_READ, &hive.value);
    if (status == ERROR_FILE_NOT_FOUND && !profile.empty()) status = RegLoadAppKeyW((profile + L"\\NTUSER.DAT").c_str(), &hive.value, KEY_READ, REG_PROCESS_APPKEY, 0);
    if (status != ERROR_SUCCESS) scan.inventory.warnings.push_back(ownerName(sid) + L": user registry unavailable (" + winerror(status) + L"); redirected folders and disabled state may be unavailable.");
    scanUser(scan, hive.value, sid, profile, false);
  }
}
void scanMachine(Scan& scan) {
  for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
    const auto identity = view == KEY_WOW64_32KEY ? L"HKLM32" : L"HKLM";
    scanRegistry(scan, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", view, identity, L"All users", L"", false, L"Registry", std::wstring(approvalBase) + (view == KEY_WOW64_32KEY ? L"Run32" : L"Run"), true);
    scanRegistry(scan, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce", view, identity, L"All users", L"", false, L"Run once", L"");
  }
  scanRegistry(scan, HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Explorer\\Run", KEY_WOW64_64KEY, L"HKLM", L"All users", L"", false, L"Policy", L"");
  wchar_t* directory = nullptr; const auto status = SHGetKnownFolderPath(FOLDERID_CommonStartup, KF_FLAG_DONT_VERIFY, nullptr, &directory);
  if (SUCCEEDED(status)) scanFolder(scan, directory, HKEY_LOCAL_MACHINE, L"All users", L"", false, true);
  else scan.inventory.warnings.push_back(L"Common startup folder unavailable.");
  CoTaskMemFree(directory);
}
void scanServices(Scan& scan) {
  ServiceHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE));
  if (!manager.value) { scan.warning(L"Services and drivers", GetLastError()); return; }
  std::vector<BYTE> buffer(256 * 1024); DWORD resume = 0;
  for (;;) {
    DWORD needed = 0, count = 0;
    const bool success = EnumServicesStatusExW(manager.value, SC_ENUM_PROCESS_INFO, SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL, buffer.data(), DWORD(buffer.size()), &needed, &count, &resume, nullptr) != FALSE;
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (!success && error != ERROR_MORE_DATA) { scan.warning(L"Services and drivers", error); break; }
    const auto entries = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(buffer.data());
    for (DWORD index = 0; index < count; ++index) {
      ServiceHandle service(OpenServiceW(manager.value, entries[index].lpServiceName, SERVICE_QUERY_CONFIG));
      if (!service.value) { scan.warning(std::wstring(L"Service ") + entries[index].lpServiceName, GetLastError()); continue; }
      DWORD size = 0; QueryServiceConfigW(service.value, nullptr, 0, &size);
      if (!size || size > 1024 * 1024) { scan.warning(std::wstring(L"Service ") + entries[index].lpServiceName, GetLastError()); continue; }
      std::vector<BYTE> config(size); const auto information = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(config.data());
      if (!QueryServiceConfigW(service.value, information, size, &size)) { scan.warning(std::wstring(L"Service ") + entries[index].lpServiceName, GetLastError()); continue; }
      if (information->dwStartType > SERVICE_AUTO_START) continue;
      SERVICE_DELAYED_AUTO_START_INFO delayed{}; QueryServiceConfig2W(service.value, SERVICE_CONFIG_DELAYED_AUTO_START_INFO, reinterpret_cast<BYTE*>(&delayed), sizeof(delayed), &size);
      const bool driver = (information->dwServiceType & SERVICE_DRIVER) != 0; const std::wstring command = information->lpBinaryPathName ? information->lpBinaryPathName : L"";
      auto expanded = expandForProfile(command, L"", nullptr, false); replaceInsensitive(expanded, L"\\SystemRoot\\", windowsDirectory() + L"\\");
      if (startsWithInsensitive(expanded, L"\\??\\")) expanded.erase(0, 4);
      if (startsWithInsensitive(expanded, L"System32\\")) expanded = windowsDirectory() + L"\\" + expanded;
      Entry entry; entry.key = L"service:" + std::wstring(entries[index].lpServiceName); entry.enabled = true;
      if (expanded.find(L'%') == std::wstring::npos) entry.path = commandExecutable(expanded);
      entry.cells = {entries[index].lpDisplayName, L"", delayed.fDelayedAutostart ? L"Delayed" : L"Enabled", L"Not measured", driver ? information->dwStartType == SERVICE_BOOT_START ? L"Boot driver" : information->dwStartType == SERVICE_SYSTEM_START ? L"System driver" : L"Auto driver" : L"Service", command, entries[index].lpServiceName, information->lpServiceStartName ? information->lpServiceStartName : L"System", L"HKLM\\SYSTEM\\CurrentControlSet\\Services\\" + std::wstring(entries[index].lpServiceName)}; scan.add(std::move(entry));
    }
    if (success) break;
    if (!count) { if (needed > 16 * 1024 * 1024 || needed <= buffer.size()) { scan.inventory.warnings.push_back(L"Services enumeration could not progress."); break; } buffer.resize(needed); }
  }
}
void scanTaskFolder(Scan& scan, ITaskFolder* folder, unsigned depth) {
  if (depth > 64) { scan.inventory.warnings.push_back(L"Task Scheduler folder nesting exceeds the scan limit."); return; }
  Bstr folderName; folder->get_Path(&folderName.value); ComPtr<IRegisteredTaskCollection> tasks;
  auto status = folder->GetTasks(TASK_ENUM_HIDDEN, &tasks);
  if (FAILED(status)) scan.inventory.warnings.push_back(L"Tasks " + folderName.text() + L": " + winerror(DWORD(status)));
  else {
    LONG count = 0;
    if (FAILED(tasks->get_Count(&count))) scan.inventory.warnings.push_back(L"Task count unavailable: " + folderName.text());
    for (LONG index = 1; index <= count; ++index) {
      VARIANT position{}; position.vt = VT_I4; position.lVal = index;
      ComPtr<IRegisteredTask> task; ComPtr<ITaskDefinition> definition; ComPtr<ITriggerCollection> triggers;
      if (FAILED(tasks->get_Item(position, &task)) || FAILED(task->get_Definition(&definition)) || FAILED(definition->get_Triggers(&triggers))) { scan.inventory.warnings.push_back(L"A scheduled task could not be inspected in " + folderName.text()); continue; }
      LONG triggerCount = 0; bool startup = false, activeTrigger = false, triggersKnown = true;
      if (FAILED(triggers->get_Count(&triggerCount))) { scan.inventory.warnings.push_back(L"Task triggers unavailable: " + folderName.text()); continue; }
      for (LONG triggerIndex = 1; triggerIndex <= triggerCount; ++triggerIndex) {
        ComPtr<ITrigger> trigger; TASK_TRIGGER_TYPE2 type{}; VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(triggers->get_Item(triggerIndex, &trigger)) || FAILED(trigger->get_Type(&type))) { triggersKnown = false; scan.inventory.warnings.push_back(L"A task trigger could not be inspected in " + folderName.text()); continue; }
        if (type == TASK_TRIGGER_BOOT || type == TASK_TRIGGER_LOGON) {
          startup = true;
          if (FAILED(trigger->get_Enabled(&enabled))) { triggersKnown = false; scan.inventory.warnings.push_back(L"Task trigger state unavailable: " + folderName.text()); }
          else if (enabled != VARIANT_FALSE) activeTrigger = true;
        }
      }
      if (!startup) continue;
      Bstr taskName, taskPath, owner;
      if (FAILED(task->get_Name(&taskName.value)) || FAILED(task->get_Path(&taskPath.value))) { scan.inventory.warnings.push_back(L"Task identity unavailable: " + folderName.text()); continue; }
      ComPtr<IPrincipal> principal; if (SUCCEEDED(definition->get_Principal(&principal))) { principal->get_UserId(&owner.value); if (owner.text().empty()) { SysFreeString(owner.value); owner.value = nullptr; principal->get_GroupId(&owner.value); } }
      ComPtr<IActionCollection> actions;
      if (FAILED(definition->get_Actions(&actions))) { scan.inventory.warnings.push_back(L"Task actions unavailable: " + taskPath.text()); continue; }
      LONG actionCount = 0;
      if (FAILED(actions->get_Count(&actionCount))) { scan.inventory.warnings.push_back(L"Task action count unavailable: " + taskPath.text()); continue; }
      VARIANT_BOOL enabled = VARIANT_FALSE; const bool known = SUCCEEDED(task->get_Enabled(&enabled)) && (activeTrigger || triggersKnown);
      for (LONG actionIndex = 1; actionIndex <= std::max(1L, actionCount); ++actionIndex) {
        std::wstring command, path, kind = L"Scheduled task";
        if (actionCount) {
          ComPtr<IAction> action; TASK_ACTION_TYPE type{};
          if (FAILED(actions->get_Item(actionIndex, &action)) || FAILED(action->get_Type(&type))) { scan.inventory.warnings.push_back(L"Task action unavailable: " + taskPath.text()); continue; }
          if (type == TASK_ACTION_EXEC) { ComPtr<IExecAction> exec; Bstr executablePath, arguments; if (SUCCEEDED(action.As(&exec))) { exec->get_Path(&executablePath.value); exec->get_Arguments(&arguments.value); path = expandForProfile(executablePath.text(), L"", nullptr, false); if (path.find(L'%') != std::wstring::npos) path.clear(); command = L"\"" + executablePath.text() + L"\" " + arguments.text(); } }
          else if (type == TASK_ACTION_COM_HANDLER) { ComPtr<IComHandlerAction> handler; Bstr classId; if (SUCCEEDED(action.As(&handler))) handler->get_ClassId(&classId.value); kind = L"COM task"; command = L"COM handler " + classId.text(); }
          else command = L"Non-executable task action";
        }
        Entry entry; entry.key = L"task:" + taskPath.text() + L":" + std::to_wstring(actionIndex); entry.enabled = enabled != VARIANT_FALSE && activeTrigger; entry.path = std::move(path);
        entry.cells = {taskName.text(), L"", known ? entry.enabled ? L"Enabled" : L"Disabled" : L"Unknown", L"Not measured", kind, command, L"", owner.text(), L"Task Scheduler" + taskPath.text()}; scan.add(std::move(entry));
      }
    }
  }
  ComPtr<ITaskFolderCollection> folders; status = folder->GetFolders(0, &folders);
  if (FAILED(status)) { scan.inventory.warnings.push_back(L"Task subfolders " + folderName.text() + L": " + winerror(DWORD(status))); return; }
  LONG count = 0;
  if (FAILED(folders->get_Count(&count))) { scan.inventory.warnings.push_back(L"Task subfolder count unavailable: " + folderName.text()); return; }
  for (LONG index = 1; index <= count; ++index) {
    VARIANT position{}; position.vt = VT_I4; position.lVal = index; ComPtr<ITaskFolder> child;
    if (SUCCEEDED(folders->get_Item(position, &child))) scanTaskFolder(scan, child.Get(), depth + 1);
    else scan.inventory.warnings.push_back(L"A Task Scheduler folder could not be opened.");
  }
}
void scanTasks(Scan& scan) {
  ComPtr<ITaskService> service; auto status = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service)); VARIANT empty{};
  if (SUCCEEDED(status)) status = service->Connect(empty, empty, empty, empty);
  ComPtr<ITaskFolder> root; Bstr name; name.value = SysAllocString(L"\\");
  if (SUCCEEDED(status)) status = service->GetFolder(name.value, &root);
  if (FAILED(status)) scan.inventory.warnings.push_back(L"Task Scheduler unavailable: " + winerror(DWORD(status)));
  else scanTaskFolder(scan, root.Get(), 0);
}
std::wstring xmlAttribute(IXmlReader* reader, const wchar_t* name) {
  if (reader->MoveToAttributeByName(name, nullptr) != S_OK) return L"";
  const wchar_t* text = nullptr; UINT size = 0; std::wstring result;
  if (SUCCEEDED(reader->GetValue(&text, &size))) result.assign(text, size);
  reader->MoveToElement(); return result;
}
void scanPackage(Scan& scan, const winrt::Windows::ApplicationModel::Package& package, const winrt::Windows::Management::Deployment::PackageManager& manager, bool allUsers) {
  try {
    if (package.IsFramework() || package.IsResourcePackage()) return;
    const std::wstring directory(package.InstalledLocation().Path()), fullName(package.Id().FullName());
    ComPtr<IStream> stream; ComPtr<IXmlReader> reader;
    auto status = SHCreateStreamOnFileEx((directory + L"\\AppxManifest.xml").c_str(), STGM_READ | STGM_SHARE_DENY_NONE, FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream);
    if (FAILED(status) || FAILED(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)) || FAILED(reader->SetInput(stream.Get()))) { scan.inventory.warnings.push_back(L"Package manifest unavailable: " + fullName); return; }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit); reader->SetProperty(XmlReaderProperty_MaxElementDepth, 64);
    XmlNodeType type{}; std::wstring applicationExecutable, extensionExecutable; UINT applicationDepth = 0, extensionDepth = 0; bool startupExtension = false; std::wstring owners;
    while ((status = reader->Read(&type)) == S_OK) {
      const wchar_t* name = nullptr; UINT size = 0, depth = 0; reader->GetLocalName(&name, &size); reader->GetDepth(&depth); const std::wstring_view local(name ? name : L"", size);
      if (type == XmlNodeType_Element) {
        if (local == L"Application") { applicationExecutable = xmlAttribute(reader.Get(), L"Executable"); applicationDepth = depth; }
        else if (local == L"Extension") { extensionDepth = depth; startupExtension = xmlAttribute(reader.Get(), L"Category") == L"windows.startupTask"; extensionExecutable = xmlAttribute(reader.Get(), L"Executable"); }
        else if (local == L"StartupTask" && startupExtension) {
          if (owners.empty()) {
            if (allUsers) { try { for (const auto& user : manager.FindUsers(fullName)) { if (!owners.empty()) owners += L", "; owners += ownerName(std::wstring(user.UserSecurityId())); } } catch (const winrt::hresult_error&) { scan.inventory.warnings.push_back(L"Package owner information unavailable: " + fullName); } }
            if (owners.empty()) owners = allUsers ? L"Registered users unknown" : ownerName(currentSid());
          }
          const auto taskId = xmlAttribute(reader.Get(), L"TaskId"); auto display = xmlAttribute(reader.Get(), L"DisplayName"); if (display.empty() || startsWithInsensitive(display, L"ms-resource:")) display = std::wstring(package.DisplayName());
          auto executablePath = extensionExecutable.empty() ? applicationExecutable : extensionExecutable;
          Entry entry; entry.key = L"package:" + fullName + L":" + taskId;
          if (!executablePath.empty()) entry.path = (std::filesystem::path(directory) / executablePath).wstring();
          entry.cells = {display.empty() ? taskId : display, std::wstring(package.PublisherDisplayName()), L"Windows-managed", L"Not measured", L"Packaged app", entry.path, taskId, owners, fullName + L" / " + taskId}; scan.add(std::move(entry));
        }
      } else if (type == XmlNodeType_EndElement) { if (local == L"Extension" && depth == extensionDepth) { startupExtension = false; extensionExecutable.clear(); } if (local == L"Application" && depth == applicationDepth) applicationExecutable.clear(); }
    }
    if (FAILED(status)) scan.inventory.warnings.push_back(L"Package manifest incomplete: " + fullName);
  } catch (const winrt::hresult_error& error) { scan.inventory.warnings.push_back(L"Package inspection failed: " + std::wstring(error.message())); }
}
void scanPackages(Scan& scan) {
  try {
    winrt::Windows::Management::Deployment::PackageManager manager;
    try { for (const auto& package : manager.FindPackages()) scanPackage(scan, package, manager, true); }
    catch (const winrt::hresult_error&) {
      scan.inventory.warnings.push_back(L"All-user packaged apps unavailable; only current-user packages scanned. Run as administrator for broader coverage.");
      for (const auto& package : manager.FindPackagesForUser(L"")) scanPackage(scan, package, manager, false);
    }
  } catch (const winrt::hresult_error& error) { scan.inventory.warnings.push_back(L"Packaged startup apps unavailable: " + std::wstring(error.message())); }
}
}
StartupInventory readStartup() {
  Scan scan;
  for (const auto& source : {std::pair{L"Machine startup", &scanMachine}, std::pair{L"User profiles", &scanProfiles}, std::pair{L"Task Scheduler", &scanTasks}, std::pair{L"Services and drivers", &scanServices}, std::pair{L"Packaged apps", &scanPackages}}) {
    try { source.second(scan); }
    catch (const std::exception&) { scan.inventory.warnings.push_back(std::wstring(source.first) + L": source scan failed; other sources retained."); }
  }
  scan.inventory.warnings.push_back(L"Scope: boot/logon tasks, automatic services/drivers, packaged startup tasks, Run/RunOnce/policy keys and startup folders. Shell extensions, WMI subscriptions, Winlogon scripts and other injection/provider mechanisms are not enumerated. See docs/STARTUP.md.");
  std::sort(scan.inventory.warnings.begin(), scan.inventory.warnings.end());
  scan.inventory.warnings.erase(std::unique(scan.inventory.warnings.begin(), scan.inventory.warnings.end()), scan.inventory.warnings.end());
  for (size_t index = 0; index < scan.inventory.warnings.size(); ++index) {
    Entry entry; entry.key = L"coverage:" + std::to_wstring(index);
    entry.cells = {L"Coverage: " + scan.inventory.warnings[index], L"", L"Partial", L"—", L"Coverage", L"", L"", L"", scan.inventory.warnings[index]}; scan.inventory.entries.push_back(std::move(entry));
  }
  return std::move(scan.inventory);
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> startupTests() {
  std::vector<std::pair<std::string, bool>> result;
  RegistryKey root; const auto key = L"Software\\TaskManagerNativeTests\\Startup-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
  if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &root.value, nullptr) != ERROR_SUCCESS) return {{"startup_fixture_created", false}};
  RegistryKey run, approval;
  const bool created = RegCreateKeyExW(root.value, L"Run", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &run.value, nullptr) == ERROR_SUCCESS && RegCreateKeyExW(root.value, L"Approval", 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, &approval.value, nullptr) == ERROR_SUCCESS;
  result.emplace_back("startup_fixture_created", created);
  if (created) {
    const std::wstring longName(2000, L'Z'), command = L"\"C:\\Missing App\\app.exe\" /test";
    RegSetValueExW(run.value, longName.c_str(), 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t)));
    std::array<BYTE, 12> disabled{}; disabled[0] = 3; RegSetValueExW(approval.value, longName.c_str(), 0, REG_BINARY, disabled.data(), DWORD(disabled.size()));
    Scan scan; scanRegistry(scan, root.value, L"Run", 0, L"User1", L"User1", L"", false, L"Registry", L"Approval"); scanRegistry(scan, root.value, L"Run", 0, L"User2", L"User2", L"", false, L"Registry", L"Approval");
    result.emplace_back("startup_distinct_owners_not_deduplicated", scan.inventory.entries.size() == 2);
    result.emplace_back("startup_long_value_names", scan.inventory.entries.size() == 2 && scan.inventory.entries.front().cells[6] == longName);
    result.emplace_back("startup_disabled_and_missing_target_retained", scan.inventory.entries.size() == 2 && !scan.inventory.entries.front().enabled && scan.inventory.entries.front().path == L"C:\\Missing App\\app.exe");
    result.emplace_back("startup_other_user_read_only", scan.inventory.entries.size() == 2 && !scan.inventory.entries.front().startupEditable);
    disabled[0] = 99; RegSetValueExW(approval.value, longName.c_str(), 0, REG_BINARY, disabled.data(), DWORD(disabled.size()));
    result.emplace_back("startup_unknown_approval_not_enabled", !readApproval(root.value, L"Approval", longName).known);
    scanRegistry(scan, root.value, L"Run", 0, L"User1", L"User1", L"", false, L"Registry", L"Approval");
    result.emplace_back("startup_same_registration_deduplicated", scan.inventory.entries.size() == 2);
    RegSetValueExW(run.value, L"Ignored multi-string", 0, REG_MULTI_SZ, reinterpret_cast<const BYTE*>(command.c_str()), DWORD((command.size() + 1) * sizeof(wchar_t)));
    Scan unsupported; scanRegistry(unsupported, root.value, L"Run", 0, L"User1", L"User1", L"", false, L"Run once", L"");
    result.emplace_back("startup_unsupported_value_types_ignored", unsupported.inventory.entries.size() == 1);
    result.emplace_back("startup_runonce_not_toggleable", unsupported.inventory.entries.size() == 1 && !unsupported.inventory.entries.front().startupEditable);
    result.emplace_back("startup_owner_environment", expandForProfile(L"%USERPROFILE%\\bin\\app.exe", L"C:\\Users\\Other", nullptr, false) == L"C:\\Users\\Other\\bin\\app.exe");
    result.emplace_back("startup_unresolved_user_environment_retained", expandForProfile(L"%USERNAME%\\app.exe", L"C:\\Users\\Other", nullptr, false) == L"%USERNAME%\\app.exe");
    bool refused = false; try { startupAction(scan.inventory.entries.front()); } catch (...) { refused = true; } result.emplace_back("startup_read_only_action_refused", refused);
  }
  RegDeleteTreeW(HKEY_CURRENT_USER, key.c_str());
  RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\TaskManagerNativeTests");
  return result;
}
#endif
}
