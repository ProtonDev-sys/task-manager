#include "app.hpp"
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace taskmgr {
COLORREF Application::themeColor(COLORREF light) const {
  if (highContrast) {
    if (light == RGB(255, 255, 255)) return GetSysColor(COLOR_WINDOW);
    if (light == RGB(205, 232, 255) || light == RGB(229, 243, 255)) return GetSysColor(COLOR_HIGHLIGHT);
    return GetSysColor(COLOR_WINDOWTEXT);
  }
  if (!dark) return light;
  if (light == RGB(255, 255, 255)) return RGB(32, 32, 32);
  if (light == RGB(0, 0, 0)) return RGB(240, 240, 240);
  if (light == RGB(205, 232, 255)) return RGB(40, 74, 103);
  if (light == RGB(229, 243, 255)) return RGB(48, 55, 63);
  if (light == RGB(76, 96, 122)) return RGB(197, 211, 227);
  if (light == RGB(31, 89, 195) || light == RGB(0, 102, 204)) return RGB(112, 183, 255);
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
  const COLORREF caption = dark ? RGB(32, 32, 32) : DWMWA_COLOR_DEFAULT;
  const COLORREF captionText = dark ? RGB(240, 240, 240) : DWMWA_COLOR_DEFAULT;
  DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
  DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &captionText, sizeof(captionText));
  ListView_SetBkColor(list, themeColor(RGB(255, 255, 255)));
  ListView_SetTextBkColor(list, themeColor(RGB(255, 255, 255)));
  ListView_SetTextColor(list, themeColor(RGB(0, 0, 0)));
  SetWindowTheme(list, dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
  SetWindowTheme(searchBox, dark ? L"" : nullptr, dark ? L"" : nullptr);
  SendMessageW(end, BM_SETSTYLE, dark ? BS_OWNERDRAW : BS_PUSHBUTTON, TRUE);
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
    fill(bounds, RGB(32, 32, 32));
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
  if (corner.right > corner.left && corner.bottom > corner.top) fill(corner, RGB(32, 32, 32));
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
      SetDCBrushColor(dc, app->themeColor(index == TabCtrl_GetCurSel(target) ? RGB(205, 232, 255) : RGB(229, 243, 255)));
      FillRect(dc, &box, GetStockBrush(DC_BRUSH));
      SetTextColor(dc, app->highContrast && index == app->selectedTab ? GetSysColor(COLOR_HIGHLIGHTTEXT) : app->themeColor(RGB(0, 0, 0)));
      wchar_t label[64]{}; TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = label; item.cchTextMax = int(std::size(label)); TabCtrl_GetItem(target, index, &item);
      DrawTextW(dc, label, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (message == WM_PAINT) EndPaint(target, &paint);
    return 0;
  }
  return DefSubclassProc(target, message, word, data);
}
}
