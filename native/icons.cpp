#include "core.hpp"
#include <shlobj.h>
#include <appmodel.h>

namespace taskmgr {
template <typename Read> static std::wstring processImagePath(Read read) {
  // Most image paths fit here; only long paths need the maximum-size temporary allocation.
  std::array<wchar_t, 512> buffer{}; DWORD length = DWORD(buffer.size());
  const auto status = read(buffer.data(), &length);
  if (status == ERROR_SUCCESS) return length <= buffer.size() ? std::wstring(buffer.data(), length) : L"";
  if (status != ERROR_INSUFFICIENT_BUFFER) return L"";
  std::wstring path(32768, L'\0'); length = DWORD(path.size());
  if (read(path.data(), &length) != ERROR_SUCCESS || length > path.size()) return L"";
  path.resize(length); return path;
}
#ifdef TASKMGR_DIAGNOSTICS
static std::vector<std::pair<std::string, bool>> processImagePathTests() {
  std::vector<std::pair<std::string, bool>> checks;
  unsigned calls = 0; DWORD firstSize = 0, secondSize = 0;
  const auto read = [&](const std::wstring& expected) {
    calls = 0; firstSize = secondSize = 0;
    return processImagePath([&](wchar_t* output, DWORD* size) {
      if (++calls == 1) firstSize = *size; else secondSize = *size;
      if (expected.size() >= *size) { *size = UINT32_MAX; return DWORD(ERROR_INSUFFICIENT_BUFFER); }
      std::copy(expected.begin(), expected.end(), output); *size = DWORD(expected.size()); return DWORD(ERROR_SUCCESS);
    });
  };
  const std::wstring shortPath = L"C:\\Example\\Example.exe", longPath = L"C:\\" + std::wstring(3000, L'x') + L".exe";
  checks.emplace_back("icon_short_process_path_avoids_large_temporary", read(shortPath) == shortPath && calls == 1 && firstSize == 512);
  checks.emplace_back("icon_long_process_path_retries_within_windows_limit", read(longPath) == longPath && calls == 2 && firstSize == 512 && secondSize == 32768);
  checks.emplace_back("icon_oversized_process_path_stops_after_bounded_retry", read(std::wstring(32768, L'x')).empty() && calls == 2);
  calls = 0;
  const auto denied = processImagePath([&](wchar_t*, DWORD*) { ++calls; return DWORD(ERROR_ACCESS_DENIED); });
  checks.emplace_back("icon_unavailable_process_path_is_not_retried_by_pid", denied.empty() && calls == 1);
  const auto invalid = processImagePath([](wchar_t*, DWORD* length) { ++*length; return DWORD(ERROR_SUCCESS); });
  checks.emplace_back("icon_process_path_rejects_invalid_returned_length", invalid.empty());
  return checks;
}
#endif
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
Icons::Icons(HWND window, int iconSize, bool measure, bool startWorkers) : target(window), size(iconSize), measured(measure) {
#else
Icons::Icons(HWND window, int iconSize) : target(window), size(iconSize) {
#endif
  images = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, 128, 64);
  // Stock rows borrow Windows' own Task Manager icons: generic window, service, service host and user.
  const auto taskmgr = windowsDirectory() + L"\\System32\\Taskmgr.exe";
  for (const int index : {1, 2, 3, 5}) { HICON icon = nullptr; if (index == 1 || FAILED(SHDefExtractIconW(taskmgr.c_str(), index, 0, &icon, nullptr, MAKELONG(size, size))) || !icon) icon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION)); ImageList_AddIcon(images, icon); DestroyIcon(icon); }
#ifdef TASKMGR_DIAGNOSTICS
  if (!startWorkers) return;
#endif
  try {
    for (auto& worker : workers) worker = std::thread([this] { run(); });
    enricher = std::thread([this] { enrich(); });
  } catch (...) {
    { std::lock_guard lock(mutex); stopping = true; } signal.notify_all();
    for (auto& worker : workers) if (worker.joinable()) worker.join();
    if (images) ImageList_Destroy(images); images = nullptr;
    throw;
  }
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
const FileInfo& Icons::file(const std::wstring& path, bool urgent) {
  static const FileInfo empty;
  const auto key = lower(path); if (key.empty()) return empty;
  if (auto found = files.find(key); found != files.end()) return found->second;
  std::lock_guard lock(mutex);
  if (requestedFiles.size() < 4096 && requestedFiles.insert(key).second) { if (urgent) requests.push_front({0, 0, path}); else requests.push_back({0, 0, path}); signal.notify_all(); }
  else if (urgent) { auto found = std::find_if(requests.begin(), requests.end(), [&](const Request& request) { return request.pid == 0 && lower(request.path) == key; }); if (found != requests.end()) { auto request = std::move(*found); requests.erase(found); requests.push_front(std::move(request)); } }
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
      if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user) || ticks(created) != request.created) continue;
      // An unverified PID-only fallback can resolve a replacement process after PID reuse.
      result.path = processImagePath([&](wchar_t* path, DWORD* length) { return QueryFullProcessImageNameW(process.value, 0, path, length) ? DWORD(ERROR_SUCCESS) : GetLastError(); });
      if (result.path.empty()) continue;
      result.metadata.path = result.path;
    }
    { std::lock_guard lock(mutex); if (stopping) return; if (!accepts(result)) continue; }
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
    { std::lock_guard lock(mutex); if (stopping || !accepts(result)) { if (result.icon) DestroyIcon(result.icon); if (stopping) return; continue; } notify = results.empty(); if (!result.file && enrichment.size() < 8192) enrichment.push_back({result.pid, result.created, result.path, result.metadata}); results.push_back(std::move(result)); }
    signal.notify_all();
    if (notify) PostMessageW(target, IconMessage, 0, 0);
  }
}
// Slower per-process details: account, UAC virtualization, command line and package identity.
void Icons::enrich() {
  for (;;) {
    Result request;
    { std::unique_lock lock(mutex); signal.wait(lock, [&] { return stopping || !enrichment.empty(); }); if (stopping) return; request = std::move(enrichment.front()); enrichment.pop_front(); if (!accepts(request)) continue; }
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
    { std::lock_guard lock(mutex); if (stopping) return; if (!accepts(request)) continue; notify = results.empty(); results.push_back({request.pid, request.created, std::move(request.path), std::move(metadata)}); }
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
bool Icons::draw(HDC dc, int index, int left, int top, int pixels) const {
  IMAGELISTDRAWPARAMS draw{sizeof(draw)}; draw.himl = images; draw.i = index; draw.hdcDst = dc;
  draw.x = left; draw.y = top; draw.cx = draw.cy = pixels; draw.rgbBk = CLR_NONE; draw.rgbFg = CLR_DEFAULT; draw.fStyle = ILD_TRANSPARENT | ILD_SCALE;
  if (ImageList_DrawIndirect(&draw)) return true;
  draw.i = DefaultIcon; return ImageList_DrawIndirect(&draw) != FALSE;
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> iconCacheTests() {
  auto checks = processImagePathTests();
  // No worker threads or unrelated processes: inject the exact stages of an icon request.
  Icons icons(nullptr, 16, false, false);
  Process old, current; old.id = current.id = 10; old.created = 100; current.created = 200;
  const Identity oldIdentity{old.id, old.created}, currentIdentity{current.id, current.created};
  icons.entries[oldIdentity].description = L"Old process"; icons.entries[currentIdentity].description = L"Current process";
  icons.requested = {oldIdentity, currentIdentity}; icons.requestedFiles.insert(L"example.exe");
  icons.requests = {{old.id, old.created}, {current.id, current.created}, {0, 0, L"example.exe"}};
  icons.enrichment = {{old.id, old.created}, {current.id, current.created}};
  const HICON obsoleteIcon = CopyIcon(LoadIconW(nullptr, IDI_APPLICATION));
  checks.emplace_back("icon_pruning_fixture_owns_icon", obsoleteIcon != nullptr);
  Icons::Result stale{old.id, old.created}; stale.icon = obsoleteIcon;
  Icons::Result fresh{current.id, current.created}; fresh.metadata.description = L"Fresh metadata";
  Icons::Result file{0, 0, L"example.exe"}; file.file = true; file.metadata.description = L"File metadata";
  icons.results = {stale, fresh, file};
  const std::unordered_set<Identity, IdentityHash> live{currentIdentity}; icons.prune(live);
  checks.emplace_back("icon_pruning_preserves_only_live_process_metadata", icons.entries.size() == 1 && icons.get(old).description.empty() && icons.get(current).description == L"Current process");
  checks.emplace_back("icon_pruning_discards_old_identity_requests", icons.requested.size() == 1 && icons.requested.contains(currentIdentity) && icons.requests.size() == 2 && icons.requests.front().created == current.created && icons.requests.back().pid == 0);
  checks.emplace_back("icon_pruning_discards_queued_enrichment", icons.enrichment.size() == 1 && icons.enrichment.front().created == current.created);
  checks.emplace_back("icon_pruning_discards_completed_stale_results", icons.results.size() == 2 && icons.results.front().created == current.created && icons.results.back().file);
  ICONINFO information{}; const bool retainedIcon = obsoleteIcon && GetIconInfo(obsoleteIcon, &information);
  if (information.hbmColor) DeleteObject(information.hbmColor); if (information.hbmMask) DeleteObject(information.hbmMask);
  checks.emplace_back("icon_pruning_releases_owned_stale_icon", obsoleteIcon && !retainedIcon);
  checks.emplace_back("icon_workers_reject_late_old_identity_completion", !icons.accepts(stale) && icons.accepts(fresh) && icons.accepts(file));
  icons.consume();
  checks.emplace_back("icon_live_result_is_consumed_without_resurrection", icons.entries.size() == 1 && icons.get(current).description == L"Fresh metadata" && icons.get(old).description.empty());
  checks.emplace_back("icon_pruning_keeps_file_metadata_requests", icons.file(L"EXAMPLE.EXE").description == L"File metadata" && icons.requestedFiles.contains(L"example.exe"));
  icons.prune(std::unordered_set<Identity, IdentityHash>{});
  checks.emplace_back("icon_empty_process_snapshot_clears_all_process_work", icons.entries.empty() && icons.requested.empty() && icons.enrichment.empty() && icons.results.empty() && icons.requests.size() == 1 && icons.requests.front().pid == 0);
  return checks;
}
#endif
}
