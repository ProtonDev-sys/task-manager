#include "core.hpp"
#include <cmath>

namespace taskmgr {
void History::add(double time, double value) {
  if (!std::isfinite(time) || !std::isfinite(value) || (!points.empty() && time < points.back().time)) return;
  if (!points.empty() && time == points.back().time) points.back().value = value;
  else points.push_back({time, value});
  while (points.size() > 2 && points[1].time < time - 60) points.pop_front();
  while (points.size() > 512) points.pop_front();
}
std::vector<GraphPoint> History::window(double now, double duration) const {
  std::vector<GraphPoint> result;
  if (points.empty() || duration <= 0 || !std::isfinite(now)) return result;
  const double left = std::max(points.front().time, now - duration);
  for (size_t index = 0; index < points.size(); ++index) {
    const auto point = points[index];
    if (point.time < left) continue;
    if (point.time > now) break;
    if (result.empty() && index > 0 && point.time > left) {
      const auto previous = points[index - 1];
      const double fraction = (left - previous.time) / (point.time - previous.time);
      result.push_back({left, previous.value + fraction * (point.value - previous.value)});
    }
    result.push_back(point);
  }
  if (!result.empty() && result.back().time < now) result.push_back({now, result.back().value});
  return result;
}
void drawGraph(HDC dc, RECT bounds, const History& history, double now, double maximum, COLORREF color, bool grid) {
  const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  if (width < 2 || height < 2) return;
  Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  const BYTE red = GetRValue(color), green = GetGValue(color), blue = GetBValue(color);
  Gdiplus::Pen border(Gdiplus::Color(255, red, green, blue), 1.25f), gridPen(Gdiplus::Color(45, red, green, blue), 1.0f);
  border.SetLineJoin(Gdiplus::LineJoinRound);
  if (grid) {
    for (int index = 1; index < 10; ++index) graphics.DrawLine(&gridPen, Gdiplus::REAL(bounds.left), Gdiplus::REAL(bounds.top + height * index / 10), Gdiplus::REAL(bounds.right), Gdiplus::REAL(bounds.top + height * index / 10));
    for (int index = 1; index < 12; ++index) graphics.DrawLine(&gridPen, Gdiplus::REAL(bounds.left + width * index / 12), Gdiplus::REAL(bounds.top), Gdiplus::REAL(bounds.left + width * index / 12), Gdiplus::REAL(bounds.bottom));
  }
  const auto values = history.window(now);
  if (!values.empty()) {
    const double left = values.front().time, span = std::max(0.001, now - left);
    std::vector<Gdiplus::PointF> curve; curve.reserve(values.size() + 1);
    for (const auto& point : values) curve.emplace_back(float(bounds.left + (point.time - left) / span * width), float(bounds.bottom - std::clamp(point.value / std::max(1.0, maximum), 0.0, 1.0) * height));
    if (curve.size() == 1) curve.emplace_back(float(bounds.right), curve.front().Y);
    auto polygon = curve; polygon.emplace_back(float(bounds.right), float(bounds.bottom)); polygon.emplace_back(float(bounds.left), float(bounds.bottom));
    Gdiplus::SolidBrush fill(Gdiplus::Color(28, red, green, blue)); graphics.FillPolygon(&fill, polygon.data(), INT(polygon.size()));
    graphics.DrawLines(&border, curve.data(), INT(curve.size()));
  }
  graphics.DrawRectangle(&border, float(bounds.left), float(bounds.top), float(width), float(height));
}
}
