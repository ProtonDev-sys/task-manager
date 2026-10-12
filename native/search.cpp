#include "app.hpp"
#include "search_query.hpp"

namespace taskmgr {
static bool searchContains(const wchar_t* text, const SearchTerm& term) { return text && *text && !term.text.empty() && StrStrIW(text, term.text.c_str()) != nullptr; }
static bool searchContains(const std::wstring& text, const SearchTerm& term) { return searchContains(text.c_str(), term); }
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
  const auto query = parseSearchQuery(searchText);
  if (query.terms.empty()) return source;
  const auto validProcess = [&](int index) { return current && index >= 0 && size_t(index) < current->processes.size(); };
  // Expanded rows and collapsed groups share members. Match each process once
  // per query/snapshot, without caching borrowed metadata across UI messages.
  std::vector<int8_t> processResults(current ? current->processes.size() : 0, -1);
  auto processMatches = [&](int index) {
    if (!validProcess(index)) return false;
    auto& cached = processResults[size_t(index)];
    if (cached >= 0) return cached != 0;
    const auto& process = current->processes[size_t(index)];
    const Metadata* metadata = nullptr;
    const auto data = [&]() -> const Metadata& { if (!metadata) metadata = &icons->get(process); return *metadata; };
    const auto status = process.hung ? L"Not responding" : process.suspended ? L"Suspended" : L"Running";
    const auto userMatches = [&](const SearchTerm& term) {
      if (!data().user.empty()) return searchContains(data().user, term);
      if (current->users) { const auto found = current->users->find(process.id); if (found != current->users->end() && searchContains(found->second, term)) return true; }
      return process.id <= 4 && searchContains(L"SYSTEM", term);
    };
    const auto matches = [&](const SearchTerm& term) {
      switch (term.field) {
      case SearchField::Pid: return term.pid && process.id == *term.pid;
      case SearchField::Name: return searchContains(process.name, term) || searchContains(data().description, term);
      case SearchField::Publisher: return searchContains(data().publisher, term);
      case SearchField::Command: return searchContains(data().commandLine, term);
      case SearchField::Path: return searchContains(data().path, term);
      case SearchField::User: return userMatches(term);
      case SearchField::Status: return searchContains(status, term);
      case SearchField::Any:
        return searchContains(process.name, term) || (term.numeric && searchContains(std::to_wstring(process.id), term)) || searchContains(data().description, term) || searchContains(data().publisher, term) || searchContains(data().commandLine, term) || searchContains(data().path, term) || userMatches(term) || searchContains(status, term);
      }
      return false;
    };
    cached = std::all_of(query.terms.begin(), query.terms.end(), [&](const SearchTerm& term) { return matches(term) != term.excluded; }) ? 1 : 0;
    return cached != 0;
  };
  const auto rowTermMatches = [&](const Row& row, const SearchTerm& term) {
    const auto at = [&](int index) { return searchContains(cell(row, index), term); };
    switch (term.field) {
    case SearchField::Any: return std::any_of(row.cells.begin(), row.cells.end(), [&](const Cell& value) { return searchContains(value.text, term); });
    case SearchField::Name: return at(0);
    case SearchField::Pid: {
      const int column = selectedTab == ProcessesTab ? 4 : selectedTab == DetailsTab || selectedTab == ServicesTab ? 1 : -1;
      const auto pid = searchPid(cell(row, column)); return term.pid && pid && *term.pid == *pid;
    }
    case SearchField::Publisher: return at(selectedTab == ProcessesTab ? 3 : selectedTab == StartupTab ? 1 : -1);
    case SearchField::Command: return at(selectedTab == ProcessesTab ? 6 : selectedTab == DetailsTab ? 19 : selectedTab == StartupTab ? 5 : -1);
    case SearchField::Path:
      if (selectedTab == DetailsTab) return at(18);
      if (selectedTab == StartupTab && current && current->startup && row.entry >= 0 && size_t(row.entry) < current->startup->entries.size()) return searchContains(current->startup->entries[size_t(row.entry)].path, term);
      if (selectedTab == HistoryTab) { const auto found = usage.find(row.key); return found != usage.end() && searchContains(found->second.path, term); }
      return false;
    case SearchField::User: return at(selectedTab == DetailsTab ? 3 : selectedTab == StartupTab ? 6 : selectedTab == UsersTab ? 0 : -1);
    case SearchField::Status: return at(selectedTab == ProcessesTab || selectedTab == StartupTab || selectedTab == DetailsTab ? 2 : selectedTab == UsersTab || selectedTab == ServicesTab ? 3 : -1);
    }
    return false;
  };
  auto matches = [&](const Row& row) {
    if (!query.scoped && rowTermMatches(row, query.terms.front())) return true;
    const bool sessionFields = selectedTab == UsersTab && row.kind == RowKind::User && std::all_of(query.terms.begin(), query.terms.end(), [](const SearchTerm& term) { return term.field == SearchField::User || term.field == SearchField::Status || term.field == SearchField::Any; });
    if (query.scoped && sessionFields && std::all_of(query.terms.begin(), query.terms.end(), [&](const SearchTerm& term) { return rowTermMatches(row, term) != term.excluded; })) return true;
    bool hasProcess = validProcess(row.process);
    if (processMatches(row.process)) return true;
    for (const int member : row.members) if (member != row.process) { hasProcess = hasProcess || validProcess(member); if (processMatches(member)) return true; }
    // A process-backed row requires one actual member to satisfy every scoped
    // term. Aggregated cells must not combine a PID with another member's name,
    // or let an empty group PID bypass an exclusion.
    return query.scoped && !hasProcess && std::all_of(query.terms.begin(), query.terms.end(), [&](const SearchTerm& term) { return rowTermMatches(row, term) != term.excluded; });
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
LRESULT CALLBACK Application::searchProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context) {
  auto app = reinterpret_cast<Application*>(context);
  switch (message) {
  case WM_GETDLGCODE: {
    const auto input = reinterpret_cast<const MSG*>(data);
    return DefSubclassProc(target, message, word, data) | (input && input->message == WM_KEYDOWN && input->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0);
  }
  case WM_KEYDOWN:
    if (word == VK_RETURN) {
      if (!app->searchVisible || app->compact || app->summary || app->selectedTab == PerformanceTab) return 0;
      app->applySearch(); SetFocus(app->list);
      const int selected = app->selectedIndex();
      if (selected >= 0) ListView_EnsureVisible(app->list, selected, FALSE);
      else for (size_t index = 0; index < app->rows.size(); ++index) if (app->rows[index].kind != RowKind::Heading) { app->select(int(index)); break; }
      return 0;
    }
    break;
  case WM_CHAR: if (word == VK_RETURN) return 0; break;
  case WM_NCDESTROY: RemoveWindowSubclass(target, searchProcedure, id); break;
  }
  return DefSubclassProc(target, message, word, data);
}
}
