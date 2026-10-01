#include "core.hpp"
#include <cmath>

namespace taskmgr {
void History::add(double time, double value, double secondary) {
  if (!std::isfinite(time) || !std::isfinite(value) || !std::isfinite(secondary) || (used && time < point(used - 1).time)) return;
  if (used && time == point(used - 1).time) { point(used - 1).value = value; point(used - 1).secondary = secondary; }
  else {
    if (used == 512) { first = (first + 1) & (points.size() - 1); --used; }
    if (used == points.size()) {
      std::vector<GraphPoint> next(std::max(size_t(8), points.size() * 2));
      for (size_t index = 0; index < used; ++index) next[index] = point(index);
      points.swap(next); first = 0;
    }
    point(used++) = {time, value, secondary};
  }
  while (used > 2 && point(1).time < time - 60) { first = (first + 1) & (points.size() - 1); --used; }
}
std::vector<GraphPoint> History::window(double now, double duration) const {
  std::vector<GraphPoint> result;
  if (!used || duration <= 0 || !std::isfinite(duration) || !std::isfinite(now)) return result;
  result.reserve(used + 2);
  const double left = std::max(point(0).time, now - duration);
  for (size_t index = 0; index < used; ++index) {
    const auto current = point(index);
    if (current.time < left) continue;
    if (current.time > now) break;
    if (result.empty() && index > 0 && current.time > left) {
      const auto previous = point(index - 1);
      const double fraction = (left - previous.time) / (current.time - previous.time);
      result.push_back({left, previous.value + fraction * (current.value - previous.value), previous.secondary + fraction * (current.secondary - previous.secondary)});
    }
    result.push_back(current);
  }
  if (!result.empty() && result.back().time < now) result.push_back({now, result.back().value, result.back().secondary});
  return result;
}
double History::peak(double now, double duration) const {
  double result = 0;
  if (!used || duration <= 0 || !std::isfinite(duration) || !std::isfinite(now)) return result;
  const double left = std::max(point(0).time, now - duration);
  bool started = false;
  for (size_t index = 0; index < used; ++index) {
    const auto& current = point(index);
    if (current.time < left) continue;
    if (current.time > now) break;
    if (!started && index > 0 && current.time > left) {
      const auto& previous = point(index - 1);
      const double fraction = (left - previous.time) / (current.time - previous.time);
      result = std::max({result, previous.value + fraction * (current.value - previous.value), previous.secondary + fraction * (current.secondary - previous.secondary)});
    }
    started = true;
    result = std::max({result, current.value, current.secondary});
  }
  return result;
}
// Windows 10 graphs: a fixed 60-second axis filled from the right edge, a grid that scrolls with time and a translucent fill.
void drawGraph(HDC dc, RECT bounds, const History& history, double now, double maximum, const GraphStyle& style) {
  const int width = bounds.right - bounds.left, height = bounds.bottom - bounds.top;
  if (width < 2 || height < 2) return;
  Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  const BYTE red = GetRValue(style.color), green = GetGValue(style.color), blue = GetBValue(style.color);
  Gdiplus::Pen line(Gdiplus::Color(255, red, green, blue), 1.0f), gridPen(Gdiplus::Color(40, red, green, blue), 1.0f), border(Gdiplus::Color(255, red, green, blue), 1.0f);
  line.SetLineJoin(Gdiplus::LineJoinRound);
  const float left = float(bounds.left), top = float(bounds.top), right = float(bounds.right - 1), bottom = float(bounds.bottom - 1);
  if (style.grid) {
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
    for (int index = 1; index < 10; ++index) { const float y = std::floor(top + (bottom - top) * index / 10); graphics.DrawLine(&gridPen, left, y, right, y); }
    const double shift = style.scrolling && std::isfinite(now) ? std::fmod(std::max(0.0, now), 3.0) : 0;
    for (int index = 0; index <= 20; ++index) { const float x = std::floor(float(left + (index * 3 - shift) / 60 * (right - left))); if (x > left && x < right) graphics.DrawLine(&gridPen, x, top, x, bottom); }
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
  }
  const auto values = history.window(now);
  auto position = [&](double time, double value) { return Gdiplus::PointF(float(right - (now - time) / 60 * (right - left)), float(bottom - std::clamp(value / std::max(1e-9, maximum), 0.0, 1.0) * (bottom - top))); };
  if (!values.empty()) {
    std::vector<Gdiplus::PointF> curve; curve.reserve(values.size() + 3);
    for (const auto& point : values) curve.push_back(position(point.time, point.value));
    if (curve.size() == 1) curve.insert(curve.begin(), Gdiplus::PointF(curve.front().X - 1, curve.front().Y));
    const auto lineCount = INT(curve.size()); curve.emplace_back(curve.back().X, bottom); curve.emplace_back(curve.front().X, bottom);
    Gdiplus::SolidBrush fill(Gdiplus::Color(30, red, green, blue)); graphics.FillPolygon(&fill, curve.data(), INT(curve.size()));
    graphics.DrawLines(&line, curve.data(), lineCount);
    if (style.secondary) {
      curve.clear(); for (const auto& point : values) if (point.secondary >= 0) curve.push_back(position(point.time, point.secondary));
      Gdiplus::Pen dashed(Gdiplus::Color(255, red, green, blue), 1.0f); dashed.SetDashStyle(Gdiplus::DashStyleDash);
      if (curve.size() >= 2) graphics.DrawLines(&dashed, curve.data(), INT(curve.size()));
    }
  }
  graphics.SetSmoothingMode(Gdiplus::SmoothingModeNone);
  graphics.DrawRectangle(&border, left, top, right - left, bottom - top);
}
}
