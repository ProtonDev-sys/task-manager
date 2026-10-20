#include "app.hpp"
#include <oleacc.h>
#include <windowsx.h>

namespace taskmgr {
namespace {
constexpr const wchar_t* navigationNames[] = {L"Processes", L"Performance", L"App history", L"Startup apps", L"Users", L"Details", L"Services"};

void fill(HDC dc, const RECT& bounds, COLORREF color) {
  SetDCBrushColor(dc, color); FillRect(dc, &bounds, GetStockBrush(DC_BRUSH));
}
void rounded(HDC dc, RECT bounds, COLORREF background, COLORREF border, int radius) {
  const auto oldBrush = SelectObject(dc, GetStockBrush(DC_BRUSH));
  const auto oldPen = SelectObject(dc, GetStockObject(DC_PEN));
  SetDCBrushColor(dc, background); SetDCPenColor(dc, border);
  RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom, radius, radius);
  SelectObject(dc, oldPen); SelectObject(dc, oldBrush);
}
// Small vector glyphs scale with the controls and need no font or image dependency.
void glyph(HDC dc, RECT bounds, int kind, COLORREF color) {
  Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  const float unit = float(bounds.right - bounds.left) / 20.0f;
  graphics.TranslateTransform(float(bounds.left), float(bounds.top)); graphics.ScaleTransform(unit, unit);
  Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), 1.4f);
  pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
  auto line = [&](float x1, float y1, float x2, float y2) { graphics.DrawLine(&pen, x1, y1, x2, y2); };
  if (kind == NavigationToggleId) { for (float y : {5.0f, 10.0f, 15.0f}) line(3, y, 17, y); }
  else if (kind == MenuId) {
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
    for (float x : {4.0f, 10.0f, 16.0f}) graphics.FillEllipse(&brush, x - 1.1f, 8.9f, 2.2f, 2.2f);
  } else if (kind == RunId) { graphics.DrawRectangle(&pen, 3, 3, 14, 14); line(10, 6, 10, 14); line(6, 10, 14, 10); }
  else if (kind == EndId) graphics.DrawRectangle(&pen, 5, 5, 10, 10);
  else if (kind == ExportId) { line(10, 3, 10, 12); line(6, 8, 10, 12); line(10, 12, 14, 8); line(3, 12, 3, 17); line(3, 17, 17, 17); line(17, 17, 17, 12); }
  else if (kind == SearchId) { graphics.DrawEllipse(&pen, 3, 3, 10, 10); line(12, 12, 17, 17); }
  else if (kind == NavigationId + PerformanceTab) {
    line(3, 3, 3, 17); line(3, 17, 17, 17);
    const Gdiplus::PointF points[] = {{5,12},{8,8},{11,11},{16,4}}; graphics.DrawLines(&pen, points, 4);
  } else if (kind == NavigationId + HistoryTab) { graphics.DrawEllipse(&pen, 3, 3, 14, 14); line(10, 6, 10, 10); line(10, 10, 13, 12); }
  else if (kind == NavigationId + StartupTab) { line(4, 16, 16, 4); line(8, 4, 16, 4); line(16, 4, 16, 12); line(4, 10, 4, 16); line(4, 16, 10, 16); }
  else if (kind == NavigationId + UsersTab) {
    graphics.DrawEllipse(&pen, 3, 3, 6, 6); graphics.DrawEllipse(&pen, 12, 4, 5, 5);
    graphics.DrawArc(&pen, 1, 11, 10, 10, 185, 170); graphics.DrawArc(&pen, 10, 11, 9, 10, 195, 155);
  } else if (kind == SettingsId || kind == NavigationId + ServicesTab) {
    graphics.DrawEllipse(&pen, 5, 5, 10, 10); graphics.DrawEllipse(&pen, 8, 8, 4, 4);
    for (const auto& pair : {std::array<float,4>{10,2,10,5}, {10,15,10,18}, {2,10,5,10}, {15,10,18,10}, {4.3f,4.3f,6.4f,6.4f}, {13.6f,13.6f,15.7f,15.7f}, {4.3f,15.7f,6.4f,13.6f}, {13.6f,6.4f,15.7f,4.3f}}) line(pair[0],pair[1],pair[2],pair[3]);
  } else {
    for (float y : {5.0f, 10.0f, 15.0f}) {
      if (kind == NavigationId + DetailsTab) { graphics.DrawRectangle(&pen, 3.0f, y-1.0f, 2.0f, 2.0f); line(8,y,17,y); }
      else { line(3,y,17,y); line(6,y-1.5f,6,y+1.5f); }
    }
  }
}
}

void Application::createShell() {
  const auto instance = GetModuleHandleW(nullptr);
  auto button = [&](const wchar_t* name, int id, DWORD style) {
    const auto result = CreateWindowExW(0, L"BUTTON", name, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
      0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(id)), instance, nullptr);
    SendMessageW(result, WM_SETFONT, WPARAM(font), FALSE);
    SetWindowSubclass(result, shellProcedure, 6, reinterpret_cast<DWORD_PTR>(this));
    return result;
  };
  navigationToggle = button(L"Collapse navigation", NavigationToggleId, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP);
  for (int index = 0; index < TabCount; ++index) {
    // The native radio control supplies checked state, accessible name and focus.
    // Painting is customized separately; these are not anonymous painted hit regions.
    navigation[size_t(index)] = button(navigationNames[index], NavigationId + index, BS_RADIOBUTTON | BS_PUSHLIKE | (index == 0 ? WS_GROUP : 0));
  }
  settingsButton = button(L"Settings and options", SettingsId, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP);
  runButton = button(L"Run new task", RunId, BS_OWNERDRAW | WS_TABSTOP | WS_GROUP);
  exportButton = button(L"Export table", ExportId, BS_OWNERDRAW | WS_TABSTOP);
  menuButton = button(L"More actions", MenuId, BS_OWNERDRAW | WS_TABSTOP);
  shellTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
    CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, instance, nullptr);
  if (shellTooltip) {
    SendMessageW(shellTooltip, TTM_SETMAXTIPWIDTH, 0, scale(320));
    auto tooltip = [&](HWND control, const wchar_t* label) {
      TOOLINFOW item{sizeof(item)}; item.uFlags = TTF_IDISHWND | TTF_SUBCLASS; item.hwnd = window; item.uId = UINT_PTR(control); item.lpszText = const_cast<wchar_t*>(label);
      SendMessageW(shellTooltip, TTM_ADDTOOLW, 0, LPARAM(&item));
    };
    for (int index = 0; index < TabCount; ++index) tooltip(navigation[size_t(index)], navigationNames[index]);
    tooltip(runButton, L"Run new task (Ctrl+Shift+N)"); tooltip(exportButton, L"Export table (Ctrl+Shift+S)");
    tooltip(menuButton, L"More actions (F10)"); tooltip(settingsButton, L"Settings and options");
  }
  IAccPropServices* properties = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&properties)))) {
    properties->SetHwndPropStr(searchBox, DWORD(OBJID_CLIENT), CHILDID_SELF, PROPID_ACC_NAME, L"Search tasks by name, publisher or PID");
    properties->Release();
  }
  syncNavigation();
}

void Application::syncNavigation() {
  for (int index = 0; index < TabCount; ++index) {
    const HWND item = navigation[size_t(index)]; if (!item) continue;
    const bool selected = index == selectedTab;
    if (SendMessageW(item, BM_GETCHECK, 0, 0) != (selected ? BST_CHECKED : BST_UNCHECKED)) {
      SendMessageW(item, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0); InvalidateRect(item, nullptr, FALSE);
    }
    const auto style = GetWindowLongPtrW(item, GWL_STYLE);
    const auto next = selected ? style | WS_TABSTOP : style & ~LONG_PTR(WS_TABSTOP);
    if (next != style) SetWindowLongPtrW(item, GWL_STYLE, next);
  }
}

void Application::layout() {
  auto show = [](HWND control, bool visible) {
    if (control && (((GetWindowLongPtrW(control, GWL_STYLE) & WS_VISIBLE) != 0) != visible)) ShowWindow(control, visible ? SW_SHOWNA : SW_HIDE);
  };
  auto place = [&](HWND control, int left, int top, int width, int height) {
    width = std::max(1, width); height = std::max(1, height);
    RECT bounds{}; GetWindowRect(control, &bounds); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    if (bounds.left != left || bounds.top != top || bounds.right - bounds.left != width || bounds.bottom - bounds.top != height) {
      MoveWindow(control, left, top, width, height, FALSE); InvalidateRect(control, nullptr, FALSE);
    }
  };
  RECT area{}; GetClientRect(window, &area); const int width = area.right, height = area.bottom;
  const HWND previousFocus = GetFocus();
  const bool full = !compact && !summary;
  show(tabs, false);
  for (const auto control : navigation) show(control, full);
  show(navigationToggle, full); show(settingsButton, full && height >= scale(290));
  show(menuButton, full);
  if (summary) {
    for (const auto control : {list, end, fewer, link, historyLink, serviceStatus, searchBox, runButton, exportButton}) show(control, false);
    show(performance, true); place(performance, 0, 0, width, height);
    if (previousFocus && IsChild(window, previousFocus) && !IsWindowVisible(previousFocus)) SetFocus(performance);
    InvalidateRect(performance, nullptr, FALSE); return;
  }
  updateFooter();
  const bool perf = selectedTab == PerformanceTab && !compact;
  show(list, !perf); show(performance, perf);
  show(searchBox, full && !perf && searchVisible);
  show(historyLink, full && selectedTab == HistoryTab);
  show(fewer, compact || selectedTab == ProcessesTab);
  if (compact) {
    navigationWidth = 0; navigationLabels = false; searchBounds = {};
    show(runButton, false); show(exportButton, false); show(historyLink, false); show(serviceStatus, false);
    place(list, 0, 0, width, height - scale(48));
    ListView_SetColumnWidth(list, 0, std::max(scale(100), width - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - scale(4)));
    place(fewer, scale(8), height - scale(38), std::min(scale(152), width - scale(118)), scale(28));
    place(end, width - scale(104), height - scale(39), scale(94), scale(30));
    contentBounds = {0, 0, width, std::max(1, height-scale(48))};
  } else {
    const bool canExpand = width >= scale(960) && (!perf || width >= scale(1200));
    navigationLabels = navigationExpanded && canExpand;
    navigationWidth = scale(navigationLabels ? 208 : 64);
    EnableWindow(navigationToggle, canExpand);
    const auto toggleName = !canExpand ? L"Navigation collapsed to fit window" : navigationLabels ? L"Collapse navigation" : L"Expand navigation";
    wchar_t existing[64]{}; GetWindowTextW(navigationToggle, existing, int(std::size(existing)));
    if (wcscmp(existing, toggleName)) SetWindowTextW(navigationToggle, toggleName);
    place(navigationToggle, scale(8), scale(14), scale(40), scale(32));
    const int navHeight = std::clamp((height - scale(120)) / TabCount, scale(24), scale(44));
    for (int index = 0; index < TabCount; ++index) place(navigation[size_t(index)], scale(8), scale(64) + index * navHeight, navigationWidth - scale(16), navHeight - scale(4));
    place(settingsButton, scale(8), height - scale(48), navigationWidth - scale(16), scale(40));
    const bool footer = selectedTab == ProcessesTab || perf || selectedTab == StartupTab || selectedTab == ServicesTab;
    contentBounds = {navigationWidth + scale(4), scale(64), width - scale(12), std::max(scale(140), height - scale(footer ? 48 : 12))};
    const int contentWidth = std::max(1L, contentBounds.right - contentBounds.left);
    const int toolbarY = contentBounds.top + scale(14), buttonHeight = scale(32);
    int right = contentBounds.right - scale(12);
    place(menuButton, right - scale(36), toolbarY, scale(36), buttonHeight); right -= scale(44);
    // Less common commands stay in More actions when a narrow window needs the space.
    const bool wideToolbar = contentWidth >= scale(650);
    show(runButton, wideToolbar); show(exportButton, wideToolbar && !perf);
    if (IsWindowVisible(end)) { place(end, right - scale(128), toolbarY, scale(128), buttonHeight); right -= scale(136); }
    if (wideToolbar && !perf) { place(exportButton, right - scale(112), toolbarY, scale(112), buttonHeight); right -= scale(120); }
    if (wideToolbar) place(runButton, right - scale(148), toolbarY, scale(148), buttonHeight);
    searchBounds = {contentBounds.left + scale(12), scale(12), std::min(contentBounds.right - scale(12), contentBounds.left + scale(552)), scale(48)};
    if (!perf) place(searchBox, searchBounds.left + scale(36), searchBounds.top + scale(8), searchBounds.right - searchBounds.left - scale(48), scale(22));
    const bool information = selectedTab == StartupTab || selectedTab == HistoryTab;
    const int tableTop = contentBounds.top + scale(information ? 94 : 62);
    place(perf ? performance : list, contentBounds.left + scale(1), tableTop, contentWidth - scale(2), contentBounds.bottom - tableTop - scale(1));
    if (selectedTab == HistoryTab) place(historyLink, contentBounds.right - scale(176), contentBounds.top + scale(62), scale(160), scale(25));
    const int footerY = height - scale(38);
    place(fewer, contentBounds.left + scale(4), footerY, scale(150), scale(28));
    place(link, contentBounds.left + scale(8), footerY, scale(200), scale(28));
    if (selectedTab == ServicesTab) place(serviceStatus, contentBounds.left + scale(214), footerY, contentWidth - scale(222), scale(30));
  }
  if (previousFocus && IsChild(window, previousFocus) && !IsWindowVisible(previousFocus)) SetFocus(perf ? performance : list);
  InvalidateRect(window, nullptr, FALSE);
  // Layout changes already invalidate moved child windows; live sampling does not enter this path.
}

void Application::paintWindow(HDC dc) {
  RECT area{}; GetClientRect(window, &area);
  const int saved = SaveDC(dc); if (!saved) return;
  fill(dc, area, uiColor(UiColor::Window));
  if (summary) { RestoreDC(dc, saved); return; }
  SetBkMode(dc, TRANSPARENT);
  if (compact) {
    const RECT line{0, area.bottom - scale(48), area.right, area.bottom - scale(48) + scale(1)};
    fill(dc, line, uiColor(UiColor::Border)); RestoreDC(dc, saved); return;
  }
  if (navigationLabels) {
    RECT brand{scale(58), scale(14), navigationWidth - scale(10), scale(46)};
    SelectObject(dc, font); SetTextColor(dc, uiColor(UiColor::Text)); DrawTextW(dc, L"Task Manager", -1, &brand, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
  }
  rounded(dc, contentBounds, uiColor(UiColor::Surface), uiColor(UiColor::Border), scale(8));
  RECT title{contentBounds.left + scale(20), contentBounds.top + scale(12), contentBounds.right - scale(68), contentBounds.top + scale(47)};
  for (const auto control : {runButton, exportButton, end, menuButton}) if (IsWindowVisible(control)) {
    RECT bounds{}; GetWindowRect(control, &bounds); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);
    title.right = std::min(title.right, bounds.left - scale(12));
  }
  SelectObject(dc, shellHeadingFont); SetTextColor(dc, uiColor(UiColor::Text));
  DrawTextW(dc, navigationNames[selectedTab], -1, &title, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  if (IsWindowVisible(searchBox)) {
    rounded(dc, searchBounds, uiColor(UiColor::Surface), uiColor(UiColor::Border), scale(6));
    const bool focused = GetFocus() == searchBox;
    RECT underline{searchBounds.left + scale(3), searchBounds.bottom - scale(focused ? 2 : 1), searchBounds.right - scale(3), searchBounds.bottom};
    fill(dc, underline, uiColor(focused ? UiColor::Accent : UiColor::MutedText));
    const RECT icon{searchBounds.left + scale(10), searchBounds.top + scale(8), searchBounds.left + scale(30), searchBounds.top + scale(28)};
    glyph(dc, icon, SearchId, uiColor(UiColor::MutedText));
  }
  if (selectedTab == StartupTab) {
    RECT coverage{contentBounds.left + scale(20), contentBounds.top + scale(65), contentBounds.right - scale(20), contentBounds.top + scale(89)};
    SelectObject(dc, font); SetTextColor(dc, uiColor(UiColor::MutedText));
    const auto message = !current || !current->startup->complete || (current->startup->entries.empty() && current->startup->warnings.empty()) ? L"Scanning startup sources..." : current->startup->warnings.empty() ? L"Extended startup inventory" : L"Partial coverage - see coverage rows";
    double bios = 0;
#ifdef TASKMGR_DIAGNOSTICS
    if (!options.demoData)
#endif
    bios = lastBiosSeconds();
    if (bios > 0 && contentBounds.right - contentBounds.left > scale(640)) {
      const auto value = L"Last BIOS time: " + number(bios, 1) + L" seconds";
      RECT bounds = coverage; DrawTextW(dc, value.c_str(), -1, &bounds, DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
      coverage.right -= scale(230);
    }
    DrawTextW(dc, message, -1, &coverage, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  if (selectedTab == HistoryTab) {
    SYSTEMTIME now{}; GetLocalTime(&now); FILETIME file{}; SystemTimeToFileTime(&now, &file); ULARGE_INTEGER value{file.dwLowDateTime, file.dwHighDateTime};
    value.QuadPart -= ULONGLONG(std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - historySince).count()) * 10000000ULL; file = {value.LowPart, value.HighPart};
    SYSTEMTIME local{}; FileTimeToSystemTime(&file, &local); wchar_t date[64]{}; GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &local, nullptr, date, int(std::size(date)), nullptr);
    const auto label = L"Usage since " + std::wstring(date);
    RECT bounds{contentBounds.left + scale(20), contentBounds.top + scale(65), contentBounds.right - scale(188), contentBounds.top + scale(89)};
    SelectObject(dc, font); SetTextColor(dc, uiColor(UiColor::MutedText)); DrawTextW(dc, label.c_str(), -1, &bounds, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
  }
  RestoreDC(dc, saved);
}

void Application::paintShellButton(HWND target, HDC dc, RECT bounds, UINT state) {
  const int saved = SaveDC(dc); if (!saved) return;
  const int id = GetDlgCtrlID(target);
  const bool nav = id >= NavigationId && id < NavigationId + TabCount;
  const bool rail = nav || id == SettingsId || id == NavigationToggleId;
  const bool checked = nav && SendMessageW(target, BM_GETCHECK, 0, 0) == BST_CHECKED;
  const bool disabled = (state & ODS_DISABLED) != 0 || !IsWindowEnabled(target);
  const bool hot = !disabled && GetPropW(target, L"hot") != nullptr;
  const bool pushed = !disabled && (state & ODS_SELECTED) != 0;
  const COLORREF outer = uiColor(rail || compact ? UiColor::Window : UiColor::Surface);
  fill(dc, bounds, outer);
  COLORREF background = checked ? uiColor(UiColor::Selection) : hot || pushed ? uiColor(UiColor::Hover) : outer;
  if (!rail && id != MenuId && !hot && !pushed) background = uiColor(UiColor::SurfaceAlt);
  const auto border = !rail && id != MenuId ? uiColor(UiColor::Border) : background;
  RECT face = bounds; InflateRect(&face, -scale(1), -scale(1));
  rounded(dc, face, background, border, scale(6));
  const auto textColor = disabled ? uiColor(UiColor::MutedText) : checked || (highContrast && (hot || pushed)) ? uiColor(UiColor::SelectionText) : uiColor(UiColor::Text);
  if (checked) {
    RECT marker{face.left, face.top + scale(8), face.left + scale(3), face.bottom - scale(8)};
    rounded(dc, marker, uiColor(UiColor::Accent), uiColor(UiColor::Accent), scale(3));
  }
  const bool label = nav || id == SettingsId ? navigationLabels : id != MenuId && id != NavigationToggleId;
  const int iconSize = std::clamp(int(face.bottom - face.top) - scale(4), scale(12), scale(20));
  const int iconLeft = label ? face.left + scale(12) : (face.left + face.right - iconSize) / 2;
  const int iconTop = (face.top + face.bottom - iconSize) / 2;
  const bool textOnly = compact && id == EndId;
  if (!textOnly) glyph(dc, {iconLeft, iconTop, iconLeft + iconSize, iconTop + iconSize}, id, textColor);
  if (label) {
    wchar_t text[128]{}; GetWindowTextW(target, text, int(std::size(text)));
    RECT textBounds{textOnly ? face.left + scale(8) : iconLeft + iconSize + scale(10), face.top, face.right - scale(10), face.bottom};
    if (id == SettingsId) wcscpy_s(text, L"Settings");
    if (id == ExportId) wcscpy_s(text, L"Export");
    SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, textColor);
    DrawTextW(dc, text, -1, &textBounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (textOnly ? DT_CENTER : DT_LEFT));
  }
  if ((state & ODS_FOCUS) && !(state & ODS_NOFOCUSRECT)) {
    RECT focus = face; InflateRect(&focus, -scale(3), -scale(3)); SetTextColor(dc, textColor); SetBkColor(dc, background); DrawFocusRect(dc, &focus);
  }
  RestoreDC(dc, saved);
}

void Application::showShellMenu(HWND anchor, int submenu) {
  if (compact || summary || !menuBar) return;
  const auto popup = submenu >= 0 && submenu < 3 ? GetSubMenu(menuBar, submenu) : menuBar;
  if (!popup) return;
  RECT bounds{}; GetWindowRect(anchor ? anchor : menuButton, &bounds);
  const UINT alignment = anchor == settingsButton ? TPM_BOTTOMALIGN : TPM_TOPALIGN;
  const int chosen = TrackPopupMenuEx(popup, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_RIGHTBUTTON | alignment,
    bounds.left, anchor == settingsButton ? bounds.top : bounds.bottom + scale(4), window, nullptr);
  if (chosen) { command(chosen); updateFooter(); }
}

LRESULT CALLBACK Application::shellProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR id, DWORD_PTR context) {
  const auto app = reinterpret_cast<Application*>(context);
  const int controlId = GetDlgCtrlID(target);
  const bool nav = controlId >= NavigationId && controlId < NavigationId + TabCount;
  switch (message) {
  case WM_ERASEBKGND: return 1; // All shell buttons paint their full bounds.
  case WM_PAINT: case WM_PRINTCLIENT:
    if (nav) {
      PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(target, &paint) : HDC(word);
      RECT bounds{}; GetClientRect(target, &bounds);
      const auto buttonState = SendMessageW(target, BM_GETSTATE, 0, 0);
      UINT state = (buttonState & BST_FOCUS ? ODS_FOCUS : 0) | (buttonState & BST_PUSHED ? ODS_SELECTED : 0) | (!IsWindowEnabled(target) ? ODS_DISABLED : 0);
      if (SendMessageW(target, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) state |= ODS_NOFOCUSRECT;
      app->paintShellButton(target, dc, bounds, state);
      if (message == WM_PAINT) EndPaint(target, &paint);
      return 0;
    }
    break;
  case WM_GETDLGCODE: {
    const auto input = reinterpret_cast<const MSG*>(data);
    if (nav && input && input->message == WM_KEYDOWN && (input->wParam == VK_UP || input->wParam == VK_DOWN || input->wParam == VK_LEFT || input->wParam == VK_RIGHT || input->wParam == VK_HOME || input->wParam == VK_END || input->wParam == VK_RETURN)) return DefSubclassProc(target, message, word, data) | DLGC_WANTMESSAGE;
    break;
  }
  case WM_KEYDOWN:
    if (nav && word == VK_RETURN) { app->requestTab(controlId - NavigationId); return 0; }
    if (nav && (word == VK_UP || word == VK_DOWN || word == VK_LEFT || word == VK_RIGHT || word == VK_HOME || word == VK_END)) {
      const int current = controlId - NavigationId;
      const int next = word == VK_HOME ? 0 : word == VK_END ? TabCount - 1 : (current + (word == VK_UP || word == VK_LEFT ? TabCount - 1 : 1)) % TabCount;
      SetFocus(app->navigation[size_t(next)]); app->requestTab(next); return 0;
    }
    break;
  case WM_CHAR: if (nav && word == VK_RETURN) return 0; break;
  case WM_SETCURSOR: if (IsWindowEnabled(target)) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; } break;
  case WM_MOUSEMOVE:
    if (!GetPropW(target, L"hot")) { SetPropW(target, L"hot", HANDLE(1)); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, target, 0}; TrackMouseEvent(&track); InvalidateRect(target, nullptr, FALSE); }
    break;
  case WM_MOUSELEAVE: RemovePropW(target, L"hot"); InvalidateRect(target, nullptr, FALSE); break;
  case WM_SETFOCUS: case WM_KILLFOCUS: case WM_UPDATEUISTATE: InvalidateRect(target, nullptr, FALSE); break;
  case WM_NCDESTROY: RemovePropW(target, L"hot"); RemoveWindowSubclass(target, shellProcedure, id); break;
  }
  return DefSubclassProc(target, message, word, data);
}
}
