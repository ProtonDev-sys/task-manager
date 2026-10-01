#include "core.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>

namespace taskmgr {
static constexpr wchar_t ReplacementKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\taskmgr.exe";
static constexpr wchar_t DirectorySecurity[] = L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;GRGX;;;BU)";
static constexpr wchar_t FileSecurity[] = L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;BU)";
struct RegistryKey { HKEY value = nullptr; ~RegistryKey() { if (value) RegCloseKey(value); } };
struct Security { PSECURITY_DESCRIPTOR value = nullptr; ~Security() { if (value) LocalFree(value); } };
static void requireSuccess(LSTATUS status) { if (status != ERROR_SUCCESS) throw std::runtime_error(utf8(winerror(DWORD(status)))); }
static std::wstring replacementPath() {
  PWSTR path = nullptr;
  const HRESULT status = SHGetKnownFolderPath(FOLDERID_ProgramFiles, KF_FLAG_DEFAULT, nullptr, &path);
  if (FAILED(status)) throw std::runtime_error("Cannot locate Program Files.");
  std::filesystem::path directory(path); CoTaskMemFree(path);
  return (directory / L"TaskManagerNative" / L"TaskManager.exe").wstring();
}
static std::wstring replacementCommand() { return L"\"" + replacementPath() + L"\" --replacement-launch"; }
static std::wstring debuggerValue(HKEY key) {
  DWORD bytes = 0;
  LSTATUS status = RegGetValueW(key, nullptr, L"Debugger", RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
  if (status == ERROR_FILE_NOT_FOUND) return L"";
  requireSuccess(status);
  if (bytes > 65536) throw std::runtime_error("The existing Task Manager replacement is invalid.");
  std::wstring value(bytes / sizeof(wchar_t) + 1, L'\0');
  requireSuccess(RegGetValueW(key, nullptr, L"Debugger", RRF_RT_REG_SZ, nullptr, value.data(), &bytes));
  value.resize(wcslen(value.c_str())); return value;
}
bool replacementEnabled() {
  try {
    RegistryKey key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ReplacementKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &key.value) != ERROR_SUCCESS) return false;
    return debuggerValue(key.value) == replacementCommand();
  } catch (...) { return false; }
}
static void installReplacement(const std::wstring& destination) {
  const auto directory = std::filesystem::path(destination).parent_path().wstring();
  Security security;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(DirectorySecurity, SDDL_REVISION_1, &security.value, nullptr)) requireSuccess(GetLastError());
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), security.value, FALSE};
  if (!CreateDirectoryW(directory.c_str(), &attributes)) {
    const DWORD error = GetLastError(); if (error != ERROR_ALREADY_EXISTS) requireSuccess(error);
    const DWORD flags = GetFileAttributesW(directory.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || !(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) throw std::runtime_error("The replacement directory is not a protected regular directory.");
    Security existing; PACL actual = nullptr, expected = nullptr; BOOL present = FALSE, defaulted = FALSE; PSID owner = nullptr, expectedOwner = nullptr;
    requireSuccess(GetNamedSecurityInfoW(directory.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &actual, nullptr, &existing.value));
    GetSecurityDescriptorOwner(security.value, &expectedOwner, &defaulted);
    GetSecurityDescriptorDacl(security.value, &present, &expected, &defaulted);
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    if (!owner || !expectedOwner || !EqualSid(owner, expectedOwner) || !GetSecurityDescriptorControl(existing.value, &control, &revision) || !(control & SE_DACL_PROTECTED) || !actual || !expected || actual->AclSize != expected->AclSize || memcmp(actual, expected, actual->AclSize) != 0) throw std::runtime_error("The replacement directory has unexpected permissions; it was not changed.");
  }
  const auto source = executable();
  if (CompareStringOrdinal(source.c_str(), -1, destination.c_str(), -1, TRUE) != CSTR_EQUAL && !CopyFileW(source.c_str(), destination.c_str(), TRUE)) {
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
      const DWORD flags = GetFileAttributesW(destination.c_str());
      if (flags == INVALID_FILE_ATTRIBUTES || (flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) throw std::runtime_error("The replacement executable is not a regular file.");
      std::ifstream first{std::filesystem::path(source), std::ios::binary}, second{std::filesystem::path(destination), std::ios::binary};
      std::array<char, 8192> sourceBytes{}, destinationBytes{};
      if (!first || !second) throw std::runtime_error("Cannot verify the installed replacement executable.");
      do {
        first.read(sourceBytes.data(), sourceBytes.size()); second.read(destinationBytes.data(), destinationBytes.size());
        if (first.gcount() != second.gcount() || memcmp(sourceBytes.data(), destinationBytes.data(), size_t(first.gcount())) != 0 || first.bad() || second.bad()) throw std::runtime_error("A different version is already installed. Disable replacement, close the installed app, and remove Program Files\\TaskManagerNative\\TaskManager.exe before enabling the new version.");
      } while (first.gcount() != 0);
    } else requireSuccess(error);
  }
  Security fileSecurity;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(FileSecurity, SDDL_REVISION_1, &fileSecurity.value, nullptr)) requireSuccess(GetLastError());
  PACL acl = nullptr; BOOL present = FALSE, defaulted = FALSE; PSID owner = nullptr, group = nullptr;
  GetSecurityDescriptorDacl(fileSecurity.value, &present, &acl, &defaulted);
  GetSecurityDescriptorOwner(fileSecurity.value, &owner, &defaulted); GetSecurityDescriptorGroup(fileSecurity.value, &group, &defaulted);
  requireSuccess(SetNamedSecurityInfoW(const_cast<wchar_t*>(destination.c_str()), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, owner, group, acl, nullptr));
}
int configureReplacement(bool enabled) {
  try {
    if (!IsUserAnAdmin()) throw std::runtime_error("Administrator approval is required to change the Windows Task Manager replacement.");
    RegistryKey key;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, ReplacementKey, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key.value);
    if (status == ERROR_FILE_NOT_FOUND && !enabled) return 0;
    if (status != ERROR_FILE_NOT_FOUND) requireSuccess(status);
    const auto expected = replacementCommand(); const auto existing = key.value ? debuggerValue(key.value) : L"";
    if (!existing.empty() && existing != expected) throw std::runtime_error("Another Task Manager replacement is registered. It was not overwritten or removed.");
    if (enabled) {
      installReplacement(replacementPath());
      if (!key.value) requireSuccess(RegCreateKeyExW(HKEY_LOCAL_MACHINE, ReplacementKey, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key.value, nullptr));
      if (debuggerValue(key.value) != existing) throw std::runtime_error("The Task Manager replacement changed during setup. Try again.");
      requireSuccess(RegSetValueExW(key.value, L"Debugger", 0, REG_SZ, reinterpret_cast<const BYTE*>(expected.c_str()), DWORD((expected.size() + 1) * sizeof(wchar_t))));
    } else if (!existing.empty()) requireSuccess(RegDeleteValueW(key.value, L"Debugger"));
    return 0;
  } catch (const std::exception& error) { MessageBoxA(nullptr, error.what(), "Task Manager replacement", MB_OK | MB_ICONERROR); return 1; }
}
void requestReplacement(HWND owner, bool enabled) {
  const auto path = executable(); SHELLEXECUTEINFOW request{sizeof(request)};
  request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI; request.hwnd = owner; request.lpVerb = L"runas"; request.lpFile = path.c_str(); request.lpParameters = enabled ? L"--enable-replacement" : L"--disable-replacement"; request.nShow = SW_HIDE;
  if (!ShellExecuteExW(&request)) { const DWORD error = GetLastError(); if (error == ERROR_CANCELLED) return; requireSuccess(error); }
  Handle helper(request.hProcess);
  if (!helper.value || WaitForSingleObject(helper.value, INFINITE) != WAIT_OBJECT_0) throw std::runtime_error("Cannot wait for Task Manager replacement setup.");
  DWORD code = 1; if (!GetExitCodeProcess(helper.value, &code)) requireSuccess(GetLastError());
  if (code != 0) throw std::runtime_error("Task Manager replacement setup did not complete. The menu reflects the actual Windows setting.");
}
}
