#include "app.hpp"

namespace taskmgr {
void Application::toggleSearch() {
  if (compact || summary || selectedTab == PerformanceTab) return;
  if (searchVisible && GetFocus() == searchBox) {
    SetWindowTextW(searchBox, L""); applySearch(); searchVisible = false; SetFocus(list);
  } else { searchVisible = true; layout(); SetFocus(searchBox); SendMessageW(searchBox, EM_SETSEL, 0, -1); return; }
  layout();
}
void Application::applySearch() {
  KillTimer(window, 5);
  searchQueued = false;
  wchar_t query[257]{}; GetWindowTextW(searchBox, query, int(std::size(query)));
  const auto requested = lower(query);
  if (requested == searchText) return;
  searchText = requested;
  if (!compact) rebuild(false, true);
}
std::vector<Row> Application::filteredRows(const std::vector<Row>& source) const {
  std::vector<Row> result;
  const bool numericQuery = !searchText.empty() && searchText.find_first_not_of(L"0123456789") == std::wstring::npos;
  auto processMatches = [&](int index) {
    if (!current || index < 0 || size_t(index) >= current->processes.size()) return false;
    const auto& process = current->processes[size_t(index)];
    if (StrStrIW(process.name.c_str(), searchText.c_str()) || (numericQuery && std::to_wstring(process.id).find(searchText) != std::wstring::npos)) return true;
    const auto& metadata = icons->get(process);
    return StrStrIW(metadata.description.c_str(), searchText.c_str()) || StrStrIW(metadata.publisher.c_str(), searchText.c_str()) || StrStrIW(metadata.commandLine.c_str(), searchText.c_str());
  };
  auto matches = [&](const Row& row) {
    for (const auto& value : row.cells) if (!value.text.empty() && StrStrIW(value.text.c_str(), searchText.c_str())) return true;
    if (processMatches(row.process)) return true;
    for (const int member : row.members) if (member != row.process && processMatches(member)) return true;
    return false;
  };
  const Row* heading = nullptr;
  bool headingAdded = false;
  for (size_t index = 0; index < source.size();) {
    const auto& root = source[index];
    if (root.kind == RowKind::Heading) { heading = &root; headingAdded = false; ++index; continue; }
    size_t endIndex = index + 1;
    while (endIndex < source.size() && source[endIndex].kind != RowKind::Heading && source[endIndex].depth > root.depth) ++endIndex;
    bool matched = matches(root);
    for (size_t child = index + 1; !matched && child < endIndex; ++child) matched = matches(source[child]);
    if (matched) {
      if (heading && !headingAdded) { result.push_back(*heading); headingAdded = true; }
      result.insert(result.end(), source.begin() + ptrdiff_t(index), source.begin() + ptrdiff_t(endIndex));
    }
    index = endIndex;
  }
  return result;
}
}
