#pragma once
#include <charconv>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

namespace taskmgr {
class Report {
public:
  explicit Report(const std::wstring& path) { if (_wfopen_s(&file, path.c_str(), L"wb") != 0) healthy = false; }
  ~Report() { if (file) fclose(file); }
  Report(const Report&) = delete;
  Report& operator=(const Report&) = delete;
  Report& operator<<(std::string_view text) { if (healthy && (!file || fwrite(text.data(), 1, text.size(), file) != text.size())) healthy = false; return *this; }
  Report& operator<<(char value) { return *this << std::string_view(&value, 1); }
  template<class Integer> requires std::is_integral_v<Integer>
  Report& operator<<(Integer value) {
    char buffer[32]{};
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (converted.ec != std::errc{}) healthy = false;
    else *this << std::string_view(buffer, size_t(converted.ptr - buffer));
    return *this;
  }
  Report& operator<<(double value) {
    char buffer[64]{};
    const int count = _snprintf_s(buffer, sizeof(buffer), _TRUNCATE, "%.6g", value);
    if (count < 0) healthy = false;
    else *this << std::string_view(buffer, size_t(count));
    return *this;
  }
  explicit operator bool() { if (!file || fflush(file) != 0) healthy = false; return healthy; }
private:
  FILE* file = nullptr;
  bool healthy = true;
};
}
