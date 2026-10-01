#include "core.hpp"
#include <shlobj.h>
#include <appmodel.h>

namespace taskmgr {
static std::wstring kernelImagePath(DWORD pid) {
  struct UnicodeText { USHORT length, maximum; wchar_t* buffer; };
  struct Request { HANDLE pid; UnicodeText name; };
  using QuerySystem = LONG (WINAPI*)(ULONG, void*, ULONG, ULONG*);
  static const auto query = reinterpret_cast<QuerySystem>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
  if (!query) return L"";
  std::array<wchar_t, 1024> text{}; Request request{reinterpret_cast<HANDLE>(ULONG_PTR(pid)), {0, USHORT(text.size() * sizeof(wchar_t)), text.data()}};
  if (query(88, &request, sizeof(request), nullptr) < 0 || !request.name.length) return L"";
  const std::wstring native(text.data(), request.name.length / sizeof(wchar_t));
  static std::vector<std::pair<std::wstring, std::wstring>> devices; static std::mutex gate; std::lock_guard lock(gate);
  if (devices.empty()) for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) { const wchar_t drive[3]{letter, L':', 0}; wchar_t target[512]{}; if (QueryDosDeviceW(drive, target, 512)) devices.emplace_back(target, drive); }
  for (const auto& [device, drive] : devices) if (native.size() > device.size() && native.compare(0, device.size(), device) == 0 && native[device.size()] == L'\\') return drive + native.substr(device.size());
  return L"";
}
static std::pair<std::wstring, std::wstring> versionStrings(const std::wstring& path) {
  std::pair<std::wstring, std::wstring> result; DWORD unused = 0; const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
  if (!size || size > 4 * 1024 * 1024) return result;
  std::vector<std::byte> version(size); struct Translation { WORD language, codepage; }; Translation* translations = nullptr; UINT length = 0;
  if (!GetFileVersionInfoW(path.c_str(), 0, size, version.data()) || !VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) || length < sizeof(Translation)) return result;
  for (const auto field : {L"FileDescription", L"CompanyName"}) {
    wchar_t query[128]{}; swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codepage, field);
    wchar_t* text = nullptr; UINT textLength = 0;
    if (VerQueryValueW(version.data(), query, reinterpret_cast<void**>(&text), &textLength) && text && textLength) { std::wstring value = text; while (!value.empty() && iswspace(value.back())) value.pop_back(); (wcscmp(field, L"FileDescription") == 0 ? result.first : result.second) = value; }
  }
  return result;
}
static std::wstring commandLine(HANDLE process) {
  using QueryProcess = LONG (WINAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  static const auto query = reinterpret_cast<QueryProcess>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
  if (!query) return L"";
  ULONG size = 0; query(process, 60, nullptr, 0, &size);
  struct Text { USHORT length, maximum; wchar_t* buffer; };
  if (size < sizeof(Text) || size > 256 * 1024) return L"";
  std::vector<std::byte> data(size);
  if (query(process, 60, data.data(), size, &size) < 0) return L"";
  const auto text = reinterpret_cast<const Text*>(data.data());
  return text->buffer && text->length ? std::wstring(text->buffer, text->length / sizeof(wchar_t)) : L"";
}
#ifdef TASKMGR_DIAGNOSTICS
Icons::Icons(HWND window, int iconSize, bool measure) : target(window), size(iconSize), measured(measure) {
#else
Icons::Icons(HWND window, int iconSize) : target(window), size(iconSize) {
#endif
  images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 128, 64);
  // Stock rows borrow Windows' own Task Manager icons: generic window, service, service host and user.
  const auto taskmgr = windowsDirectory() + L"\\System32\\Taskmgr.exe";
  for (const int index : {1, 2, 3, 5}) { HICON icon = nullptr; if (FAILED(SHDefExtractIconW(taskmgr.c_str(), index, 0, &icon, nullptr, MAKELONG(size, size))) || !icon) icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION)); ImageList_AddIcon(images, icon); DestroyIcon(icon); }
  for (auto& worker : workers) worker = std::thread([this] { run(); });
  enricher = std::thread([this] { enrich(); });
}
Icons::~Icons() {
  { std::lock_guard lock(mutex); stopping = true; } signal.notify_all();
  for (auto& worker : workers) if (worker.joinable()) worker.join();
  if (enricher.joinable()) enricher.join();
  for (const auto& [path, icon] : extracted) { static_cast<void>(path); if (icon) DestroyIcon(icon); }
  for (auto& result : results) if (result.icon) DestroyIcon(result.icon);
  if (images) ImageList_Destroy(images);
}
HICON Icons::extract(const std::wstring& path) {
  const auto key = lower(path); HICON icon = nullptr;
  { std::lock_guard lock(mutex); if (auto found = extracted.find(key); found != extracted.end()) return found->second ? CopyIcon(found->second) : nullptr; }
  if (!path.starts_with(L"\\\\") && (FAILED(SHDefExtractIconW(path.c_str(), 0, 0, &icon, nullptr, MAKELONG(size, size))) || !icon)) { icon = nullptr; ExtractIconExW(path.c_str(), 0, nullptr, &icon, 1); }
  std::lock_guard lock(mutex); if (extracted.size() < 1023 && !extracted.contains(key)) extracted.emplace(key, icon ? CopyIcon(icon) : nullptr);
  return icon;
}
void Icons::request(const Process& process, bool urgent) {
  if (process.id <= 4) return;
  std::lock_guard lock(mutex);
  const Identity identity{process.id, process.created};
  if (requested.contains(identity)) {
    if (urgent) { auto found = std::find_if(requests.begin(), requests.end(), [&](const Request& request) { return request.pid == process.id && request.created == process.created; }); if (found != requests.end()) { auto request = std::move(*found); requests.erase(found); requests.push_front(std::move(request)); } }
    return;
  }
  if (requested.size() >= 8192) return;
  requested.insert(identity);
  Request request{process.id, process.created};
  if (urgent) requests.push_front(std::move(request)); else requests.push_back(std::move(request));
  signal.notify_all();
}
const FileInfo& Icons::file(const std::wstring& path) {
  static const FileInfo empty;
  const auto key = lower(path); if (key.empty()) return empty;
  if (auto found = files.find(key); found != files.end()) return found->second;
  std::lock_guard lock(mutex);
  if (requestedFiles.size() < 4096 && requestedFiles.insert(key).second) { requests.push_back({0, 0, path}); signal.notify_all(); }
  return empty;
}
void Icons::run() {
  for (;;) {
    Request request;
    { std::unique_lock lock(mutex); signal.wait(lock, [&] { return stopping || !requests.empty(); }); if (stopping) return; request = std::move(requests.front()); requests.pop_front(); }
    Result result{request.pid, request.created, request.path}; result.file = request.pid == 0;
#ifdef TASKMGR_DIAGNOSTICS
    result.queued = request.queued;
#endif
    if (!result.file) {
      Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.pid));
      FILETIME created{}, exited{}, kernel{}, user{};
      if (process.value && (!GetProcessTimes(process.value, &created, &exited, &kernel, &user) || ticks(created) != request.created)) continue;
      std::wstring path(32768, L'\0'); DWORD length = DWORD(path.size());
      if (process.value && QueryFullProcessImageNameW(process.value, 0, path.data(), &length)) path.resize(length); else path = kernelImagePath(request.pid);
      if (path.empty()) continue;
      result.path = path; result.metadata.path = path;
    }
    const auto key = lower(result.path);
    std::pair<std::wstring, std::wstring> strings; bool cached = false;
    { std::lock_guard lock(mutex); if (auto found = versionCache.find(key); found != versionCache.end()) { strings = found->second; cached = true; } }
    if (!cached && !result.path.starts_with(L"\\\\")) { strings = versionStrings(result.path); std::lock_guard lock(mutex); if (versionCache.size() < 4096) versionCache.emplace(key, strings); }
    result.metadata.description = strings.first; result.metadata.publisher = strings.second;
    result.icon = extract(result.path);
#ifdef TASKMGR_DIAGNOSTICS
    result.latency = milliseconds(request.queued);
#endif
    bool notify = false;
    { std::lock_guard lock(mutex); if (stopping) { if (result.icon) DestroyIcon(result.icon); return; } notify = results.empty(); if (!result.file && enrichment.size() < 8192) enrichment.push_back({result.pid, result.created, result.path, result.metadata}); results.push_back(std::move(result)); }
    signal.notify_all();
    if (notify) PostMessageW(target, IconMessage, 0, 0);
  }
}
// Slower per-process details: account, UAC virtualization, command line and package identity.
void Icons::enrich() {
  for (;;) {
    Result request;
    { std::unique_lock lock(mutex); signal.wait(lock, [&] { return stopping || !enrichment.empty(); }); if (stopping) return; request = std::move(enrichment.front()); enrichment.pop_front(); }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.pid)); FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user) || ticks(created) != request.created) continue;
    auto metadata = std::move(request.metadata);
    metadata.commandLine = commandLine(process.value);
    UINT32 length = 0; if (GetPackageFamilyName(process.value, &length, nullptr) == ERROR_INSUFFICIENT_BUFFER && length > 0 && length < 1024) { std::wstring family(length, L'\0'); if (GetPackageFamilyName(process.value, &length, family.data()) == ERROR_SUCCESS) { family.resize(wcslen(family.c_str())); metadata.package = family; } }
    Handle token;
    if (OpenProcessToken(process.value, TOKEN_QUERY, &token.value)) {
      DWORD allowed = 0, enabled = 0, returned = 0;
      metadata.virtualization = GetTokenInformation(token.value, TokenVirtualizationAllowed, &allowed, sizeof(allowed), &returned) && allowed ? GetTokenInformation(token.value, TokenVirtualizationEnabled, &enabled, sizeof(enabled), &returned) && enabled ? L"Enabled" : L"Disabled" : L"Not allowed";
      DWORD needed = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &needed);
      if (needed && needed <= 65536) {
        std::vector<std::byte> data(needed);
        if (GetTokenInformation(token.value, TokenUser, data.data(), needed, &needed)) {
          wchar_t name[256]{}, domain[256]{}; DWORD nameSize = 256, domainSize = 256; SID_NAME_USE type{};
          if (LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, name, &nameSize, domain, &domainSize, &type)) metadata.user = name;
        }
      }
    }
    metadata.resolved = true;
    bool notify = false;
    { std::lock_guard lock(mutex); if (stopping) return; notify = results.empty(); results.push_back({request.pid, request.created, std::move(request.path), std::move(metadata)}); }
    if (notify) PostMessageW(target, IconMessage, 0, 0);
  }
}
void Icons::consume() {
  std::deque<Result> ready;
  { std::lock_guard lock(mutex); ready.swap(results); }
  for (auto& result : ready) {
    const auto path = lower(result.path);
    int index = -1;
    if (auto found = paths.find(path); found != paths.end()) index = found->second;
    else if (result.icon && paths.size() < 2047) { index = ImageList_AddIcon(images, result.icon); if (index >= 0) paths.emplace(path, index); }
    if (result.icon) {
      DestroyIcon(result.icon);
#ifdef TASKMGR_DIAGNOSTICS
      if (measured) { latencies.push_back(result.latency); if (latencies.size() > 4096) latencies.erase(latencies.begin(), latencies.begin() + 2048); }
#endif
    }
#ifdef TASKMGR_DIAGNOSTICS
    if (measured && result.queued != Time{} && !result.file) { if (displayLatencies.size() == 4096) displayLatencies.erase(displayLatencies.begin()); displayLatencies.push_back(milliseconds(result.queued)); }
#endif
    if (result.file) { if (files.size() < 4096) files[path] = {result.metadata.description, result.metadata.publisher, index}; continue; }
    const Identity key{result.pid, result.created};
    auto& entry = entries[key];
    if (result.metadata.resolved) { const int icon = entry.icon; entry = std::move(result.metadata); entry.icon = icon ? icon : std::max(0, index); }
    else { entry.path = std::move(result.metadata.path); entry.description = std::move(result.metadata.description); entry.publisher = std::move(result.metadata.publisher); entry.icon = std::max(0, index); }
  }
}
const Metadata& Icons::get(const Process& process) const {
  static const Metadata empty;
  auto found = entries.find({process.id, process.created}); return found != entries.end() ? found->second : empty;
}
}
