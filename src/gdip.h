#pragma once
// GDI+ needs min/max, which NOMINMAX removes; give it std's versions.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <objidl.h>
#include <gdiplus.h>

namespace mtm {

inline Gdiplus::Color ToColor(COLORREF c, BYTE alpha = 255) {
    return Gdiplus::Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c));
}

inline void RoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    const float d = std::min(radius * 2, std::min(r.Width, r.Height));
    if (d <= 0.5f) {
        path.AddRectangle(r);
        return;
    }
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    path.CloseFigure();
}

inline void FillRoundRect(Gdiplus::Graphics& g, COLORREF c, const Gdiplus::RectF& r, float radius, BYTE alpha = 255) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, r, radius);
    Gdiplus::SolidBrush brush(ToColor(c, alpha));
    g.FillPath(&brush, &path);
}

inline void StrokeRoundRect(Gdiplus::Graphics& g, COLORREF c, const Gdiplus::RectF& r, float radius, float width = 1.0f,
                            BYTE alpha = 255) {
    Gdiplus::GraphicsPath path;
    RoundRectPath(path, r, radius);
    Gdiplus::Pen pen(ToColor(c, alpha), width);
    g.DrawPath(&pen, &path);
}

}  // namespace mtm
