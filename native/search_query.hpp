#pragma once
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace taskmgr {
enum class SearchField { Any, Name, Pid, Publisher, Command, Path, User, Status };
struct SearchTerm {
  SearchField field = SearchField::Any;
  std::wstring text;
  bool excluded = false, numeric = false;
  std::optional<uint32_t> pid;
};
struct SearchQuery { std::vector<SearchTerm> terms; bool scoped = false; };
inline std::optional<uint32_t> searchPid(std::wstring_view text) {
  if (text.empty()) return std::nullopt;
  uint32_t result = 0;
  for (const wchar_t character : text) {
    if (character < L'0' || character > L'9') return std::nullopt;
    const auto digit = uint32_t(character - L'0');
    if (result > ((std::numeric_limits<uint32_t>::max)() - digit) / 10) return std::nullopt;
    result = result * 10 + digit;
  }
  return result;
}
inline SearchQuery parseSearchQuery(std::wstring_view text) {
  SearchQuery result;
  const auto space = [](wchar_t value) { return value == L' ' || value == L'\t' || value == L'\r' || value == L'\n'; };
  const auto field = [](std::wstring_view name) {
    const auto equals = [&](std::wstring_view expected) {
      if (name.size() != expected.size()) return false;
      for (size_t index = 0; index < name.size(); ++index) {
        const auto value = name[index];
        if ((value >= L'A' && value <= L'Z' ? value + (L'a' - L'A') : value) != expected[index]) return false;
      }
      return true;
    };
    if (equals(L"name")) return SearchField::Name;
    if (equals(L"pid")) return SearchField::Pid;
    if (equals(L"publisher")) return SearchField::Publisher;
    if (equals(L"command")) return SearchField::Command;
    if (equals(L"path")) return SearchField::Path;
    if (equals(L"user")) return SearchField::User;
    if (equals(L"status")) return SearchField::Status;
    return SearchField::Any;
  };
  const auto finish = [](SearchTerm& term) {
    term.numeric = !term.text.empty() && term.text.find_first_not_of(L"0123456789") == std::wstring::npos;
    if (term.field == SearchField::Pid) term.pid = searchPid(term.text);
  };
  for (size_t index = 0; index < text.size();) {
    while (index < text.size() && space(text[index])) ++index;
    if (index == text.size()) break;
    SearchTerm term;
    const bool excluded = text[index] == L'-';
    const size_t start = index + (excluded ? 1 : 0);
    size_t colon = start;
    while (colon < text.size() && text[colon] != L':' && text[colon] != L'"' && !space(text[colon])) ++colon;
    if (colon < text.size() && text[colon] == L':') term.field = field(text.substr(start, colon - start));
    if (term.field != SearchField::Any) { result.scoped = true; term.excluded = excluded; index = colon + 1; }
    bool quoted = false;
    while (index < text.size() && (quoted || !space(text[index]))) {
      const auto character = text[index++];
      if (character == L'"') quoted = !quoted;
      else term.text.push_back(character);
    }
    finish(term);
    result.terms.push_back(std::move(term));
  }
  // Ordinary searches retain their original literal phrase, including spaces,
  // quotes, Windows paths and command switches. Only known fields enable syntax.
  if (!result.scoped) {
    result.terms.clear();
    if (!text.empty()) { SearchTerm term; term.text = text; finish(term); result.terms.push_back(std::move(term)); }
  }
  return result;
}
}
