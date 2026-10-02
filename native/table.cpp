#include "app.hpp"
#include <windowsx.h>

namespace taskmgr {
// Windows 10 Task Manager heat map and selection colours, sampled from the stock application.
static constexpr COLORREF Heat[] = {RGB(255, 244, 196), RGB(249, 236, 168), RGB(255, 228, 135), RGB(255, 210, 100), RGB(255, 189, 85), RGB(255, 167, 69), RGB(252, 141, 55)};
static constexpr COLORREF Selection = RGB(205, 232, 255), Hover = RGB(229, 243, 255), HeaderText = RGB(76, 96, 122), HeadingText = RGB(31, 89, 195);
static const wchar_t* PowerNames[] = {L"Very low", L"Low", L"Moderate", L"High", L"Very high"};
static COLORREF blend(COLORREF over, COLORREF under, double amount) {
  auto mix = [&](int top, int bottom) { return int(std::lround(bottom + (top - bottom) * amount)); };
  return RGB(mix(GetRValue(over), GetRValue(under)), mix(GetGValue(over), GetGValue(under)), mix(GetBValue(over), GetBValue(under)));
}
static int level(double value, double total, bool zero) {
  if (zero) return 0; const double share = value / std::max(1e-9, total);
  return share < 0.10 ? 1 : share < 0.20 ? 2 : share < 0.40 ? 3 : share < 0.60 ? 4 : share < 0.80 ? 5 : 6;
}
static std::wstring percent(double value) { return value < 0.05 ? L"0%" : number(value, 1) + L"%"; }
static std::wstring megabytes(double value) { return grouped(value / 1048576, 1) + L" MB"; }
static std::wstring kilobytes(double value) { return grouped(std::floor(value / 1024), 0) + L" K"; }
static std::wstring elapsedText(uint64_t seconds) { wchar_t text[32]{}; swprintf_s(text, L"%llu:%02llu:%02llu", seconds / 3600, seconds / 60 % 60, seconds % 60); return text; }
std::span<const ColumnDef> Application::definitions(int tab) {
  static const ColumnDef processes[] = {{L"Name", 357, false, true, false}, {L"Type", 130, false, false, false}, {L"Status", 120, false, true, false}, {L"Publisher", 160, false, false, false}, {L"PID", 50, true, false, false},
    {L"Process name", 130, false, false, false}, {L"Command line", 300, false, false, false}, {L"CPU", 73, true, true, true}, {L"Memory", 132, true, true, true}, {L"Disk", 73, true, true, true}, {L"Network", 73, true, true, true},
    {L"GPU", 73, true, true, true}, {L"GPU engine", 130, false, true, false}, {L"Power usage", 100, false, true, true}, {L"Power usage trend", 110, false, true, true}};
  static const ColumnDef performance[] = {{L"Name", 100, false, true, false}};
  static const ColumnDef history[] = {{L"Name", 300, false, true, false}, {L"CPU time", 110, true, true, false}};
  static const ColumnDef startup[] = {{L"Name", 250, false, true, false}, {L"Publisher", 200, false, true, false}, {L"Status", 100, false, true, false}, {L"Startup impact", 110, false, true, false}, {L"Startup type", 110, false, false, false}, {L"Command line", 300, false, false, false}, {L"User", 180, false, false, false}, {L"Source location", 350, false, false, false}};
  static const ColumnDef users[] = {{L"User", 250, false, true, false}, {L"ID", 50, true, false, false}, {L"Session", 90, false, false, false}, {L"Status", 110, false, true, false}, {L"CPU", 73, true, true, true}, {L"Memory", 90, true, true, true}, {L"Disk", 73, true, true, true}, {L"Network", 73, true, true, true}};
  static const ColumnDef details[] = {{L"Name", 200, false, true, false}, {L"PID", 60, true, true, false}, {L"Status", 80, false, true, false}, {L"User name", 100, false, true, false}, {L"Session ID", 70, true, false, false}, {L"CPU", 40, true, true, false},
    {L"CPU time", 80, true, false, false}, {L"Memory (active private working set)", 110, true, true, false}, {L"Memory (private working set)", 110, true, false, false}, {L"Memory (commit size)", 110, true, false, false}, {L"Memory (working set)", 110, true, false, false},
    {L"Memory (peak working set)", 110, true, false, false}, {L"Page faults", 90, true, false, false}, {L"Base priority", 90, false, false, false}, {L"Handles", 70, true, false, false}, {L"Threads", 70, true, false, false}, {L"I/O read bytes", 110, true, false, false},
    {L"I/O write bytes", 110, true, false, false}, {L"Image path name", 300, false, false, false}, {L"Command line", 300, false, false, false}, {L"UAC virtualization", 100, false, true, false}, {L"Description", 280, false, true, false}, {L"GPU", 50, true, false, false}, {L"GPU engine", 110, false, false, false}};
  static const ColumnDef services[] = {{L"Name", 200, false, true, false}, {L"PID", 60, true, true, false}, {L"Description", 300, false, true, false}, {L"Status", 80, false, true, false}, {L"Group", 200, false, true, false}};
  switch (tab) { case ProcessesTab: return processes; case HistoryTab: return history; case StartupTab: return startup; case UsersTab: return users; case DetailsTab: return details; case ServicesTab: return services; default: return performance; }
}
int Application::rowHeight() const { return scale(compact ? 24 : selectedTab == DetailsTab || selectedTab == ServicesTab ? 22 : 28); }
int Application::columnAt(int index) const { const auto& ids = shown[size_t(selectedTab)]; return compact ? 0 : index >= 0 && size_t(index) < ids.size() ? ids[size_t(index)] : -1; }
int Application::minimumColumnWidth(int tab, int column) const {
  const auto defs = definitions(tab);
  if (column < 0 || size_t(column) >= defs.size()) return 48;
  const auto& definition = defs[size_t(column)];
  return column == 0 ? 120 : definition.heat ? 60 : definition.right ? 48 : std::clamp(definition.width / 2, 64, 100);
}
void Application::columns(bool preserveRows) {
  columnsTab = selectedTab;
  columnGeometryDirty = true; hotHeader = -1;
  rebuilding = true; SendMessageW(list, WM_SETREDRAW, FALSE, 0);
  if (!preserveRows) { orderedTab = -1; ListView_SetItemCountEx(list, 0, LVSICF_NOINVALIDATEALL); rows.clear(); hotRow = -1; }
  while (Header_GetItemCount(header)) ListView_DeleteColumn(list, 0);
  const auto previousStyle = GetWindowLongPtrW(list, GWL_STYLE);
  const auto style = compact ? previousStyle | LVS_NOCOLUMNHEADER : previousStyle & ~LONG_PTR(LVS_NOCOLUMNHEADER);
  if (style != previousStyle) SetWindowLongPtrW(list, GWL_STYLE, style);
  if (!preserveRows) {
    int spacerWidth = 0, spacerHeight = 0;
    if (!rowSpacer || !ImageList_GetIconSize(rowSpacer, &spacerWidth, &spacerHeight) || spacerHeight != rowHeight() - 1) {
      const auto previous = rowSpacer;
      rowSpacer = ImageList_Create(1, rowHeight() - 1, ILC_COLOR32, 1, 1); ListView_SetImageList(list, rowSpacer, LVSIL_SMALL);
      if (previous) ImageList_Destroy(previous);
    }
  }
  const auto defs = definitions(selectedTab);
  if (compact) { RECT area{}; GetClientRect(window, &area); LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH; column.pszText = const_cast<wchar_t*>(L"Name"); column.cx = std::max(scale(100), int(area.right) - GetSystemMetrics(SM_CXVSCROLL) - scale(4)); ListView_InsertColumn(list, 0, &column); }
  else for (size_t index = 0; index < shown[size_t(selectedTab)].size(); ++index) {
    const int id = shown[size_t(selectedTab)][index]; const auto& definition = defs[size_t(id)];
    LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT; column.pszText = const_cast<wchar_t*>(definition.title); column.cx = scale(std::max(minimumColumnWidth(selectedTab, id), index < widths[size_t(selectedTab)].size() ? widths[size_t(selectedTab)][index] : definition.width));
    column.fmt = index && (definition.right || definition.heat) ? LVCFMT_RIGHT : LVCFMT_LEFT; ListView_InsertColumn(list, int(index), &column);
  }
  RECT client{}; GetClientRect(list, &client); SendMessageW(list, WM_SIZE, 0, MAKELPARAM(client.right, client.bottom));
  if (style != previousStyle) SetWindowPos(list, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  SendMessageW(list, WM_SETREDRAW, TRUE, 0); rebuilding = false; InvalidateRect(list, nullptr, TRUE);
}
void Application::updateColumnGeometry() {
  if (!columnGeometryDirty) return;
  displayCount = std::clamp(Header_GetItemCount(header), 0, int(displayOrder.size()));
  if (displayCount && !ListView_GetColumnOrderArray(list, displayCount, displayOrder.data())) { displayCount = 0; return; }
  for (int position = 0; position < displayCount; ++position) displayWidths[size_t(position)] = ListView_GetColumnWidth(list, displayOrder[size_t(position)]);
  columnGeometryDirty = false;
}
double Application::powerScore(const Process& process) const { return process.cpu + process.gpu * 0.5 + process.ioRate / 1048576 * 0.2 + std::max(0.0, process.network) / 125000 * 0.1; }
static int powerLevel(double score) { return score < 0.5 ? 0 : score < 3 ? 1 : score < 10 ? 2 : score < 25 ? 3 : 4; }
struct Aggregate { double cpu = 0, memory = 0, disk = 0, network = 0, gpu = 0, power = 0, trend = 0; std::wstring engine, status; };
std::wstring Application::processName(int index, const std::unordered_map<DWORD, std::vector<const Entry*>>& services) const {
  const auto& process = current->processes[size_t(index)]; const auto& metadata = icons->get(process);
  if (process.id == 0) return L"System Idle Process";
  if (process.id == 4) return L"System";
  if (lower(process.name) == L"svchost.exe") {
    const auto found = services.find(process.id);
    if (found == services.end() || found->second.empty()) return L"Service Host";
    if (found->second.size() == 1) return L"Service Host: " + found->second.front()->description;
    return L"Service Host: " + serviceGroupCaption(found->second.front()->group) + L" (" + std::to_wstring(found->second.size()) + L")";
  }
  return metadata.description.empty() ? process.name : metadata.description;
}
void Application::processCells(Row& row, std::span<const int> members) const {
  Aggregate total; bool suspended = !members.empty(), hung = false;
  for (const int index : members) {
    const auto& process = current->processes[size_t(index)];
    total.cpu += process.cpu; total.memory += double(process.privateWorking); total.disk += process.ioRate; if (process.network >= 0) total.network += process.network;
    if (process.gpu > total.gpu) total.engine = process.gpuEngine; total.gpu += process.gpu; suspended = suspended && process.suspended; hung = hung || process.hung;
    const auto tracked = activity.find({process.id, process.created}); total.trend += tracked == activity.end() ? 0 : tracked->second.trend;
  }
  if (row.kind == RowKind::Interrupts) total.cpu = current->interrupts;
  total.gpu = std::min(100.0, total.gpu); total.power = total.cpu + total.gpu * 0.5 + total.disk / 1048576 * 0.2 + total.network / 125000 * 0.1;
  total.status = hung ? L"Not responding" : suspended ? L"Suspended" : L"";
  const double used = std::max(1.0, double(current->memory.total - current->memory.available));
  double linkSpeed = 0; for (const auto& adapter : current->networks) linkSpeed = std::max(linkSpeed, double(adapter.speed) / 8); if (linkSpeed <= 0) linkSpeed = 125000000;
  const auto cpu = Cell{percent(total.cpu), total.cpu, level(total.cpu, 100, total.cpu < 0.05)};
  const auto memory = Cell{memoryPercent ? percent(100 * total.memory / std::max(1.0, double(current->memory.total))) : megabytes(total.memory), total.memory, level(total.memory, used, total.memory < 0.05 * 1048576)};
  const auto disk = Cell{total.disk < 0.05 * 1048576 ? L"0 MB/s" : number(total.disk / 1048576, 1) + L" MB/s", total.disk, level(total.disk, 1073741824, total.disk < 0.05 * 1048576)};
  const auto network = !current->networkAttribution ? Cell{L"—", -1, 0} : Cell{networkPercent ? percent(100 * total.network / linkSpeed) : total.network * 8 < 50000 ? L"0 Mbps" : number(total.network * 8 / 1000000, 1) + L" Mbps", total.network, level(total.network, linkSpeed, total.network * 8 < 50000)};
  const auto gpu = Cell{percent(total.gpu), total.gpu, level(total.gpu, 100, total.gpu < 0.05)};
  if (selectedTab == UsersTab) { row.cells.resize(8); row.cells[4] = cpu; row.cells[5] = memory; row.cells[6] = disk; row.cells[7] = network; if (row.kind != RowKind::User) row.cells[3] = {total.status}; return; }
  row.cells.resize(15);
  row.cells[2] = {total.status}; row.cells[7] = cpu; row.cells[8] = memory; row.cells[9] = disk; row.cells[10] = network; row.cells[11] = gpu; row.cells[12] = {total.engine};
  const int power = powerLevel(total.power), trend = powerLevel(total.trend);
  row.cells[13] = {PowerNames[power], double(power), power + 1}; row.cells[14] = {PowerNames[trend], double(trend), trend + 1};
}
static bool rowLess(const Row& left, const Row& right, int column, bool direction, bool numeric) {
    static const Cell empty;
    int compare = 0; const auto& first = left.cells.size() > size_t(column) ? left.cells[size_t(column)] : empty; const auto& second = right.cells.size() > size_t(column) ? right.cells[size_t(column)] : empty;
    if (numeric) compare = first.value < second.value ? -1 : first.value > second.value ? 1 : 0;
    else compare = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, first.text.c_str(), -1, second.text.c_str(), -1, nullptr, nullptr, 0) - CSTR_EQUAL;
    if (!compare && column != 0 && !left.cells.empty() && !right.cells.empty()) compare = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, left.cells[0].text.c_str(), -1, right.cells[0].text.c_str(), -1, nullptr, nullptr, 0) - CSTR_EQUAL;
    if (!compare) compare = left.key.compare(right.key);
    return direction ? compare < 0 : compare > 0;
}
void Application::sortRows(std::vector<Row>& items) const {
  const int column = sortColumn[size_t(selectedTab)]; const bool direction = ascending[size_t(selectedTab)];
  const auto defs = definitions(selectedTab); const bool numeric = column > 0 && size_t(column) < defs.size() && (defs[size_t(column)].right || defs[size_t(column)].heat);
  std::sort(items.begin(), items.end(), [&](const Row& left, const Row& right) { return rowLess(left, right, column, direction, numeric); });
}
void Application::sortVisibleRows() {
  const bool reverse = orderedTab == selectedTab && orderedColumn == sortColumn[size_t(selectedTab)] && orderedAscending != ascending[size_t(selectedTab)];
  if (compact || (selectedTab != ProcessesTab && selectedTab != UsersTab)) { if (reverse) std::reverse(rows.begin(), rows.end()); else sortRows(rows); return; }
  const int column = sortColumn[size_t(selectedTab)]; const bool direction = ascending[size_t(selectedTab)];
  const auto defs = definitions(selectedTab); const bool numeric = column > 0 && size_t(column) < defs.size() && (defs[size_t(column)].right || defs[size_t(column)].heat);
  auto less = [&](const Row& left, const Row& right) { return rowLess(left, right, column, direction, numeric); };
  struct Block { size_t start, end; };
  std::vector<Block> blocks; blocks.reserve(rows.size());
  for (size_t start = 0; start < rows.size();) {
    size_t blockEnd = start + 1;
    if (rows[start].kind != RowKind::Heading) while (blockEnd < rows.size() && rows[blockEnd].depth > 0) ++blockEnd;
    size_t children = start + 1;
    while (children < blockEnd && rows[children].kind == RowKind::Window) ++children;
    if (children < blockEnd && rows[children].kind == RowKind::Process) { if (reverse) std::reverse(rows.begin() + children, rows.begin() + blockEnd); else std::sort(rows.begin() + children, rows.begin() + blockEnd, less); }
    blocks.push_back({start, blockEnd}); start = blockEnd;
  }
  for (size_t start = 0; start < blocks.size();) {
    if (rows[blocks[start].start].kind == RowKind::Heading) { ++start; continue; }
    size_t blockEnd = start + 1;
    while (blockEnd < blocks.size() && rows[blocks[blockEnd].start].kind != RowKind::Heading) ++blockEnd;
    if (reverse) std::reverse(blocks.begin() + start, blocks.begin() + blockEnd);
    else std::sort(blocks.begin() + start, blocks.begin() + blockEnd, [&](const Block& left, const Block& right) { return less(rows[left.start], rows[right.start]); });
    start = blockEnd;
  }
  std::vector<Row> sorted; sorted.reserve(rows.size());
  for (const auto& block : blocks) for (size_t index = block.start; index < block.end; ++index) sorted.push_back(std::move(rows[index]));
  rows.swap(sorted);
}
const std::unordered_map<DWORD, std::vector<const Entry*>>& Application::servicesByProcess() {
  if (current->inventory && indexedInventory == current->inventory) return indexedServices;
  std::unordered_map<DWORD, std::vector<const Entry*>> result;
  for (const auto& service : current->services) if (service.pid) result[service.pid].push_back(&service);
  for (auto& [pid, hosted] : result) std::sort(hosted.begin(), hosted.end(), [](const Entry* left, const Entry* right) { return CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE, left->description.c_str(), -1, right->description.c_str(), -1, nullptr, nullptr, 0) == CSTR_LESS_THAN; });
  indexedServices.swap(result);
  indexedInventory = current->inventory;
  return indexedServices;
}
static std::wstring identityKey(const Process& process) { return std::to_wstring(process.id) + L":" + std::to_wstring(process.created); }
void Application::buildProcesses(std::vector<Row>& next) {
  const auto& processes = current->processes; const auto& services = servicesByProcess();
  std::unordered_map<DWORD, int> byId; byId.reserve(processes.size());
  for (size_t index = 0; index < processes.size(); ++index) byId[processes[index].id] = int(index);
  std::vector<std::pair<std::wstring, bool>> owners(processes.size()); std::vector<char> known(processes.size());
  std::function<const std::pair<std::wstring, bool>&(int, int)> owner = [&](int index, int depth) -> const std::pair<std::wstring, bool>& {
    if (known[size_t(index)]) return owners[size_t(index)];
    const auto& process = processes[size_t(index)]; const auto& metadata = icons->get(process);
    std::pair<std::wstring, bool> result{L"pid:" + std::to_wstring(process.id), process.app};
    if (!metadata.package.empty()) result = {L"package:" + metadata.package, process.app};
    else if (const auto parent = byId.find(process.parent); depth < 64 && process.id > 4 && parent != byId.end() && process.parent != process.id && process.parent > 4 && processes[size_t(parent->second)].created <= process.created) {
      const auto& parentOwner = owner(parent->second, depth + 1); const auto& parentProcess = processes[size_t(parent->second)];
      const bool sameImage = lower(parentProcess.name) == lower(process.name), shell = lower(parentProcess.name) == L"explorer.exe" && !sameImage;
      if (parentOwner.second && (!process.app || sameImage) && (sameImage || !shell)) result = parentOwner;
    }
    known[size_t(index)] = 1; owners[size_t(index)] = std::move(result); return owners[size_t(index)];
  };
  std::map<std::wstring, std::vector<int>> groups;
  for (size_t index = 0; index < processes.size(); ++index) { const auto& process = processes[index]; if (process.id == 0 || process.name == L"Memory Compression") continue; groups[owner(int(index), 0).first].push_back(int(index)); }
  struct Top { Row row; std::vector<Row> children; };
  std::vector<Top> tops; tops.reserve(groups.size() + 1);
  for (auto& [key, members] : groups) {
    int root = members.front();
    if (key.starts_with(L"pid:")) { const auto found = byId.find(DWORD(_wtoi(key.c_str() + 4))); if (found != byId.end()) root = found->second; }
    else root = *std::min_element(members.begin(), members.end(), [&](int left, int right) { const auto& a = processes[size_t(left)]; const auto& b = processes[size_t(right)]; return a.app != b.app ? a.app : a.created < b.created; });
    const auto& rootProcess = processes[size_t(root)]; const auto& metadata = icons->get(rootProcess);
    Top top; auto& row = top.row; row.process = root; row.members = members; row.icon = lower(rootProcess.name) == L"svchost.exe" ? Icons::ServiceHostIcon : metadata.icon;
    const bool app = std::any_of(members.begin(), members.end(), [&](int index) { return processes[size_t(index)].app; });
    // Windows processes are the operating system's own infrastructure, not everything installed under the Windows directory.
    static const std::unordered_set<std::wstring> system{L"registry", L"secure system", L"smss.exe", L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe", L"lsass.exe", L"lsaiso.exe", L"svchost.exe", L"dwm.exe", L"fontdrvhost.exe", L"explorer.exe"};
    row.category = app ? 0 : rootProcess.id <= 4 || system.contains(lower(rootProcess.name)) ? 2 : 1;
    processCells(row, members);
    auto name = processName(root, services);
    const auto hosted = services.find(rootProcess.id);
    if (app && !compact) {
      row.expandable = true;
      if (expanded.contains(members.size() > 1 ? L"g:" + identityKey(rootProcess) : L"p:" + identityKey(rootProcess)))
        for (const int member : members) for (const auto& visibleWindow : current->windows) if (visibleWindow.pid == processes[size_t(member)].id && !visibleWindow.title.empty()) {
          Row child; child.kind = RowKind::Window; child.process = member; child.window = visibleWindow.window; child.depth = 1; child.icon = icons->get(processes[size_t(member)]).icon; child.key = L"w:" + std::to_wstring(reinterpret_cast<ULONG_PTR>(visibleWindow.window));
          child.cells.resize(15); child.cells[0] = {visibleWindow.title}; for (const int heat : {7, 8, 9, 10, 11, 13, 14}) child.cells[size_t(heat)].heat = 0; top.children.push_back(std::move(child));
        }
    }
    if (members.size() > 1) {
      row.kind = RowKind::Group; row.key = L"g:" + identityKey(rootProcess); row.expandable = true; name += L" (" + std::to_wstring(members.size()) + L")";
      if (expanded.contains(row.key)) for (const int member : members) { Row child; child.kind = RowKind::Process; child.process = member; child.members = {member}; child.depth = 1; child.key = L"p:" + identityKey(processes[size_t(member)]); child.icon = lower(processes[size_t(member)].name) == L"svchost.exe" ? Icons::ServiceHostIcon : icons->get(processes[size_t(member)]).icon; processCells(child, child.members); child.cells[0] = {processName(member, services)}; top.children.push_back(std::move(child)); }
    } else {
      row.kind = RowKind::Process; row.key = L"p:" + identityKey(rootProcess);
      if (hosted != services.end() && !app) { row.expandable = true; if (expanded.contains(row.key)) for (const auto service : hosted->second) { Row child; child.kind = RowKind::Service; child.process = root; child.depth = 1; child.icon = Icons::ServiceIcon; child.key = L"s:" + service->key; child.cells.resize(15); child.cells[0] = {service->description}; for (const int heat : {7, 8, 9, 10, 11, 13, 14}) child.cells[size_t(heat)].heat = 0; top.children.push_back(std::move(child)); } }
    }
    row.cells[0] = {name};
    row.cells[1] = {row.category == 0 ? L"App" : row.category == 1 ? L"Background process" : L"Windows process"}; row.cells[3] = {metadata.publisher};
    row.cells[4] = members.size() > 1 ? Cell{L"", 0} : Cell{std::to_wstring(rootProcess.id), double(rootProcess.id)}; row.cells[5] = {rootProcess.name}; row.cells[6] = {metadata.commandLine};
    row.expanded = row.expandable && expanded.contains(row.key);
    for (auto& child : top.children) if (child.kind == RowKind::Process) { const auto& process = processes[size_t(child.process)]; const auto& childMetadata = icons->get(process); child.cells[1] = row.cells[1]; child.cells[3] = {childMetadata.publisher}; child.cells[4] = {std::to_wstring(process.id), double(process.id)}; child.cells[5] = {process.name}; child.cells[6] = {childMetadata.commandLine}; }
    tops.push_back(std::move(top));
  }
  if (!compact) { Top interrupts; interrupts.row.kind = RowKind::Interrupts; interrupts.row.key = L"interrupts"; interrupts.row.category = 2; interrupts.row.icon = Icons::DefaultIcon; processCells(interrupts.row, {}); interrupts.row.cells[0] = {L"System interrupts"}; interrupts.row.cells[1] = {L"Windows process"}; interrupts.row.cells[4] = {L"-", 0}; interrupts.row.cells[8] = {megabytes(0), 0, 0}; tops.push_back(std::move(interrupts)); }
  std::unordered_map<std::wstring, size_t> positions; for (size_t index = 0; index < tops.size(); ++index) positions[tops[index].row.key] = index;
  std::vector<Row> roots; roots.reserve(tops.size()); for (auto& top : tops) roots.push_back(std::move(top.row));
  sortRows(roots);
  if (compact) { for (auto& row : roots) if (row.category == 0) { row.depth = 0; row.expandable = false; next.push_back(std::move(row)); } return; }
  const bool byType = groupByType && sortColumn[ProcessesTab] == 0;
  std::array<int, 3> counts{}; for (const auto& row : roots) ++counts[size_t(row.category)];
  static const wchar_t* headings[] = {L"Apps", L"Background processes", L"Windows processes"};
  for (int category = byType ? 0 : -1; category < (byType ? 3 : 0); ++category) {
    if (byType && counts[size_t(category)] == 0) continue;
    if (byType) { Row heading; heading.kind = RowKind::Heading; heading.key = L"h:" + std::to_wstring(category); heading.cells.resize(15); heading.cells[0] = {std::wstring(headings[category]) + L" (" + std::to_wstring(counts[size_t(category)]) + L")"}; for (const int heat : {7, 8, 9, 10, 11, 13, 14}) heading.cells[size_t(heat)].heat = 0; next.push_back(std::move(heading)); }
    for (auto& row : roots) {
      if (byType && row.category != category) continue;
      auto children = std::move(tops[positions[row.key]].children);
      next.push_back(std::move(row));
      if (children.empty()) continue;
      const auto windowsEnd = std::stable_partition(children.begin(), children.end(), [](const Row& child) { return child.kind == RowKind::Window; });
      if (windowsEnd != children.end() && windowsEnd->kind == RowKind::Process) { std::vector<Row> processRows(std::make_move_iterator(windowsEnd), std::make_move_iterator(children.end())); children.erase(windowsEnd, children.end()); sortRows(processRows); for (auto& child : processRows) children.push_back(std::move(child)); }
      for (auto& child : children) next.push_back(std::move(child));
    }
  }
}
void Application::buildUsers(std::vector<Row>& next) {
  const auto& services = servicesByProcess();
  std::vector<Row> users;
  for (size_t entry = 0; entry < current->sessions.size(); ++entry) {
    const auto& session = current->sessions[entry]; Row row; row.kind = RowKind::User; row.entry = int(entry); row.key = session.key; row.icon = Icons::UserIcon; row.expandable = true;
    for (size_t index = 0; index < current->processes.size(); ++index) if (current->processes[index].session == session.session && current->processes[index].id > 4) row.members.push_back(int(index));
    processCells(row, row.members);
    row.cells[0] = {(fullName ? fullAccountName(session.cells[0]) : session.cells[0]) + L" (" + std::to_wstring(row.members.size()) + L")"}; row.cells[1] = {session.cells[2], double(session.session)}; row.cells[2] = {session.cells[3]}; row.cells[3] = {session.cells[1]};
    row.expanded = expanded.contains(row.key); users.push_back(std::move(row));
  }
  sortRows(users);
  for (auto& user : users) {
    next.push_back(user);
    if (!user.expanded) continue;
    std::vector<Row> children;
    for (const int index : user.members) { const auto& process = current->processes[size_t(index)]; Row child; child.kind = RowKind::Process; child.process = index; child.members = {index}; child.depth = 1; child.key = L"p:" + identityKey(process); child.icon = icons->get(process).icon; processCells(child, child.members); child.cells[0] = {processName(index, services)}; child.cells[1] = {L""}; child.cells[2] = {L""}; children.push_back(std::move(child)); }
    sortRows(children); for (auto& child : children) next.push_back(std::move(child));
  }
}
void Application::buildDetails(std::vector<Row>& next) {
  const auto& users = current->users;
  for (size_t index = 0; index < current->processes.size(); ++index) {
    const auto& process = current->processes[index]; const auto& metadata = icons->get(process);
    Row row; row.kind = RowKind::Process; row.process = int(index); row.members = {int(index)}; row.key = L"p:" + identityKey(process); row.icon = metadata.icon; row.cells.resize(24);
    std::wstring user = metadata.user; if (user.empty() && users) { const auto found = users->find(process.id); if (found != users->end()) user = found->second; } if (user.empty() && process.id <= 4) user = L"SYSTEM";
    const int cpu = std::min(99, int(std::lround(process.cpu))); wchar_t cpuText[8]{}; swprintf_s(cpuText, L"%02d", cpu);
    const wchar_t* priority = process.priority >= 24 ? L"Realtime" : process.priority >= 13 ? L"High" : process.priority >= 10 ? L"Above normal" : process.priority >= 8 ? L"Normal" : process.priority >= 6 ? L"Below normal" : process.priority >= 1 ? L"Low" : L"N/A";
    const double active = process.suspended ? 0 : double(process.privateWorking);
    row.cells[0] = {process.name}; row.cells[1] = {std::to_wstring(process.id), double(process.id)}; row.cells[2] = {process.hung ? L"Not responding" : process.suspended ? L"Suspended" : L"Running"}; row.cells[3] = {user};
    row.cells[4] = {std::to_wstring(process.session), double(process.session)}; row.cells[5] = {cpuText, process.cpu}; row.cells[6] = {elapsedText(process.cpuTicks / 10000000), double(process.cpuTicks)};
    row.cells[7] = {kilobytes(active), active}; row.cells[8] = {kilobytes(double(process.privateWorking)), double(process.privateWorking)}; row.cells[9] = {kilobytes(double(process.privateBytes)), double(process.privateBytes)};
    row.cells[10] = {kilobytes(double(process.working)), double(process.working)}; row.cells[11] = {kilobytes(double(process.peakWorking)), double(process.peakWorking)}; row.cells[12] = {grouped(process.pageFaults), double(process.pageFaults)};
    row.cells[13] = {priority, double(process.priority)}; row.cells[14] = {grouped(process.handles), double(process.handles)}; row.cells[15] = {grouped(process.threads), double(process.threads)};
    row.cells[16] = {grouped(double(process.readBytes)), double(process.readBytes)}; row.cells[17] = {grouped(double(process.writeBytes)), double(process.writeBytes)}; row.cells[18] = {metadata.path}; row.cells[19] = {metadata.commandLine};
    row.cells[20] = {metadata.virtualization.empty() ? L"Not allowed" : metadata.virtualization}; row.cells[21] = {process.id == 0 ? L"Percentage of time the processor is idle" : process.id == 4 ? L"NT Kernel & System" : metadata.description};
    row.cells[22] = {std::to_wstring(int(std::lround(process.gpu))), process.gpu}; row.cells[23] = {process.gpuEngine};
    next.push_back(std::move(row));
  }
  sortRows(next);
}
void Application::buildEntries(std::vector<Row>& next) {
  if (selectedTab == HistoryTab) {
    for (const auto& [key, value] : usage) {
      if (value.ticks < 10000000 || (!allHistory && !value.app)) continue;
      Row row; row.kind = RowKind::Entry; row.key = key; const auto& file = icons->file(value.path); row.icon = file.icon >= 0 ? file.icon : Icons::DefaultIcon;
      row.cells = {{value.name}, {elapsedText(value.ticks / 10000000), double(value.ticks)}}; next.push_back(std::move(row));
    }
  } else {
    const auto& entries = selectedTab == StartupTab ? current->startup->entries : current->services;
    for (size_t index = 0; index < entries.size(); ++index) {
      const auto& entry = entries[index]; Row row; row.kind = RowKind::Entry; row.entry = int(index); row.key = entry.key;
      if (selectedTab == ServicesTab) { row.icon = Icons::ServiceIcon; for (const auto& text : entry.cells) row.cells.push_back({text}); row.cells[1].value = entry.pid; }
      else { const auto& file = icons->file(entry.path, true); row.icon = file.icon >= 0 ? file.icon : Icons::DefaultIcon; for (size_t cell = 0; cell < 8; ++cell) { const auto source = cell < 6 ? cell : cell + 1; row.cells.push_back({source < entry.cells.size() ? entry.cells[source] : L""}); } if (row.cells[1].text.empty()) row.cells[1].text = file.publisher; }
      next.push_back(std::move(row));
    }
  }
  sortRows(next);
}
void Application::rebuild(bool sortOnly, bool filterOnly) {
  if (!current || !list || (selectedTab == PerformanceTab && !compact)) return;
  KillTimer(window, 3);
  const bool manualSort = sortOnly || sortQueued;
  if (sortOnly && !compact && selectedTab == ProcessesTab && groupByType && ((orderedColumn == 0) != (sortColumn[ProcessesTab] == 0))) sortOnly = false;
  sortQueued = false;
  if (sortOnly && orderedTab == selectedTab && orderedColumn == sortColumn[size_t(selectedTab)] && orderedAscending == ascending[size_t(selectedTab)]) return;
#ifdef TASKMGR_DIAGNOSTICS
  const auto begin = Clock::now();
  if (hidden()) { if (sortOnly) ++sortPasses; else if (!filterOnly) ++fullRebuilds; }
#endif
  const auto selected = rowKey(selectedIndex()); const int top = ListView_GetTopIndex(list); const auto topKey = rowKey(top);
  rebuilding = true; std::vector<Row> next;
  bool partialUpdate = false;
  if (filterOnly) {
    if (searchText.empty()) { rows = std::move(searchRows); searchRows = std::vector<Row>{}; }
    else { if (searchRows.empty()) searchRows = std::move(rows); rows = filteredRows(searchRows); }
    orderedTab = -1;
    sortVisibleRows();
  }
  else if (sortOnly) sortVisibleRows();
  else if (compact || selectedTab == ProcessesTab) buildProcesses(next);
  else if (selectedTab == UsersTab) buildUsers(next);
  else if (selectedTab == DetailsTab) buildDetails(next);
  else if (selectedTab != PerformanceTab) buildEntries(next);
  if (!sortOnly && !filterOnly) {
    if (searchText.empty() || compact || selectedTab == PerformanceTab) {
      partialUpdate = !manualSort && orderedTab == selectedTab && rows.size() == next.size() && std::equal(rows.begin(), rows.end(), next.begin(), [](const Row& previous, const Row& incoming) { return previous.key == incoming.key; });
      rows.swap(next); searchRows = std::vector<Row>{};
    }
    else { searchRows = std::move(next); rows = filteredRows(searchRows); }
  }
  orderedTab = selectedTab; orderedColumn = sortColumn[size_t(selectedTab)]; orderedAscending = ascending[size_t(selectedTab)];
  if (!partialUpdate) { SendMessageW(list, WM_SETREDRAW, FALSE, 0); if (!sortOnly) ListView_SetItemCountEx(list, int(rows.size()), LVSICF_NOINVALIDATEALL | LVSICF_NOSCROLL); }
  int found = -1; if (!selected.empty()) for (size_t index = 0; index < rows.size(); ++index) if (rows[index].key == selected) { found = int(index); break; }
  if (found >= 0) { if (selectedIndex() != found) select(found); } else if (selectedIndex() >= 0) ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  if (manualSort) { const int offset = ListView_GetTopIndex(list); if (offset) ListView_Scroll(list, 0, -offset * rowHeight()); }
  else if (!partialUpdate && sortColumn[size_t(selectedTab)] == 0 && !topKey.empty()) for (size_t index = 0; index < rows.size(); ++index) if (rows[index].key == topKey) { const int delta = int(index) - ListView_GetTopIndex(list); if (delta) ListView_Scroll(list, 0, delta * rowHeight()); break; }
  if (partialUpdate) {
    const int first = std::max(0, ListView_GetTopIndex(list)), last = std::min(int(rows.size()), first + ListView_GetCountPerPage(list) + 1);
    for (int index = first; index < last; ++index) if (!sameRowPaint(next[size_t(index)], rows[size_t(index)])) {
      RECT bounds{}; if (ListView_GetItemRect(list, index, &bounds, LVIR_BOUNDS)) InvalidateRect(list, &bounds, FALSE);
#ifdef TASKMGR_DIAGNOSTICS
      if (hidden() && measured) ++invalidatedRows;
#endif
    }
#ifdef TASKMGR_DIAGNOSTICS
    if (hidden() && measured) ++partialTableUpdates;
#endif
  } else { SendMessageW(list, WM_SETREDRAW, TRUE, 0); InvalidateRect(list, nullptr, FALSE); }
  rebuilding = false;
  if (!sortOnly && !filterOnly) {
    const int visible = std::max(0, ListView_GetTopIndex(list)), count = std::max(1, ListView_GetCountPerPage(list));
    for (int index = std::min(int(rows.size()), visible + count + 1) - 1; index >= visible; --index) for (const int member : rows[size_t(index)].members) icons->request(current->processes[size_t(member)], true);
    for (const auto& process : current->processes) icons->request(process);
  }
  if (!sortOnly) updateFooter();
  if (manualSort) InvalidateRect(header, nullptr, FALSE);
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden()) (sortOnly ? sortTimes : updateTimes).push_back(milliseconds(begin));
#endif
}
const Process* Application::selectedProcess() const { const auto row = selectedRow(); return current && row && row->process >= 0 && row->kind != RowKind::Heading ? &current->processes[size_t(row->process)] : nullptr; }
std::vector<const Process*> Application::selectedMembers() const {
  std::vector<const Process*> result; const auto row = selectedRow();
  if (current && row && (row->kind == RowKind::Group || row->kind == RowKind::Process || row->kind == RowKind::Window)) for (const int member : row->members) result.push_back(&current->processes[size_t(member)]);
  return result;
}
void Application::select(int index) {
  if (index < 0 || size_t(index) >= rows.size() || rows[size_t(index)].kind == RowKind::Heading) return;
  ListView_SetItemState(list, -1, 0, LVIS_SELECTED | LVIS_FOCUSED); ListView_SetItemState(list, index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED); ListView_EnsureVisible(list, index, FALSE);
}
void Application::toggle(int index) {
  if (index < 0 || size_t(index) >= rows.size() || !rows[size_t(index)].expandable) return;
  const auto key = rows[size_t(index)].key; if (!expanded.erase(key)) expanded.insert(key);
  rebuild();
}
void Application::invalidateRow(int index) { if (index >= 0 && size_t(index) < rows.size()) ListView_RedrawItems(list, index, index); }
void Application::letter(wchar_t character) {
#ifdef TASKMGR_DIAGNOSTICS
  const auto begin = Clock::now();
#endif
  if (character < L' ' || rows.empty()) return;
  const wchar_t folded = wchar_t(towlower(character)); const bool expired = prefixAt == Time{} || Clock::now() - prefixAt > std::chrono::seconds(1);
  const bool repeat = !expired && prefix.size() == 1 && prefix[0] == folded;
  if (expired || repeat) prefix.assign(1, folded); else prefix.push_back(folded); prefixAt = Clock::now();
  int found = findRow(prefix, selectedIndex() + (prefix.size() == 1 ? 1 : 0));
  if (found < 0 && prefix.size() > 1) { prefix.assign(1, folded); found = findRow(prefix, selectedIndex() + 1); }
  select(found);
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden()) inputTimes.push_back(milliseconds(begin));
#endif
}
int Application::findRow(std::wstring_view query, int start) const {
  if (rows.empty() || query.empty()) return -1;
  const int count = int(rows.size()), first = (start % count + count) % count;
  for (int offset = 0; offset < count; ++offset) { const int index = (first + offset) % count; const auto& row = rows[size_t(index)]; if (row.kind != RowKind::Heading && startsWithInsensitive(cell(row, 0), query)) return index; }
  return -1;
}
void Application::copy() {
  std::wstring text;
  if (selectedTab == PerformanceTab && !compact) text = performanceSummary();
  else {
    const auto row = selectedRow(); if (!row) return;
    const int count = Header_GetItemCount(header); std::vector<int> order(size_t(std::max(1, count))); if (count > 0) ListView_GetColumnOrderArray(list, count, order.data());
    for (int position = 0; position < std::max(1, count); ++position) { if (position) text += L'\t'; text += cell(*row, columnAt(order[size_t(position)])); }
  }
  if (!OpenClipboard(window)) return; HGLOBAL storage = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
  if (storage) { void* memory = GlobalLock(storage); if (memory) { memcpy(memory, text.c_str(), (text.size() + 1) * sizeof(wchar_t)); GlobalUnlock(storage); EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, storage)) GlobalFree(storage); } else GlobalFree(storage); }
  CloseClipboard();
}
// GDI+ is scoped to the chevron itself: a Graphics object kept alive across GDI calls on the same DC corrupts their output.
static void chevron(HDC dc, float x, float y, float unit, bool open) {
  Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); Gdiplus::Pen pen(Gdiplus::Color(255, 128, 128, 128), 1.0f);
  if (open) { const Gdiplus::PointF points[] = {{x - unit, y - 2 * unit}, {x + 3 * unit, y + 2 * unit}, {x + 7 * unit, y - 2 * unit}}; graphics.DrawLines(&pen, points, 3); }
  else { const Gdiplus::PointF points[] = {{x + unit, y - 4 * unit}, {x + 5 * unit, y}, {x + unit, y + 4 * unit}}; graphics.DrawLines(&pen, points, 3); }
}
void Application::drawRow(HDC dc, int index, RECT bounds) {
  RECT client{}; GetClientRect(list, &client);
  RECT tail{bounds.right, bounds.top, client.right, bounds.bottom}; FillRect(dc, &tail, backgroundBrush);
  if (index < 0 || size_t(index) >= rows.size()) { FillRect(dc, &bounds, backgroundBrush); return; }
  const auto& row = rows[size_t(index)]; const bool selected = ListView_GetItemState(list, index, LVIS_SELECTED) != 0, hot = index == hotRow && row.kind != RowKind::Heading;
  updateColumnGeometry(); const auto defs = definitions(selectedTab); const int count = displayCount;
  SetBkMode(dc, TRANSPARENT); SelectObject(dc, font);
  const bool tree = (selectedTab == ProcessesTab || selectedTab == UsersTab) && !compact; const int iconSize = scale(16), indent = row.depth * scale(20);
  int x = bounds.left;
  for (int position = 0; position < std::max(1, count); ++position) {
    const int lvIndex = displayOrder[size_t(position)], id = columnAt(lvIndex); const int width = compact ? bounds.right - bounds.left : displayWidths[size_t(position)];
    RECT box{x, bounds.top, x + width, bounds.bottom}; x += width;
    if (box.left >= client.right) break;
    if (box.right <= client.left) continue;
    if (id < 0 || size_t(id) >= defs.size()) continue;
    const auto& definition = defs[size_t(id)]; const Cell empty; const auto& value = size_t(id) < row.cells.size() ? row.cells[size_t(id)] : empty;
    const bool heat = definition.heat && value.heat >= 0 && !compact && !highContrast;
    COLORREF background = heat ? themeColor(Heat[std::clamp(value.heat, 0, 6)]) : themeColor(RGB(255, 255, 255));
    if (selected) background = heat ? blend(background, themeColor(Selection), 0.25) : themeColor(Selection); else if (hot) background = heat ? blend(background, themeColor(Hover), 0.25) : themeColor(Hover);
    SetDCBrushColor(dc, background); FillRect(dc, &box, GetStockBrush(DC_BRUSH));
    if (heat) { SetDCBrushColor(dc, blend(themeColor(RGB(190, 140, 130)), background, 0.25)); RECT edge{box.right - 1, box.top, box.right, box.bottom}; FillRect(dc, &edge, GetStockBrush(DC_BRUSH)); edge = {box.left - 1, box.top, box.left, box.bottom}; FillRect(dc, &edge, GetStockBrush(DC_BRUSH)); }
    if (row.kind == RowKind::Heading) {
      if (id == 0) { RECT text{box.left + scale(10), box.top + scale(4), client.right, box.bottom}; SelectObject(dc, headingFont); SetTextColor(dc, themeColor(HeadingText)); DrawTextW(dc, value.text.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); SelectObject(dc, font); }
      continue;
    }
    RECT text{box.left + scale(6), box.top, box.right - scale(6), box.bottom};
    if (id == 0) {
      int iconLeft = box.left + scale(6);
      if (tree) {
        if (row.expandable) chevron(dc, float(box.left + scale(11) + indent), float(box.top + (box.bottom - box.top) / 2), float(scale(100)) / 100, row.expanded);
        iconLeft = box.left + scale(28) + indent;
      } else if (compact) iconLeft = box.left + scale(8);
      if (!ImageList_Draw(icons->images, row.icon, dc, iconLeft, box.top + (box.bottom - box.top - iconSize) / 2, ILD_TRANSPARENT)) ImageList_Draw(icons->images, Icons::DefaultIcon, dc, iconLeft, box.top + (box.bottom - box.top - iconSize) / 2, ILD_TRANSPARENT);
      text.left = iconLeft + iconSize + scale(6);
    }
    SetTextColor(dc, highContrast && (selected || hot) ? GetSysColor(COLOR_HIGHLIGHTTEXT) : themeColor(RGB(0, 0, 0)));
    DrawTextW(dc, value.text.c_str(), -1, &text, (definition.right || definition.heat) && id != 0 && !(definition.heat && !definition.right) ? DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX : DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
}
std::wstring Application::headerValue(int column) const {
  if (!current) return L"";
  const int offset = selectedTab == UsersTab ? 4 : 7; const int index = column - offset;
  double value = 0;
  switch (index) {
  case 0: value = current->cpu; break;
  case 1: value = current->memory.total ? 100.0 * double(current->memory.total - current->memory.available) / double(current->memory.total) : 0; break;
  case 2: for (const auto& disk : current->disks) value = std::max(value, disk.active); break;
  case 3: for (const auto& network : current->networks) if (network.speed) value = std::max(value, 800.0 * (network.send + network.receive) / double(network.speed)); break;
  case 4: if (selectedTab != ProcessesTab) return L""; for (const auto& gpu : current->gpus) value = std::max(value, gpu.usage); break;
  default: return L"";
  }
  return number(std::min(100.0, value), 0) + L"%";
}
void Application::paintHeader(HDC dc, RECT client) {
  FillRect(dc, &client, backgroundBrush); SetBkMode(dc, TRANSPARENT);
  updateColumnGeometry(); const auto defs = definitions(selectedTab); const bool twoLine = twoLineHeader();
  RECT clip{}; GetClipBox(dc, &clip);
  for (int position = 0; position < displayCount; ++position) {
    const int index = displayOrder[size_t(position)], id = columnAt(index); RECT box{}; Header_GetItemRect(header, index, &box);
    if (box.left >= clip.right) break;
    if (box.right <= clip.left) continue;
    if (id < 0 || size_t(id) >= defs.size()) continue;
    const auto& definition = defs[size_t(id)]; const bool right = (definition.right || definition.heat) && id != 0 && !(definition.heat && !definition.right);
    const auto value = twoLine && definition.heat ? headerValue(id) : L"";
    COLORREF background = index == hotHeader ? themeColor(Hover) : themeColor(RGB(255, 255, 255));
    if (!value.empty()) { const double amount = _wtof(value.c_str()); if (amount >= 90) background = themeColor(Heat[6]); else if (amount >= 80) background = themeColor(Heat[5]); }
    SetDCBrushColor(dc, background); RECT fill{box.left, box.top, box.right - 1, box.bottom - 1}; FillRect(dc, &fill, GetStockBrush(DC_BRUSH));
    if (dark) { RECT separator{box.right - 1, box.top + 1, box.right, box.bottom - 1}; SetDCBrushColor(dc, themeColor(RGB(64, 64, 64))); FillRect(dc, &separator, GetStockBrush(DC_BRUSH)); }
    else for (int y = box.top + 1; y < box.bottom - 1; ++y) { const double fraction = double(y - box.top) / std::max(1L, box.bottom - box.top); const int shade = int(232 - 46 * fraction); SetPixelV(dc, box.right - 1, y, themeColor(RGB(shade, shade, shade))); }
    if (!value.empty()) { RECT top{box.left + scale(4), box.top + scale(4), box.right - scale(7), box.top + scale(25)}; SelectObject(dc, percentFont); SetTextColor(dc, themeColor(RGB(0, 0, 0))); DrawTextW(dc, value.c_str(), -1, &top, DT_RIGHT | DT_TOP | DT_SINGLELINE); }
    RECT label{box.left + scale(6), twoLine ? box.bottom - scale(23) : box.top, box.right - scale(7), twoLine ? box.bottom - scale(4) : box.bottom - 1};
    SelectObject(dc, font); SetTextColor(dc, themeColor(HeaderText)); DrawTextW(dc, definition.title, -1, &label, (right ? DT_RIGHT : DT_LEFT) | (twoLine ? DT_BOTTOM : DT_VCENTER) | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (id == sortColumn[size_t(selectedTab)] && !compact) {
      Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); Gdiplus::Pen pen(Gdiplus::Color(255, 118, 118, 118), 1.0f); const float unit = float(scale(100)) / 100;
      const float left = float(twoLine && right ? box.left + scale(9) : (box.left + box.right) / 2 - scale(4)), top = float(box.top + scale(twoLine ? 6 : 1)), width = 8 * unit, height = 4 * unit;
      const bool descending = !ascending[size_t(selectedTab)];
      const Gdiplus::PointF points[] = {{left, descending ? top : top + height}, {left + width / 2, descending ? top + height : top}, {left + width, descending ? top : top + height}}; graphics.DrawLines(&pen, points, 3);
    }
  }
  RECT bottom{client.left, client.bottom - 1, client.right, client.bottom}; SetDCBrushColor(dc, themeColor(RGB(229, 229, 229))); FillRect(dc, &bottom, GetStockBrush(DC_BRUSH));
}
LRESULT CALLBACK Application::headerProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context) {
  auto app = reinterpret_cast<Application*>(context);
  HDITEMW widthItem{};
  const bool widthChange = message == HDM_SETITEMW && data && (reinterpret_cast<HDITEMW*>(data)->mask & HDI_WIDTH);
  if (widthChange && !app->compact) {
    widthItem = *reinterpret_cast<HDITEMW*>(data);
    widthItem.cxy = std::max(widthItem.cxy, app->scale(app->minimumColumnWidth(app->columnsTab, app->columnAt(int(word)))));
    if (widthItem.mask == HDI_WIDTH && widthItem.cxy == ListView_GetColumnWidth(app->list, int(word))) return TRUE;
    data = LPARAM(&widthItem);
  }
  const bool geometryChange = widthChange || message == HDM_SETORDERARRAY || (app->columnTracking && (message == WM_MOUSEMOVE || message == WM_LBUTTONUP));
  if (geometryChange && !app->rebuilding) app->geometryQueued = true;
  switch (message) {
  case WM_ERASEBKGND: return 1;
  case WM_LBUTTONDOWN: {
    HDHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(data), GET_Y_LPARAM(data)}; SendMessageW(target, HDM_HITTEST, 0, LPARAM(&hit));
    app->columnTracking = (hit.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)) != 0;
    break;
  }
  case WM_CAPTURECHANGED: app->columnTracking = false; break;
  case WM_CONTEXTMENU: {
    POINT point{GET_X_LPARAM(data), GET_Y_LPARAM(data)};
    if (point.x == -1 && point.y == -1) { RECT bounds{}; GetWindowRect(target, &bounds); point = {bounds.left, bounds.bottom}; }
    app->headerMenu(point); return 0;
  }
  case WM_LBUTTONDBLCLK: { HDHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(data), GET_Y_LPARAM(data)}; SendMessageW(target, HDM_HITTEST, 0, LPARAM(&hit)); if (hit.flags & HHT_ONHEADER) return DefSubclassProc(target, WM_LBUTTONDOWN, word, data); break; }
  case WM_PAINT: {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(target, &paint);
    if (app->geometryQueued && app->columnMutationDepth) { EndPaint(target, &paint); return 0; }
#ifdef TASKMGR_DIAGNOSTICS
    ++app->headerPaints;
#endif
    RECT client{}; GetClientRect(target, &client);
    RECT viewport{}; GetClientRect(app->list, &viewport); MapWindowPoints(app->list, target, reinterpret_cast<POINT*>(&viewport), 2);
    RECT area{}; IntersectRect(&area, &paint.rcPaint, &viewport); IntersectRect(&area, &area, &client);
    if (IsRectEmpty(&area)) { EndPaint(target, &paint); return 0; }
    HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, area.right - area.left, area.bottom - area.top); auto old = SelectObject(memory, bitmap);
    SetViewportOrgEx(memory, -area.left, -area.top, nullptr); IntersectClipRect(memory, area.left, area.top, area.right, area.bottom);
    app->paintHeader(memory, client); BitBlt(dc, area.left, area.top, area.right - area.left, area.bottom - area.top, memory, area.left, area.top, SRCCOPY);
    SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); EndPaint(target, &paint); return 0;
  }
  case HDM_LAYOUT: {
    const auto result = DefSubclassProc(target, message, word, data); const auto layout = reinterpret_cast<HDLAYOUT*>(data);
    if (layout && layout->prc && layout->pwpos && !app->compact) { const int height = app->twoLineHeader() ? app->scale(44) : app->scale(25); layout->pwpos->cy = height; layout->prc->top = layout->pwpos->y + height; }
    return result;
  }
  case WM_MOUSEMOVE: { HDHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(data), GET_Y_LPARAM(data)}; const int item = int(SendMessageW(target, HDM_HITTEST, 0, LPARAM(&hit))); if (item != app->hotHeader) { app->hotHeader = item; InvalidateRect(target, nullptr, FALSE); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0}; TrackMouseEvent(&track); } break; }
  case WM_MOUSELEAVE: app->hotHeader = -1; InvalidateRect(target, nullptr, FALSE); break;
  case WM_NCDESTROY: RemoveWindowSubclass(target, headerProcedure, id); break;
  }
  const bool batch = geometryChange && !app->columnMutationDepth && !app->rebuilding && (GetWindowLongPtrW(app->list, GWL_STYLE) & WS_VISIBLE);
  ++app->columnMutationDepth;
  if (batch) SendMessageW(app->list, WM_SETREDRAW, FALSE, 0);
  if (message == WM_MOUSEMOVE && app->columnTracking) { app->trackingColumn = -1; app->trackingWidth = -1; }
  const auto result = DefSubclassProc(target, message, word, data);
  if (message == WM_MOUSEMOVE && app->columnTracking && app->trackingColumn >= 0 && app->trackingWidth >= 0) {
    const int column = app->trackingColumn, width = app->trackingWidth;
    app->trackingColumn = -1; app->trackingWidth = -1;
    if (ListView_GetColumnWidth(app->list, column) != width) ListView_SetColumnWidth(app->list, column, width);
  }
  if (batch) SendMessageW(app->list, WM_SETREDRAW, TRUE, 0);
  --app->columnMutationDepth;
  if (geometryChange && !app->rebuilding) app->columnGeometryDirty = true;
  if (app->geometryQueued && !app->columnMutationDepth && !app->rebuilding) SendMessageW(app->window, WM_APP + 10, 0, 0);
  return result;
}
LRESULT CALLBACK Application::listProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context) {
  auto app = reinterpret_cast<Application*>(context);
  const bool deferScrollbars = app->rebuilding || (app->geometryQueued && app->columnMutationDepth);
  if (app->dark && message == WM_NCPAINT) { if (!deferScrollbars) app->paintScrollbars(); return 0; }
  if (app->dark && message == WM_NCACTIVATE) { if (!deferScrollbars) app->paintScrollbars(); return TRUE; }
  const bool horizontal = message == WM_HSCROLL || message == WM_MOUSEHWHEEL;
  const int previousScroll = horizontal ? GetScrollPos(target, SB_HORZ) : 0;
  const bool control = GetKeyState(VK_CONTROL) & 0x8000;
  switch (message) {
  case LVM_SETCOLUMNWIDTH:
    if (data >= 0 && !app->compact) {
      data = std::max(int(data), app->scale(app->minimumColumnWidth(app->columnsTab, app->columnAt(int(word)))));
      if (int(data) == ListView_GetColumnWidth(target, int(word))) return TRUE;
    }
    break;
  case WM_NOTIFY: {
    const auto notice = reinterpret_cast<NMHEADERW*>(data);
    if (notice && notice->hdr.hwndFrom == app->header && (notice->hdr.code == HDN_ITEMCHANGINGW || notice->hdr.code == HDN_TRACKW) && notice->pitem && (notice->pitem->mask & HDI_WIDTH) && !app->compact) {
      notice->pitem->cxy = std::max(notice->pitem->cxy, app->scale(app->minimumColumnWidth(app->columnsTab, app->columnAt(notice->iItem))));
      if (notice->hdr.code == HDN_TRACKW && app->columnTracking) { app->trackingColumn = notice->iItem; app->trackingWidth = notice->pitem->cxy; }
    }
    break;
  }
  case WM_CHAR: if (app->hidden() || !control) { if (word >= L' ') app->letter(wchar_t(word)); return 0; } break;
  case WM_KEYDOWN: {
    const int index = app->selectedIndex(); const Row* row = app->selectedRow();
    if (control && word == 'C') { app->copy(); return 0; }
    if (word == VK_DELETE && !app->hidden()) { if (IsWindowEnabled(app->end) && IsWindowVisible(app->end)) app->command(EndId); return 0; }
    if (row && row->expandable && ((word == VK_RIGHT && !row->expanded) || (word == VK_LEFT && row->expanded))) { app->toggle(index); return 0; }
    if (row && word == VK_LEFT && row->depth > 0) { for (int parent = index - 1; parent >= 0; --parent) if (app->rows[size_t(parent)].depth == 0) { app->select(parent); break; } return 0; }
    if (row && word == VK_RETURN) { if (row->expandable) app->toggle(index); else if (app->selectedTab == ProcessesTab && (row->kind == RowKind::Window || (row->kind == RowKind::Process && app->current && app->current->processes[size_t(row->process)].app))) app->command(SwitchTo); return 0; }
    break;
  }
  case WM_KEYUP: if (word == VK_CONTROL) app->consume(); break;
  case WM_GETDLGCODE: { const auto message_ = reinterpret_cast<const MSG*>(data); return DefSubclassProc(target, message, word, data) | (message_ && message_->message == WM_KEYDOWN && message_->wParam == VK_RETURN ? DLGC_WANTMESSAGE : 0); }
  case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
    LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(data), GET_Y_LPARAM(data)}; const int index = ListView_HitTest(target, &hit);
    if (index < 0 || size_t(index) >= app->rows.size()) break;
    const auto& row = app->rows[size_t(index)];
    if (row.kind == RowKind::Heading) { SetFocus(target); return 0; }
    RECT bounds{}; ListView_GetItemRect(target, index, &bounds, LVIR_BOUNDS);
    const int zone = hit.pt.x - bounds.left - row.depth * app->scale(20);
    if (row.expandable && (message == WM_LBUTTONDBLCLK || (zone >= 0 && zone < app->scale(26)))) { app->select(index); app->toggle(index); SetFocus(target); return 0; }
    if (message == WM_LBUTTONDBLCLK && app->selectedTab == ProcessesTab && !app->compact) return 0;
    if (message == WM_LBUTTONDBLCLK && app->compact) { app->select(index); app->command(SwitchTo); return 0; }
    break;
  }
  case WM_MOUSEMOVE: {
    LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(data), GET_Y_LPARAM(data)}; const int index = ListView_HitTest(target, &hit);
    if (index != app->hotRow) { const int previous = app->hotRow; app->hotRow = index; app->invalidateRow(previous); app->invalidateRow(index); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0}; TrackMouseEvent(&track); }
    break;
  }
  case WM_MOUSELEAVE: { const int previous = app->hotRow; app->hotRow = -1; app->invalidateRow(previous); break; }
  case WM_NCDESTROY: RemoveWindowSubclass(target, listProcedure, id); break;
  }
  const bool batch = message == LVM_SETCOLUMNWIDTH && !app->columnMutationDepth && !app->rebuilding && (GetWindowLongPtrW(target, GWL_STYLE) & WS_VISIBLE);
  ++app->columnMutationDepth;
  if (batch) DefSubclassProc(target, WM_SETREDRAW, FALSE, 0);
  const auto result = DefSubclassProc(target, message, word, data);
  if (batch) DefSubclassProc(target, WM_SETREDRAW, TRUE, 0);
  --app->columnMutationDepth;
  if (app->geometryQueued && !app->columnMutationDepth && !app->rebuilding) SendMessageW(app->window, WM_APP + 10, 0, 0);
  if (horizontal && GetScrollPos(target, SB_HORZ) != previousScroll) RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
  if (app->dark && (message == WM_PRINT || (!app->rebuilding && !(app->geometryQueued && app->columnMutationDepth))) && (message == WM_PAINT || message == WM_HSCROLL || message == WM_VSCROLL || message == WM_MOUSEWHEEL || message == WM_SIZE || message == WM_NCMOUSEMOVE || message == WM_NCMOUSELEAVE || message == WM_THEMECHANGED || message == WM_PRINT)) app->paintScrollbars(message == WM_PRINT ? HDC(word) : nullptr);
  return result;
}
}
