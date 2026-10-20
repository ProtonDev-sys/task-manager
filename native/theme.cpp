#include "app.hpp"
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace taskmgr {
COLORREF Application::uiColor(UiColor role) const {
  if (highContrast) {
    switch (role) {
    case UiColor::Window: case UiColor::Surface: case UiColor::SurfaceAlt: return GetSysColor(COLOR_WINDOW);
    case UiColor::Accent: case UiColor::Hover: case UiColor::Selection: return GetSysColor(COLOR_HIGHLIGHT);
    case UiColor::SelectionText: return GetSysColor(COLOR_HIGHLIGHTTEXT);
    default: return GetSysColor(COLOR_WINDOWTEXT);
    }
  }
  switch (role) {
  case UiColor::Window: return dark ? RGB(32, 32, 32) : RGB(243, 243, 243);
  case UiColor::Surface: return dark ? RGB(39, 39, 39) : RGB(255, 255, 255);
  case UiColor::SurfaceAlt: return dark ? RGB(44, 44, 44) : RGB(249, 249, 249);
  case UiColor::Border: return dark ? RGB(61, 61, 61) : RGB(229, 229, 229);
  case UiColor::MutedText: return dark ? RGB(192, 192, 192) : RGB(96, 96, 96);
  case UiColor::Accent: return dark ? RGB(96, 205, 255) : RGB(0, 103, 192);
  case UiColor::Hover: return dark ? RGB(51, 51, 51) : RGB(234, 234, 234);
  case UiColor::Selection: return dark ? RGB(37, 60, 74) : RGB(227, 239, 249);
  case UiColor::Text: case UiColor::SelectionText: return dark ? RGB(243, 243, 243) : RGB(27, 27, 27);
  }
  return dark ? RGB(243, 243, 243) : RGB(27, 27, 27);
}
COLORREF Application::themeColor(COLORREF light) const {
  if (highContrast) {
    if (light == RGB(255, 255, 255)) return GetSysColor(COLOR_WINDOW);
    if (light == RGB(205, 232, 255) || light == RGB(229, 243, 255)) return GetSysColor(COLOR_HIGHLIGHT);
    return GetSysColor(COLOR_WINDOWTEXT);
  }
  // Existing graph and performance drawing uses these semantic colour aliases.
  // Keep it on the same palette as the shell without rebuilding any view models.
  if (light == RGB(255, 255, 255)) return uiColor(UiColor::Surface);
  if (light == RGB(0, 0, 0)) return uiColor(UiColor::Text);
  if (light == RGB(205, 232, 255)) return uiColor(UiColor::Selection);
  if (light == RGB(229, 243, 255)) return uiColor(UiColor::Hover);
  if (light == RGB(76, 96, 122)) return uiColor(UiColor::MutedText);
  if (light == RGB(31, 89, 195) || light == RGB(0, 102, 204)) return uiColor(UiColor::Accent);
  if (light == RGB(229, 229, 229)) return uiColor(UiColor::Border);
  if (!dark) return light;
  if (light == RGB(230, 212, 239)) return RGB(88, 53, 104);
  if (light == RGB(244, 236, 248)) return RGB(56, 43, 64);
  const int red = GetRValue(light), green = GetGValue(light), blue = GetBValue(light);
  if (red == green && green == blue) {
    const int shade = red >= 180 ? 64 : red < 80 ? 212 : 174;
    return RGB(shade, shade, shade);
  }
  if (red >= 249 && blue <= 196 && green >= 141) return RGB(72 + (244 - green) / 2, 58 + (244 - green) / 6, 30);
  return RGB(std::min(255, red + 45), std::min(255, green + 45), std::min(255, blue + 45));
}
void Application::applyTheme() {
  HIGHCONTRASTW contrast{sizeof(contrast)};
  highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
  DWORD systemLight = 1, size = sizeof(systemLight);
  if (theme == 2) RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &systemLight, &size);
  dark = !highContrast && (theme == 1 || (theme == 2 && !systemLight));
  if (backgroundBrush) DeleteObject(backgroundBrush);
  backgroundBrush = CreateSolidBrush(themeColor(RGB(255, 255, 255)));
  const BOOL enabled = dark;
  using VersionFunction = LONG (WINAPI*)(OSVERSIONINFOW*);
  const auto versionFunction = reinterpret_cast<VersionFunction>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
  OSVERSIONINFOW version{sizeof(version)};
  if (versionFunction && versionFunction(&version) == 0 && version.dwBuildNumber >= 18362 && version.dwBuildNumber < 22000) {
    const auto library = GetModuleHandleW(L"uxtheme.dll");
    using PreferredMode = int (WINAPI*)(int);
    using AllowWindow = BOOL (WINAPI*)(HWND, BOOL);
    const auto preferredMode = reinterpret_cast<PreferredMode>(GetProcAddress(library, MAKEINTRESOURCEA(135)));
    const auto allowWindow = reinterpret_cast<AllowWindow>(GetProcAddress(library, MAKEINTRESOURCEA(133)));
    if (preferredMode) preferredMode(dark ? 2 : 0);
    if (allowWindow) { allowWindow(window, enabled); allowWindow(list, enabled); }
  }
  DefWindowProcW(window, WM_THEMECHANGED, 0, 0);
  if (FAILED(DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &enabled, sizeof(enabled)))) DwmSetWindowAttribute(window, 19, &enabled, sizeof(enabled));
  const COLORREF caption = highContrast ? DWMWA_COLOR_DEFAULT : uiColor(UiColor::Window);
  const COLORREF captionText = highContrast ? DWMWA_COLOR_DEFAULT : uiColor(UiColor::Text);
  DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
  DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
  ListView_SetBkColor(list, themeColor(RGB(255, 255, 255)));
  ListView_SetTextBkColor(list, themeColor(RGB(255, 255, 255)));
  ListView_SetTextColor(list, themeColor(RGB(0, 0, 0)));
  SetWindowTheme(list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  SetWindowTheme(searchBox, dark ? L"" : nullptr, dark ? L"" : nullptr);
  SendMessageW(end, BM_SETSTYLE, BS_OWNERDRAW, TRUE);
  MENUINFO menu{sizeof(menu)}; menu.fMask = MIM_BACKGROUND; menu.hbrBack = backgroundBrush; SetMenuInfo(menuBar, &menu);
  for (UINT index = 0; index < 3; ++index) {
    MENUITEMINFOW item{sizeof(item)}; item.fMask = MIIM_FTYPE | MIIM_DATA; item.fType = dark || highContrast ? MFT_OWNERDRAW : MFT_STRING; item.dwItemData = index + 1;
    SetMenuItemInfoW(menuBar, index, TRUE, &item);
  }
  DrawMenuBar(window);
  const BOOL active = GetActiveWindow() == window;
  DefWindowProcW(window, WM_NCACTIVATE, !active, 0);
  DefWindowProcW(window, WM_NCACTIVATE, active, 0);
  SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void Application::paintScrollbars(HDC destination) {
  if (!dark || highContrast || !list) return;
  HDC dc = destination ? destination : GetWindowDC(list);
  if (!dc) return;
  const int saved = SaveDC(dc);
  if (!saved) { if (!destination) ReleaseDC(list, dc); return; }
#ifdef TASKMGR_DIAGNOSTICS
  ++scrollbarPaints;
#endif
  RECT windowBounds{}; GetWindowRect(list, &windowBounds);
  auto fill = [&](RECT bounds, COLORREF color) { SetDCBrushColor(dc, color); FillRect(dc, &bounds, GetStockBrush(DC_BRUSH)); };
  SelectObject(dc, GetStockObject(DC_BRUSH)); SelectObject(dc, GetStockObject(NULL_PEN));
  RECT corner{};
  for (const LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
    SCROLLBARINFO info{sizeof(info)};
    if (!GetScrollBarInfo(list, object, &info) || (info.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) continue;
    RECT bounds = info.rcScrollBar; OffsetRect(&bounds, -windowBounds.left, -windowBounds.top);
    const bool vertical = object == OBJID_VSCROLL;
    fill(bounds, uiColor(UiColor::Surface));
    RECT thumb = bounds;
    if (vertical) { thumb.top = bounds.top + info.xyThumbTop; thumb.bottom = bounds.top + info.xyThumbBottom; thumb.left += 2; thumb.right -= 2; }
    else { thumb.left = bounds.left + info.xyThumbTop; thumb.right = bounds.left + info.xyThumbBottom; thumb.top += 2; thumb.bottom -= 2; }
    if (!(info.rgstate[3] & STATE_SYSTEM_UNAVAILABLE)) fill(thumb, info.rgstate[3] & STATE_SYSTEM_PRESSED ? RGB(110, 110, 110) : RGB(76, 76, 76));
    for (int arrow = 0; arrow < 2; ++arrow) {
      const int centerX = vertical ? (bounds.left + bounds.right) / 2 : arrow ? bounds.right - info.dxyLineButton / 2 : bounds.left + info.dxyLineButton / 2;
      const int centerY = vertical ? arrow ? bounds.bottom - info.dxyLineButton / 2 : bounds.top + info.dxyLineButton / 2 : (bounds.top + bounds.bottom) / 2;
      const int direction = arrow ? 1 : -1;
      POINT points[3];
      if (vertical) { points[0] = {centerX - 3, centerY - direction * 2}; points[1] = {centerX + 3, centerY - direction * 2}; points[2] = {centerX, centerY + direction * 2}; }
      else { points[0] = {centerX - direction * 2, centerY - 3}; points[1] = {centerX - direction * 2, centerY + 3}; points[2] = {centerX + direction * 2, centerY}; }
      SetDCBrushColor(dc, info.rgstate[arrow ? 5 : 1] & STATE_SYSTEM_UNAVAILABLE ? RGB(96, 96, 96) : RGB(212, 212, 212)); Polygon(dc, points, 3);
    }
    if (vertical) { corner.left = bounds.left; corner.right = bounds.right; }
    else { corner.top = bounds.top; corner.bottom = bounds.bottom; }
  }
  if (corner.right > corner.left && corner.bottom > corner.top) fill(corner, uiColor(UiColor::Surface));
  RestoreDC(dc, saved);
  if (!destination) ReleaseDC(list, dc);
}
LRESULT CALLBACK Application::tabsProcedure(HWND target, UINT message, WPARAM word, LPARAM data, UINT_PTR, DWORD_PTR context) {
  const auto app = reinterpret_cast<Application*>(context);
  if ((app->dark || app->highContrast) && message == WM_ERASEBKGND) return 1;
  if ((app->dark || app->highContrast) && (message == WM_PAINT || message == WM_PRINTCLIENT)) {
    PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(target, &paint) : HDC(word);
    RECT client{}; GetClientRect(target, &client); FillRect(dc, &client, app->backgroundBrush);
    SelectObject(dc, app->font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, app->themeColor(RGB(0, 0, 0)));
    for (int index = 0; index < TabCount; ++index) {
      RECT box{}; TabCtrl_GetItemRect(target, index, &box);
      const bool selected = index == TabCtrl_GetCurSel(target);
      SetDCBrushColor(dc, app->themeColor(selected ? RGB(205, 232, 255) : app->highContrast ? RGB(255, 255, 255) : RGB(229, 243, 255)));
      FillRect(dc, &box, GetStockBrush(DC_BRUSH));
      SetTextColor(dc, app->highContrast && selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : app->themeColor(RGB(0, 0, 0)));
      wchar_t label[64]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = label; item.cchTextMax = int(std::size(label)); TabCtrl_GetItem(target, index, &item);
      DrawTextW(dc, label, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (message == WM_PAINT) EndPaint(target, &paint);
    return 0;
  }
  return DefSubclassProc(target, message, word, data);
}
}
