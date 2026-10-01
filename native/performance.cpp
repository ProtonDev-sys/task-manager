#include "app.hpp"
#include <windowsx.h>

namespace taskmgr {
static constexpr COLORREF LabelColor = RGB(112, 112, 112), SecondaryText = RGB(60, 60, 60);
static void text(HDC dc, const std::wstring& value, HFONT font, COLORREF color, RECT box, UINT flags = DT_LEFT | DT_TOP) { SelectObject(dc, font); SetTextColor(dc, color); DrawTextW(dc, value.c_str(), -1, &box, flags | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS); }
static double niceScale(double peak, std::span<const double> steps) { for (const double step : steps) if (peak * 1.1 <= step) return step; return steps.back() * std::ceil(peak * 1.1 / steps.back()); }
static constexpr double bitSteps[] = {100e3, 500e3, 1e6, 5e6, 10e6, 50e6, 100e6, 500e6, 1e9, 5e9, 10e9, 50e9, 100e9};
static constexpr double byteSteps[] = {100.0 * 1024, 500.0 * 1024, 1048576, 5.0 * 1048576, 10.0 * 1048576, 50.0 * 1048576, 100.0 * 1048576, 500.0 * 1048576, 1073741824, 5.0 * 1073741824};
static std::wstring gigabytes(double value) { return number(value / 1073741824, 1); }
static std::wstring sendReceive(const NetworkInfo& network) {
  const auto send = bits(network.send), receive = bits(network.receive); const auto unit = [](const std::wstring& value) { return value.substr(value.rfind(L' ') + 1); };
  return unit(send) == unit(receive) ? L"S: " + send.substr(0, send.rfind(L' ')) + L"  R: " + receive : L"S: " + send + L"  R: " + receive;
}
const std::vector<PerfItem>& Application::perfItems() const {
  if (!performanceItemsDirty) return performanceItems;
  auto& result = performanceItems; result.clear(); performanceItemsDirty = false;
  if (!current) return result;
  result.reserve(2 + current->disks.size() + current->networks.size() + current->gpus.size());
#ifdef TASKMGR_DIAGNOSTICS
  ++performanceItemBuilds;
#endif
  result.push_back({L"cpu", L"CPU", L"", number(current->cpu, 0) + L"%  " + number(current->cpuSpeed, 2) + L" GHz", current->cpuInfo.name, CpuColor});
  const double used = double(current->memory.total - current->memory.available);
  result.push_back({L"memory", L"Memory", L"", gigabytes(used) + L"/" + gigabytes(double(current->memory.total)) + L" GB (" + number(100 * used / std::max(1.0, double(current->memory.total)), 0) + L"%)", bytes(double(current->memory.installed ? current->memory.installed : current->memory.total)), MemoryColor});
  for (const auto& disk : current->disks) result.push_back({L"disk/" + disk.key, disk.title, disk.type, number(disk.active, 0) + L"%", disk.model, DiskColor});
  for (const auto& network : current->networks) result.push_back({L"net/" + std::to_wstring(network.luid), network.type, L"", sendReceive(network), network.description, NetworkColor});
  for (const auto& gpu : current->gpus) result.push_back({L"gpu/" + gpu.key, L"GPU " + std::to_wstring(gpu.index), gpu.name, number(gpu.usage, 0) + L"%" + (gpu.temperature >= 0 ? L" (" + number(gpu.temperature, 0) + L" °C)" : L""), gpu.name, GpuColor});
  return result;
}
void Application::recordHistories(double now) {
  histories[L"cpu"].add(now, current->cpu);
  for (size_t index = 0; index < current->cores.size(); ++index) histories[L"core/" + std::to_wstring(index)].add(now, current->cores[index]);
  histories[L"memory"].add(now, current->memory.total ? 100.0 * double(current->memory.total - current->memory.available) / double(current->memory.total) : 0);
  for (const auto& disk : current->disks) { histories[L"disk/" + disk.key].add(now, disk.active); histories[L"transfer/" + disk.key].add(now, disk.read, disk.write); }
  for (const auto& network : current->networks) histories[L"net/" + std::to_wstring(network.luid)].add(now, network.receive, network.send);
  for (const auto& gpu : current->gpus) {
    histories[L"gpu/" + gpu.key].add(now, gpu.usage);
    for (const auto& [type, value] : gpu.engines) histories[L"engine/" + gpu.key + L"/" + type].add(now, value);
    histories[L"dedicated/" + gpu.key].add(now, double(gpu.dedicated)); histories[L"shared/" + gpu.key].add(now, double(gpu.shared));
  }
  std::erase_if(histories, [&](const auto& item) { return !item.second.recent(now); });
}
void Application::paintPerformance(HDC dc, RECT bounds) {
#ifdef TASKMGR_DIAGNOSTICS
  const auto begin = Clock::now();
  ++performancePaints;
#endif
  FillRect(dc, &bounds, backgroundBrush); SetBkMode(dc, TRANSPARENT);
  const auto& items = perfItems();
  if (items.empty()) { text(dc, L"Collecting performance data…", font, themeColor(LabelColor), {scale(250), scale(30), bounds.right, scale(60)}); return; }
  if (std::none_of(items.begin(), items.end(), [&](const PerfItem& item) { return item.key == selectedResource; })) selectedResource = items.front().key;
  const auto& selected = *std::find_if(items.begin(), items.end(), [&](const PerfItem& item) { return item.key == selectedResource; });
  if (summary == 1) { paintDetail(dc, {bounds.left + scale(8), bounds.top, bounds.right - scale(8), bounds.bottom - scale(8)}, selected); return; }
  if (summary == 2) { paintSidebar(dc, bounds, items); return; }
  const int sidebar = scale(222);
  RECT sidebarBounds{0, 0, sidebar, bounds.bottom};
  if (RectVisible(dc, &sidebarBounds)) paintSidebar(dc, sidebarBounds, items);
  RECT border{sidebar, 0, sidebar + 1, bounds.bottom};
  if (RectVisible(dc, &border)) { SetDCBrushColor(dc, themeColor(RGB(238, 238, 238))); FillRect(dc, &border, GetStockBrush(DC_BRUSH)); }
  RECT detail{sidebar + scale(28), 0, bounds.right - scale(26), bounds.bottom};
  if (RectVisible(dc, &detail)) paintDetail(dc, detail, selected);
#ifdef TASKMGR_DIAGNOSTICS
  if (hidden()) paintTimes.push_back(milliseconds(begin));
#endif
}
void Application::paintSidebar(HDC dc, RECT bounds, const std::vector<PerfItem>& items) {
  const int pitch = scale(hideGraphs ? 60 : 82), maximumScroll = std::max(0, int(items.size()) * pitch + scale(24) - int(bounds.bottom - bounds.top));
  sidebarScroll = std::clamp(sidebarScroll, 0, maximumScroll);
  const int save = SaveDC(dc); IntersectClipRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
  for (size_t index = 0; index < items.size(); ++index) {
    const auto& item = items[index]; const int top = bounds.top + scale(12) + int(index) * pitch - sidebarScroll;
    if (top + pitch < bounds.top || top > bounds.bottom) continue;
    RECT box{bounds.left + scale(2), top, bounds.right - scale(4), top + pitch - scale(4)};
    if (!RectVisible(dc, &box)) continue;
    const bool hot = int(index) == hotResource;
    if (item.key == selectedResource || hot) { SetDCBrushColor(dc, themeColor(item.key == selectedResource ? RGB(205, 232, 255) : RGB(229, 243, 255))); FillRect(dc, &box, GetStockBrush(DC_BRUSH)); }
    int textLeft = bounds.left + scale(16);
    if (!hideGraphs) {
      double maximum = 100; auto& history = histories[item.key];
      if (item.key.starts_with(L"net/")) maximum = niceScale(history.peak(graphTime) * 8, bitSteps) / 8;
      drawGraph(dc, {bounds.left + scale(15), top + scale(12), bounds.left + scale(105), top + scale(64)}, history, graphTime, maximum, {themeColor(item.color), false, false, item.key.starts_with(L"net/")});
      textLeft = bounds.left + scale(116);
    }
    text(dc, item.title, sidebarFont, themeColor(RGB(0, 0, 0)), {textLeft, top + scale(hideGraphs ? 6 : 10), box.right, top + scale(hideGraphs ? 28 : 33)});
    if (!item.subtitle.empty()) { text(dc, item.subtitle, labelFont, themeColor(SecondaryText), {textLeft, top + scale(hideGraphs ? 27 : 33), box.right, top + scale(hideGraphs ? 43 : 49)}); text(dc, item.value, labelFont, themeColor(SecondaryText), {textLeft, top + scale(hideGraphs ? 41 : 49), box.right, top + scale(hideGraphs ? 57 : 65)}); }
    else { RECT value{textLeft, top + scale(hideGraphs ? 27 : 35), box.right - scale(2), top + scale(hideGraphs ? 57 : 67)}; SelectObject(dc, labelFont); SetTextColor(dc, themeColor(SecondaryText)); DrawTextW(dc, item.value.c_str(), -1, &value, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS); }
  }
  RestoreDC(dc, save);
}
void Application::paintDetail(HDC dc, RECT bounds, const PerfItem& item) {
#ifdef TASKMGR_DIAGNOSTICS
  ++performanceDetailPaints;
#endif
  const int left = bounds.left, width = std::max<int>(scale(100), bounds.right - bounds.left); const int right = left + width;
  const auto key = item.key; const bool cpu = key == L"cpu", memory = key == L"memory", disk = key.starts_with(L"disk/"), network = key.starts_with(L"net/"), gpu = key.starts_with(L"gpu/");
  const auto stat = [&](const std::wstring& label, const std::wstring& value, int x, int y, bool large = true) { text(dc, label, labelFont, themeColor(LabelColor), {x, y, x + scale(200), y + scale(18)}); text(dc, value, large ? valueFont : sidebarFont, themeColor(RGB(0, 0, 0)), {x, y + scale(17), x + scale(220), y + scale(large ? 51 : 43)}); };
  const auto pair = [&](const std::wstring& label, const std::wstring& value, int x, int y, int span) { text(dc, label, labelFont, themeColor(LabelColor), {x, y, x + span, y + scale(20)}); text(dc, value, font, themeColor(RGB(0, 0, 0)), {x + scale(120), y, x + span, y + scale(20)}); };
  const bool compactSummary = summary == 1;
  int top = compactSummary ? scale(8) : scale(14);
  if (!compactSummary) {
    text(dc, item.title, titleFont, themeColor(RGB(0, 0, 0)), {left, top, right, top + scale(42)});
    text(dc, item.description, subtitleFont, themeColor(RGB(0, 0, 0)), {left + scale(140), top + scale(15), right, top + scale(40)}, DT_RIGHT | DT_TOP);
    top = scale(73);
  }
  const int statsHeight = compactSummary ? scale(10) : cpu ? scale(250) : memory ? scale(290) : disk ? scale(330) : network ? scale(160) : scale(180);
  int graphBottom = std::max(top + scale(80), int(bounds.bottom) - statsHeight);
  if (gpu) graphBottom = std::max(top + scale(110), graphBottom - scale(150));
  if (!compactSummary) graphBottom = std::min(graphBottom, top + scale(21) + (gpu ? scale(260) : scale(360)));
  const RECT graph{left, top + scale(21), right, graphBottom};
  const auto& history = histories[key];
  double maximum = 100; std::wstring leftLabel = cpu ? L"% Utilization" : memory ? L"Memory usage" : disk ? L"Active time" : network ? L"Throughput" : L"", rightLabel = L"100%";
  if (memory && current) rightLabel = gigabytes(double(current->memory.total)) + L" GB";
  if (network) { maximum = niceScale(history.peak(graphTime) * 8, bitSteps) / 8; rightLabel = bits(maximum); }
  if (!gpu) { text(dc, leftLabel, labelFont, themeColor(LabelColor), {left, top, right, top + scale(18)}); text(dc, rightLabel, labelFont, themeColor(LabelColor), {left, top, right, top + scale(18)}, DT_RIGHT | DT_TOP); }
  auto panels = [&](const std::vector<std::tuple<std::wstring, std::wstring, std::wstring>>& items, RECT area, COLORREF color) {
    if (items.empty()) return;
    const int count = int(items.size()), columns = count <= 4 ? std::min(count, 2) : int(std::ceil(std::sqrt(double(count) * (area.right - area.left) / std::max(1L, area.bottom - area.top)))), lines = (count + columns - 1) / columns;
    const int gap = scale(8), labelHeight = gpu ? scale(18) : 0, panelWidth = (area.right - area.left - (columns - 1) * gap) / columns, panelHeight = (area.bottom - area.top - (lines - 1) * gap) / lines - labelHeight;
    for (int index = 0; index < count; ++index) {
      const int x = area.left + index % columns * (panelWidth + gap), y = area.top + index / columns * (panelHeight + gap + labelHeight);
      const auto& [historyKey, title, value] = items[size_t(index)];
      if (gpu) { text(dc, title, labelFont, themeColor(LabelColor), {x, y, x + panelWidth, y + scale(18)}); text(dc, value, labelFont, themeColor(LabelColor), {x, y, x + panelWidth, y + scale(18)}, DT_RIGHT | DT_TOP); }
      drawGraph(dc, {x, y + labelHeight, x + panelWidth, y + labelHeight + panelHeight}, histories[historyKey], graphTime, 100, {color, true, true, false});
    }
  };
  int bottom = graph.bottom;
  if (cpu && logical && current && !current->cores.empty()) { std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> items; for (size_t index = 0; index < current->cores.size(); ++index) items.emplace_back(L"core/" + std::to_wstring(index), L"", L""); panels(items, graph, themeColor(item.color)); }
  else if (gpu && current) {
    const auto found = std::find_if(current->gpus.begin(), current->gpus.end(), [&](const GpuInfo& value) { return L"gpu/" + value.key == key; });
    if (found != current->gpus.end()) {
      static const std::array<std::wstring, 4> preferred{L"3D", L"Copy", L"VideoEncode", L"VideoDecode"};
      std::vector<std::pair<std::wstring, double>> engines;
      for (const auto& name : preferred) for (const auto& engine : found->engines) if (engine.first == name) engines.push_back(engine);
      for (const auto& engine : found->engines) if (engines.size() < 4 && std::none_of(engines.begin(), engines.end(), [&](const auto& value) { return value.first == engine.first; })) engines.push_back(engine);
      std::vector<std::tuple<std::wstring, std::wstring, std::wstring>> items;
      for (const auto& [type, value] : engines) { auto title = type; if (title == L"VideoEncode") title = L"Video Encode"; else if (title == L"VideoDecode") title = L"Video Decode"; else if (title == L"VideoProcessing") title = L"Video Processing"; items.emplace_back(L"engine/" + found->key + L"/" + type, title, number(value, 0) + L"%"); }
      panels(items, {graph.left, graph.top - scale(21), graph.right, graph.bottom}, themeColor(item.color));
      int y = graph.bottom + scale(10);
      for (const auto& [series, label, used, limit] : {std::tuple{L"dedicated/", L"Dedicated GPU memory usage", found->dedicated, found->dedicatedLimit}, std::tuple{L"shared/", L"Shared GPU memory usage", found->shared, found->sharedLimit}}) {
        if (compactSummary) break;
        text(dc, label, labelFont, themeColor(LabelColor), {left, y, right, y + scale(18)}); text(dc, gigabytes(double(limit)) + L" GB", labelFont, themeColor(LabelColor), {left, y, right, y + scale(18)}, DT_RIGHT | DT_TOP);
        drawGraph(dc, {left, y + scale(19), right, y + scale(63)}, histories[series + found->key], graphTime, std::max(1.0, double(limit)), {themeColor(item.color), false, false, false}); (void)used;
        bottom = y + scale(63); y = bottom + scale(8);
      }
    }
  } else drawGraph(dc, graph, history, graphTime, maximum, {themeColor(item.color), true, true, network});
  if (compactSummary || !current) return;
  if (!gpu) { text(dc, L"60 seconds", labelFont, themeColor(LabelColor), {left, bottom + scale(4), right, bottom + scale(22)}); text(dc, L"0", labelFont, themeColor(LabelColor), {left, bottom + scale(4), right, bottom + scale(22)}, DT_RIGHT | DT_TOP); }
  int stats = bottom + scale(34);
  const int pairs = right - scale(260), pairWidth = scale(260);
  if (cpu) {
    stat(L"Utilization", number(current->cpu, 0) + L"%", left, stats); stat(L"Speed", number(current->cpuSpeed, 2) + L" GHz", left + scale(130), stats);
    stat(L"Processes", grouped(double(current->processes.size())), left, stats + scale(57), false); stat(L"Threads", grouped(current->threads), left + scale(110), stats + scale(57), false); stat(L"Handles", grouped(current->handles), left + scale(210), stats + scale(57), false);
    stat(L"Up time", duration(current->uptime), left, stats + scale(109));
    if (width >= scale(560)) {
      const auto& info = current->cpuInfo; int y = stats + scale(4);
      pair(L"Base speed:", number(info.baseMhz / 1000, 2) + L" GHz", pairs, y, pairWidth); y += scale(21);
      pair(L"Sockets:", std::to_wstring(info.sockets), pairs, y, pairWidth); y += scale(21); pair(L"Cores:", std::to_wstring(info.cores), pairs, y, pairWidth); y += scale(21);
      pair(L"Logical processors:", std::to_wstring(info.logical), pairs, y, pairWidth); y += scale(21); pair(L"Virtualization:", info.virtualization ? L"Enabled" : L"Disabled", pairs, y, pairWidth); y += scale(21);
      for (size_t index = 0; index < 3; ++index) { pair(L"L" + std::to_wstring(index + 1) + L" cache:", info.caches[index] ? bytes(double(info.caches[index])) : L"—", pairs, y, pairWidth); y += scale(21); }
    }
  } else if (memory) {
    const auto& value = current->memory; const double total = std::max(1.0, double(value.total)), used = double(value.total - value.available), cached = std::min(double(value.cached), double(value.available)), free = double(value.available) - cached;
    text(dc, L"Memory composition", labelFont, themeColor(LabelColor), {left, stats - scale(6), right, stats + scale(12)});
    const int barTop = stats + scale(14), barBottom = barTop + scale(31); int x = left;
    for (const auto& [amount, fill] : {std::pair{used, themeColor(RGB(230, 212, 239))}, std::pair{cached, themeColor(RGB(244, 236, 248))}, std::pair{free, themeColor(RGB(255, 255, 255))}}) {
      const int next = x + int((right - left) * amount / total); RECT segment{x, barTop, next, barBottom}; HBRUSH brush = CreateSolidBrush(fill); FillRect(dc, &segment, brush); DeleteObject(brush);
      if (next > x && next < right - 1) { RECT divider{next, barTop, next + 1, barBottom}; HBRUSH line = CreateSolidBrush(MemoryColor); FillRect(dc, &divider, line); DeleteObject(line); }
      x = next;
    }
    HBRUSH frame = CreateSolidBrush(MemoryColor); RECT outline{left, barTop, right, barBottom}; FrameRect(dc, &outline, frame); DeleteObject(frame);
    stats = barBottom + scale(14);
    stat(L"In use (Compressed)", bytes(used) + L" (" + number(double(value.compressed) / 1048576, 0) + L" MB)", left, stats); stat(L"Available", bytes(double(value.available)), left + scale(230), stats);
    stat(L"Committed", gigabytes(double(value.committed)) + L"/" + gigabytes(double(value.commitLimit)) + L" GB", left, stats + scale(57), false); stat(L"Cached", bytes(double(value.cached)), left + scale(230), stats + scale(57), false);
    stat(L"Paged pool", bytes(double(value.paged)), left, stats + scale(104), false); stat(L"Non-paged pool", bytes(double(value.nonpaged)), left + scale(230), stats + scale(104), false);
    if (width >= scale(640)) {
      int y = stats + scale(4);
      pair(L"Speed:", value.speed ? std::to_wstring(value.speed) + L" MHz" : L"—", pairs, y, pairWidth); y += scale(21);
      pair(L"Slots used:", value.slots ? std::to_wstring(value.usedSlots) + L" of " + std::to_wstring(value.slots) : L"—", pairs, y, pairWidth); y += scale(21);
      pair(L"Form factor:", value.formFactor.empty() ? L"—" : value.formFactor, pairs, y, pairWidth); y += scale(21);
      pair(L"Hardware reserved:", value.installed > value.total ? bytes(double(value.installed - value.total)) : L"—", pairs, y, pairWidth);
    }
  } else if (disk) {
    const auto found = std::find_if(current->disks.begin(), current->disks.end(), [&](const DiskInfo& value) { return L"disk/" + value.key == key; }); if (found == current->disks.end()) return;
    auto& transfer = histories[L"transfer/" + found->key]; const double transferMaximum = niceScale(transfer.peak(graphTime), byteSteps);
    text(dc, L"Disk transfer rate", labelFont, themeColor(LabelColor), {left, stats - scale(6), right, stats + scale(12)}); text(dc, bytes(transferMaximum) + L"/s", labelFont, themeColor(LabelColor), {left, stats - scale(6), right, stats + scale(12)}, DT_RIGHT | DT_TOP);
    const RECT transferGraph{left, stats + scale(14), right, stats + scale(94)}; drawGraph(dc, transferGraph, transfer, graphTime, transferMaximum, {themeColor(item.color), true, true, true});
    text(dc, L"60 seconds", labelFont, themeColor(LabelColor), {left, transferGraph.bottom + scale(4), right, transferGraph.bottom + scale(22)}); text(dc, L"0", labelFont, themeColor(LabelColor), {left, transferGraph.bottom + scale(4), right, transferGraph.bottom + scale(22)}, DT_RIGHT | DT_TOP);
    stats = transferGraph.bottom + scale(34);
    stat(L"Active time", number(found->active, 0) + L"%", left, stats); stat(L"Average response time", number(found->response, 1) + L" ms", left + scale(170), stats);
    stat(L"Read speed", bytes(found->read) + L"/s", left, stats + scale(57), false); stat(L"Write speed", bytes(found->write) + L"/s", left + scale(170), stats + scale(57), false);
    if (width >= scale(560)) {
      int y = stats + scale(4);
      pair(L"Capacity:", found->capacity ? bytes(double(found->capacity)) : L"—", pairs, y, pairWidth); y += scale(21); pair(L"Formatted:", found->formatted ? bytes(double(found->formatted)) : L"—", pairs, y, pairWidth); y += scale(21);
      pair(L"System disk:", found->system ? L"Yes" : L"No", pairs, y, pairWidth); y += scale(21); pair(L"Page file:", found->pagefile ? L"Yes" : L"No", pairs, y, pairWidth); y += scale(21);
      pair(L"Type:", found->type.empty() ? L"—" : found->type, pairs, y, pairWidth);
    }
  } else if (network) {
    const auto found = std::find_if(current->networks.begin(), current->networks.end(), [&](const NetworkInfo& value) { return L"net/" + std::to_wstring(value.luid) == key; }); if (found == current->networks.end()) return;
    stat(L"Send", bits(found->send), left + scale(14), stats); stat(L"Receive", bits(found->receive), left + scale(194), stats);
    HPEN dashed = CreatePen(PS_DASH, 1, themeColor(item.color)), solid = CreatePen(PS_SOLID, 1, themeColor(item.color)); auto old = SelectObject(dc, dashed);
    MoveToEx(dc, left, stats + scale(8), nullptr); LineTo(dc, left + scale(9), stats + scale(8)); SelectObject(dc, solid); MoveToEx(dc, left + scale(180), stats + scale(8), nullptr); LineTo(dc, left + scale(189), stats + scale(8)); SelectObject(dc, old); DeleteObject(dashed); DeleteObject(solid);
    if (width >= scale(560)) {
      int y = stats + scale(4); const int span = scale(320), x = right - span;
      pair(L"Adapter name:", found->alias, x, y, span); y += scale(21); pair(L"Connection type:", found->type, x, y, span); y += scale(21);
      pair(L"IPv4 address:", found->ipv4.empty() ? L"—" : found->ipv4, x, y, span); y += scale(21); pair(L"IPv6 address:", found->ipv6.empty() ? L"—" : found->ipv6, x, y, span);
    }
  } else if (gpu) {
    const auto found = std::find_if(current->gpus.begin(), current->gpus.end(), [&](const GpuInfo& value) { return L"gpu/" + value.key == key; }); if (found == current->gpus.end()) return;
    stat(L"Utilization", number(found->usage, 0) + L"%", left, stats); stat(L"Dedicated GPU memory", gigabytes(double(found->dedicated)) + L"/" + gigabytes(double(found->dedicatedLimit)) + L" GB", left + scale(160), stats);
    stat(L"GPU Memory", gigabytes(double(found->dedicated + found->shared)) + L"/" + gigabytes(double(found->dedicatedLimit + found->sharedLimit)) + L" GB", left, stats + scale(57), false); stat(L"Shared GPU memory", gigabytes(double(found->shared)) + L"/" + gigabytes(double(found->sharedLimit)) + L" GB", left + scale(160), stats + scale(57), false);
    if (found->temperature >= 0) stat(L"GPU Temperature", number(found->temperature, 0) + L" °C", left, stats + scale(104), false);
    if (width >= scale(600)) {
      int y = stats + scale(4);
      pair(L"Driver version:", found->driverVersion.empty() ? L"—" : found->driverVersion, pairs, y, pairWidth); y += scale(21); pair(L"Driver date:", found->driverDate.empty() ? L"—" : found->driverDate, pairs, y, pairWidth); y += scale(21);
      pair(L"Physical location:", found->location.empty() ? L"—" : found->location, pairs, y, pairWidth);
    }
  }
}
std::wstring Application::performanceSummary() const {
  if (!current) return L"";
  const auto& items = perfItems(); std::wstring result;
  for (const auto& item : items) if (item.key == selectedResource) { result = item.title + L"\r\n\r\n\t" + item.description + L"\r\n\r\n\t" + item.value + L"\r\n"; break; }
  if (selectedResource == L"cpu") result += L"\tProcesses:\t" + std::to_wstring(current->processes.size()) + L"\r\n\tThreads:\t" + std::to_wstring(current->threads) + L"\r\n\tHandles:\t" + std::to_wstring(current->handles) + L"\r\n\tUp time:\t" + duration(current->uptime) + L"\r\n";
  return result;
}
void Application::performanceClick(POINT point, bool right, bool twice) {
  if (!hidden()) SetFocus(performance);
  const auto& items = perfItems(); const int sidebar = summary == 2 ? INT_MAX : summary == 1 ? -1 : scale(222);
  if (point.x < sidebar) {
    const int index = performanceHitTest(point);
    if (index >= 0 && selectedResource != items[size_t(index)].key) { selectedResource = items[size_t(index)].key; InvalidateRect(performance, nullptr, FALSE); }
    if (twice) setSummary(summary ? 0 : 2);
    if (right) { POINT screen = point; ClientToScreen(performance, &screen); performanceMenu(screen, true); }
    return;
  }
  if (twice) setSummary(summary ? 0 : 1);
  if (right) { POINT screen = point; ClientToScreen(performance, &screen); performanceMenu(screen, false); }
}
void Application::performanceKey(WPARAM key) {
  if ((GetKeyState(VK_CONTROL) & 0x8000) && key == 'C') { copy(); return; }
  const auto& items = perfItems(); if (items.empty()) return;
  int index = int(std::find_if(items.begin(), items.end(), [&](const PerfItem& item) { return item.key == selectedResource; }) - items.begin());
  if (key == VK_UP) --index; else if (key == VK_DOWN) ++index; else if (key == VK_HOME) index = 0; else if (key == VK_END) index = int(items.size()) - 1; else return;
  index = std::clamp(index, 0, int(items.size()) - 1); selectedResource = items[size_t(index)].key;
  RECT client{}; GetClientRect(performance, &client); const int pitch = scale(hideGraphs ? 60 : 82);
  if (index * pitch < sidebarScroll) sidebarScroll = index * pitch; if ((index + 1) * pitch + scale(24) > sidebarScroll + client.bottom) sidebarScroll = (index + 1) * pitch + scale(24) - client.bottom;
  InvalidateRect(performance, nullptr, FALSE);
}
RECT Application::performanceRowRect(int index) const {
  RECT client{}; GetClientRect(performance, &client);
  const int pitch = scale(hideGraphs ? 60 : 82), top = scale(12) + index * pitch - sidebarScroll;
  return {scale(2), top, (summary == 2 ? client.right : scale(222)) - scale(4), top + pitch - scale(4)};
}
int Application::performanceHitTest(POINT point) const {
  if (summary == 1 || point.y < 0) return -1;
  RECT client{}; GetClientRect(performance, &client);
  if (!PtInRect(&client, point)) return -1;
  if (point.x < scale(2) || point.x >= (summary == 2 ? client.right : scale(222)) - scale(4)) return -1;
  const int offset = point.y - scale(12) + sidebarScroll;
  if (offset < 0) return -1;
  const int index = offset / scale(hideGraphs ? 60 : 82);
  if (size_t(index) >= perfItems().size()) return -1;
  const RECT box = performanceRowRect(index);
  return PtInRect(&box, point) ? index : -1;
}
void Application::performanceHover(int index) {
  if (index == hotResource) return;
  const int previous = hotResource; hotResource = index;
  for (const int row : {previous, index}) if (row >= 0) { const RECT box = performanceRowRect(row); InvalidateRect(performance, &box, FALSE); }
  UpdateWindow(performance);
}
LRESULT CALLBACK Application::performanceProcedure(HWND target, UINT message, WPARAM word, LPARAM data) {
  auto app = reinterpret_cast<Application*>(GetWindowLongPtrW(target, GWLP_USERDATA));
  if (message == WM_NCCREATE) { app = static_cast<Application*>(reinterpret_cast<CREATESTRUCTW*>(data)->lpCreateParams); SetWindowLongPtrW(target, GWLP_USERDATA, LONG_PTR(app)); }
  if (!app) return DefWindowProcW(target, message, word, data);
  switch (message) {
  case WM_ERASEBKGND: return 1;
  case WM_PAINT: {
    PAINTSTRUCT paint{}; HDC dc = BeginPaint(target, &paint); RECT bounds{}; GetClientRect(target, &bounds);
    const RECT dirty = paint.rcPaint; const int width = dirty.right - dirty.left, height = dirty.bottom - dirty.top;
    if (width > 0 && height > 0) {
      HDC memory = CreateCompatibleDC(dc); HBITMAP bitmap = memory ? CreateCompatibleBitmap(dc, width, height) : nullptr;
      if (bitmap) {
        auto old = SelectObject(memory, bitmap); SetViewportOrgEx(memory, -dirty.left, -dirty.top, nullptr); IntersectClipRect(memory, dirty.left, dirty.top, dirty.right, dirty.bottom);
        app->paintPerformance(memory, bounds); BitBlt(dc, dirty.left, dirty.top, width, height, memory, dirty.left, dirty.top, SRCCOPY);
        SelectObject(memory, old); DeleteObject(bitmap);
      } else app->paintPerformance(dc, bounds);
      if (memory) DeleteDC(memory);
    }
    EndPaint(target, &paint); return 0;
  }
  case WM_GETDLGCODE: return DLGC_WANTARROWS;
  case WM_LBUTTONDOWN: app->performanceClick({GET_X_LPARAM(data), GET_Y_LPARAM(data)}, false, false); return 0;
  case WM_LBUTTONDBLCLK: app->performanceClick({GET_X_LPARAM(data), GET_Y_LPARAM(data)}, false, true); return 0;
  case WM_RBUTTONUP: app->performanceClick({GET_X_LPARAM(data), GET_Y_LPARAM(data)}, true, false); return 0;
  case WM_CONTEXTMENU: if (GET_X_LPARAM(data) == -1) { RECT bounds{}; GetWindowRect(target, &bounds); app->performanceMenu({bounds.left + app->scale(260), bounds.top + app->scale(100)}, false); return 0; } break;
  case WM_MOUSEWHEEL: {
    if (app->summary == 1) return 0;
    RECT sidebar{}; GetClientRect(target, &sidebar); if (app->summary != 2) sidebar.right = app->scale(222);
    const int pitch = app->scale(app->hideGraphs ? 60 : 82), maximum = std::max(0, int(app->perfItems().size()) * pitch + app->scale(24) - int(sidebar.bottom));
    const int wheelDelta = app->sidebarWheelDelta + GET_WHEEL_DELTA_WPARAM(word);
    app->sidebarWheelDelta = wheelDelta % WHEEL_DELTA;
    const int scroll = std::clamp(app->sidebarScroll - wheelDelta / WHEEL_DELTA * pitch, 0, maximum);
    if (scroll != app->sidebarScroll) { app->sidebarScroll = scroll; app->hotResource = -1; InvalidateRect(target, &sidebar, FALSE); UpdateWindow(target); }
    return 0;
  }
  case WM_MOUSEMOVE: {
    const int index = GetCapture() ? -1 : app->performanceHitTest({GET_X_LPARAM(data), GET_Y_LPARAM(data)});
    if (index >= 0 && !app->performanceTracking) { TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0}; app->performanceTracking = TrackMouseEvent(&track) != FALSE; }
    app->performanceHover(index); return 0;
  }
  case WM_MOUSELEAVE: app->performanceTracking = false; app->performanceHover(-1); return 0;
  case WM_KEYDOWN: app->performanceKey(word); return 0;
  case WM_SETFOCUS: case WM_KILLFOCUS: return 0;
  }
  return DefWindowProcW(target, message, word, data);
}
}
