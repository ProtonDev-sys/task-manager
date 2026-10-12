#pragma once
#include <string>
#include <string_view>
#include <utility>

namespace taskmgr {
// Display strings are exported as text. Prefix formula-like values so opening
// process-controlled metadata in a spreadsheet does not interpret a formula.
inline bool csvNeedsTextPrefix(std::string_view value) {
  if (value.empty()) return false;
  if (value.front() == '\t' || value.front() == '\r' || value.front() == '\n' || value.front() == '\0') return true;
  const auto start = value.find_first_not_of(" \t\r\n\v\f");
  if (start == value.npos) return false;
  value.remove_prefix(start);
  return value.front() == '=' || value.front() == '+' || value.front() == '-' || value.front() == '@' ||
    value.starts_with("\xef\xbc\x9d") || value.starts_with("\xef\xbc\x8b") || value.starts_with("\xef\xbc\x8d") || value.starts_with("\xef\xbc\xa0");
}
class Csv {
  std::string output = "\xef\xbb\xbf";
  bool first = true;
public:
  void field(std::string_view value) {
    if (!first) output.push_back(',');
    first = false; output.push_back('"');
    if (csvNeedsTextPrefix(value)) output.push_back('\'');
    for (const auto character : value) {
      if (character == '"') output.push_back('"');
      if (character == '\0') output += "\xef\xbf\xbd";
      else output.push_back(character);
    }
    output.push_back('"');
  }
  void endRow() { output += "\r\n"; first = true; }
  std::string take() { return std::move(output); }
};
}
