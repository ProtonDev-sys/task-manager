#pragma once
#include <charconv>
#include <cmath>
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
  Report& jsonString(std::string_view value) {
    *this << '"';
    for (const unsigned char character : value) {
      if (character == '"' || character == '\\') *this << '\\' << char(character);
      else if (character < 32) { constexpr char hex[] = "0123456789abcdef"; *this << "\\u00" << hex[character >> 4] << hex[character & 15]; }
      else *this << char(character);
    }
    return *this << '"';
  }
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
    // JSON numbers always use a period, regardless of the calling thread's locale.
    // Keep invalid metrics syntactically valid while failing the report operation.
    if (!std::isfinite(value)) { *this << "null"; validNumbers = false; return *this; }
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 6);
    if (converted.ec != std::errc{}) healthy = false;
    else *this << std::string_view(buffer, size_t(converted.ptr - buffer));
    return *this;
  }
  explicit operator bool() { if (!file || fflush(file) != 0) healthy = false; return healthy && validNumbers; }
private:
  FILE* file = nullptr;
  bool healthy = true, validNumbers = true;
};
inline bool writeDiagnosticError(const std::wstring& path, std::string_view message) {
  Report stream{path};
  stream << "{\"native\":true,\"passed\":false,\"error\":";
  stream.jsonString(message) << '}';
  return bool(stream);
}
}
