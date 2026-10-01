// Separate from core.hpp: <lm.h> conflicts with the PDH headers.
#include <windows.h>
#include <lm.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace taskmgr {
std::wstring fullAccountName(const std::wstring& user) {
  static std::unordered_map<std::wstring, std::wstring> cache; static std::mutex gate; std::lock_guard lock(gate);
  if (const auto found = cache.find(user); found != cache.end()) return found->second;
  std::wstring name = user; USER_INFO_2* information = nullptr;
  if (NetUserGetInfo(nullptr, user.c_str(), 2, reinterpret_cast<BYTE**>(&information)) == NERR_Success && information) { if (information->usri2_full_name && *information->usri2_full_name) name = std::wstring(information->usri2_full_name) + L" (" + user + L")"; NetApiBufferFree(information); }
  cache.emplace(user, name); return name;
}
}
