#include "core.hpp"
#include <shlobj.h>

namespace taskmgr {
Icons::Icons(HWND window) : target(window) {
  images = ImageList_Create(16, 16, ILC_COLOR32 | ILC_MASK, 128, 64);
  ImageList_AddIcon(images, LoadIconW(nullptr, IDI_APPLICATION));
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
  Request request{process.id, process.created, process.name};
  if (urgent) requests.push_front(std::move(request)); else requests.push_back(std::move(request));
  signal.notify_all();
}
void Icons::run() {
  for (;;) {
    Request request;
    { std::unique_lock lock(mutex); signal.wait(lock, [&] { return stopping || !requests.empty(); }); if (stopping) return; request = std::move(requests.front()); requests.pop_front(); }
    const auto started = request.queued;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.pid));
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user) || ticks(created) != request.created) continue;
    Metadata metadata; std::wstring path(32768, L'\0'); DWORD size = DWORD(path.size());
    if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &size)) continue;
    path.resize(size); metadata.path = path;
    HICON smallIcon = nullptr;
    bool cached = false; const auto pathKey = lower(path);
    { std::lock_guard lock(mutex); if (auto found = extracted.find(pathKey); found != extracted.end()) { cached = true; if (found->second) smallIcon = CopyIcon(found->second); } }
    if (!cached && !path.starts_with(L"\\\\")) { HICON large = nullptr; ExtractIconExW(path.c_str(), 0, &large, &smallIcon, 1); if (large) DestroyIcon(large); std::lock_guard lock(mutex); if (extracted.size() < 1023 && !extracted.contains(pathKey)) extracted.emplace(pathKey, smallIcon ? CopyIcon(smallIcon) : nullptr); }
    auto deliver = [&](HICON icon, double latency) {
      bool notify = false;
      { std::lock_guard lock(mutex); if (stopping) { if (icon) DestroyIcon(icon); return; } notify = results.empty(); results.push_back({request.pid, request.created, metadata, icon, latency, request.queued}); }
      if (notify) PostMessageW(target, IconMessage, 0, 0);
    };
    deliver(smallIcon, milliseconds(started));
    { std::lock_guard lock(mutex); if (enrichment.size() < 8192) enrichment.push_back({request.pid, request.created, metadata}); } signal.notify_all();
  }
}
void Icons::enrich() {
  for (;;) {
    Result request;
    { std::unique_lock lock(mutex); signal.wait(lock, [&] { return stopping || !enrichment.empty(); }); if (stopping) return; request = std::move(enrichment.front()); enrichment.pop_front(); }
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, request.pid)); FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || !GetProcessTimes(process.value, &created, &exited, &kernel, &user) || ticks(created) != request.created) continue;
    auto metadata = request.metadata; const auto& path = metadata.path;
    if (path.starts_with(L"\\\\")) continue;
    const auto cached = versionCache.find(lower(path));
    if (cached != versionCache.end()) { metadata.publisher = cached->second.publisher; metadata.description = cached->second.description; }
    else {
    DWORD unused = 0; const DWORD versionSize = GetFileVersionInfoSizeW(path.c_str(), &unused);
    if (versionSize && versionSize <= 4 * 1024 * 1024) {
      std::vector<std::byte> version(versionSize);
      if (GetFileVersionInfoW(path.c_str(), 0, versionSize, version.data())) {
        struct Translation { WORD language, codepage; }; Translation* translations = nullptr; UINT translationSize = 0;
        if (VerQueryValueW(version.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &translationSize) && translationSize >= sizeof(Translation)) {
          for (const auto field : {L"FileDescription", L"CompanyName"}) {
            wchar_t query[128]{}; swprintf_s(query, L"\\StringFileInfo\\%04x%04x\\%s", translations[0].language, translations[0].codepage, field);
            wchar_t* text = nullptr; UINT length = 0;
            if (VerQueryValueW(version.data(), query, reinterpret_cast<void**>(&text), &length) && text && length) { if (wcscmp(field, L"FileDescription") == 0) metadata.description = text; else metadata.publisher = text; }
          }
        }
      }
    }
    if (versionCache.size() < 1023) versionCache.emplace(lower(path), metadata);
    }
    Handle token;
    if (OpenProcessToken(process.value, TOKEN_QUERY, &token.value)) {
      DWORD needed = 0; GetTokenInformation(token.value, TokenUser, nullptr, 0, &needed);
      if (needed && needed <= 65536) {
        std::vector<std::byte> data(needed);
        if (GetTokenInformation(token.value, TokenUser, data.data(), needed, &needed)) {
          wchar_t name[256]{}, domain[256]{}; DWORD nameSize = 256, domainSize = 256; SID_NAME_USE type{};
          if (LookupAccountSidW(nullptr, reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, name, &nameSize, domain, &domainSize, &type)) metadata.user = name;
        }
      }
    }
    bool notify = false;
    { std::lock_guard lock(mutex); if (stopping) return; notify = results.empty(); results.push_back({request.pid, request.created, metadata}); }
    if (notify) PostMessageW(target, IconMessage, 0, 0);
  }
}
void Icons::consume() {
  std::deque<Result> ready;
  { std::lock_guard lock(mutex); ready.swap(results); }
  for (auto& result : ready) {
    if (result.queued != Time{}) { if (displayLatencies.size() == 4096) displayLatencies.erase(displayLatencies.begin()); displayLatencies.push_back(milliseconds(result.queued)); }
    const Identity key{result.pid, result.created};
    const auto existing = entries.find(key);
    int index = existing != entries.end() ? existing->second.icon : 0;
    const auto path = lower(result.metadata.path);
    if (auto found = paths.find(path); found != paths.end()) index = found->second;
    else if (result.icon && paths.size() < 1023) { index = ImageList_AddIcon(images, result.icon); if (index < 0) index = 0; paths.emplace(path, index); }
    if (result.icon) { DestroyIcon(result.icon); latencies.push_back(result.latency); if (latencies.size() > 4096) latencies.erase(latencies.begin(), latencies.begin() + 2048); }
    result.metadata.icon = index; entries.insert_or_assign(key, std::move(result.metadata));
  }
}
const Metadata& Icons::get(const Process& process) const {
  static const Metadata empty;
  auto found = entries.find({process.id, process.created}); return found != entries.end() ? found->second : empty;
}
void Icons::prune(std::span<const Process> live) {
  std::unordered_set<Identity, IdentityHash> identities; identities.reserve(live.size());
  for (const auto& process : live) identities.insert({process.id, process.created});
  std::erase_if(entries, [&](const auto& item) { return !identities.contains(item.first); });
  std::lock_guard lock(mutex);
  std::erase_if(requested, [&](const auto& item) { return !identities.contains(item); });
  std::erase_if(requests, [&](const auto& request) { return !identities.contains({request.pid, request.created}); });
}
}
