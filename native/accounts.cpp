// Separate from core.hpp: <lm.h> conflicts with the PDH headers.
#include <windows.h>
#include <lm.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace taskmgr {
std::wstring accountName(const std::wstring& user, const std::wstring& domain) { return domain.empty() ? user : domain + L"\\" + user; }
template <typename Lookup> static std::wstring resolveAccountName(const std::wstring& user, const std::wstring& domain, const std::wstring& computer, Lookup lookup) {
  const auto qualified = accountName(user, domain);
  // A local SAM lookup must not supply another domain's same-named account.
  if (user.empty() || domain.empty() || computer.empty() || CompareStringOrdinal(domain.c_str(), -1, computer.c_str(), -1, TRUE) != CSTR_EQUAL) return qualified;
  const auto full = lookup(user);
  return full.empty() ? qualified : full + L" (" + qualified + L")";
}
template <typename Lookup> static std::wstring cachedAccountName(const std::wstring& user, const std::wstring& domain, const std::wstring& computer, std::unordered_map<std::wstring, std::wstring>& cache, Lookup lookup) {
  const auto key = accountName(user, domain);
  if (const auto found = cache.find(key); found != cache.end()) return found->second;
  auto name = resolveAccountName(user, domain, computer, lookup);
  cache.emplace(key, name); return name;
}
std::wstring fullAccountName(const std::wstring& user, const std::wstring& domain) {
  static std::unordered_map<std::wstring, std::wstring> cache; static std::mutex gate; std::lock_guard lock(gate);
  static const auto computer = [] { wchar_t name[MAX_COMPUTERNAME_LENGTH + 1]{}; DWORD size = MAX_COMPUTERNAME_LENGTH + 1; return GetComputerNameW(name, &size) ? std::wstring(name, size) : std::wstring{}; }();
  return cachedAccountName(user, domain, computer, cache, [](const std::wstring& user) {
    std::wstring full; USER_INFO_2* information = nullptr;
    if (NetUserGetInfo(nullptr, user.c_str(), 2, reinterpret_cast<BYTE**>(&information)) == NERR_Success && information) { if (information->usri2_full_name) full = information->usri2_full_name; NetApiBufferFree(information); }
    return full;
  });
}
#ifdef TASKMGR_DIAGNOSTICS
std::vector<std::pair<std::string, bool>> accountNameTests() {
  std::vector<std::pair<std::string, bool>> checks; int lookups = 0;
  const auto lookup = [&](const std::wstring&) { ++lookups; return std::wstring(L"Local Example"); };
  checks.emplace_back("account_local_domain_resolves_full_name", resolveAccountName(L"alex", L"DESKTOP", L"DESKTOP", lookup) == L"Local Example (DESKTOP\\alex)" && lookups == 1);
  checks.emplace_back("account_domain_name_never_uses_local_namesake", resolveAccountName(L"alex", L"CORPORATE", L"DESKTOP", lookup) == L"CORPORATE\\alex" && lookups == 1);
  checks.emplace_back("account_local_domain_comparison_ignores_case", resolveAccountName(L"alex", L"desktop", L"DESKTOP", lookup) == L"Local Example (desktop\\alex)" && lookups == 2);
  checks.emplace_back("account_unknown_domain_does_not_query_local_sam", resolveAccountName(L"alex", L"", L"DESKTOP", lookup) == L"alex" && lookups == 2);
  checks.emplace_back("account_unknown_computer_does_not_query_local_sam", resolveAccountName(L"alex", L"DESKTOP", L"", lookup) == L"DESKTOP\\alex" && lookups == 2);
  checks.emplace_back("account_missing_full_name_keeps_qualified_identity", resolveAccountName(L"alex", L"DESKTOP", L"DESKTOP", [](const std::wstring&) { return std::wstring{}; }) == L"DESKTOP\\alex");
  std::unordered_map<std::wstring, std::wstring> cache; lookups = 0;
  const auto local = cachedAccountName(L"alex", L"DESKTOP", L"DESKTOP", cache, lookup), remote = cachedAccountName(L"alex", L"CORPORATE", L"DESKTOP", cache, lookup);
  checks.emplace_back("account_cache_keys_include_domain", local == L"Local Example (DESKTOP\\alex)" && remote == L"CORPORATE\\alex" && cache.size() == 2 && lookups == 1);
  checks.emplace_back("account_cached_local_name_avoids_repeated_lookup", cachedAccountName(L"alex", L"DESKTOP", L"DESKTOP", cache, lookup) == local && lookups == 1);
  return checks;
}
#endif
}
