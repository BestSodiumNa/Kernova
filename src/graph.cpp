#include "graph.h"

#include "gdip.h"
#include "theme.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace mtm {

namespace {
constexpr wchar_t kWidgetClass[] = L"MtmWidget";

COLORREF TintColor(Graph::Tint t) {
    switch (t) {
    case Graph::Tint::Cpu: return g_pal.cpu;
    case Graph::Tint::Gpu: return g_pal.gpu;
    case Graph::Tint::Memory: return g_pal.mem;
    case Graph::Tint::Disk: return g_pal.disk;
    default: return g_pal.net;
    }
}

// Smooth area chart with a soft glow, drawn into plot.
//
// `phase` (0..1) is how far we are through the current sample interval. The whole
// series is shifted right by (1 - phase) of a step, so between samples the line glides
// left and the newest point slides in from the right edge instead of popping in.
// Pass phase = 1 for a static chart. Callers keep one extra sample beyond `capacity`
// so the left edge stays covered while the series is shifted.
void DrawSeries(Gdiplus::Graphics& g, const std::vector<float>& values, size_t capacity, const RECT& plot,
                double maxV, COLORREF color, bool fill, float width, double phase = 1.0) {
    if (values.size() < 2 || maxV <= 0 || capacity < 2) return;
    const float w = float(plot.right - plot.left), h = float(plot.bottom - plot.top);
    const float step = w / float(capacity - 1);
    const float shift = float(1.0 - std::clamp(phase, 0.0, 1.0)) * step;
    std::vector<Gdiplus::PointF> pts;
    pts.reserve(values.size());
    const size_t n = values.size();
    for (size_t i = 0; i < n; ++i) {
        const double v = std::clamp(double(values[i]) / maxV, 0.0, 1.0);
        pts.emplace_back(float(plot.right) - float(n - 1 - i) * step + shift, float(plot.bottom) - float(v) * h);
    }
    // Clip so the spline's overshoot and the off-screen points never leave the plot.
    g.SetClip(Gdiplus::RectF(float(plot.left), float(plot.top) - width * 2, w, h + width * 2));
    constexpr float kTension = 0.45f;  // Catmull-Rom style curve: smooth but faithful to the samples
    const INT count = static_cast<INT>(pts.size());
    if (fill) {
        Gdiplus::GraphicsPath area;
        area.AddCurve(pts.data(), count, kTension);
        area.AddLine(pts.back(), Gdiplus::PointF(pts.back().X, float(plot.bottom)));
        area.AddLine(Gdiplus::PointF(pts.back().X, float(plot.bottom)), Gdiplus::PointF(pts.front().X, float(plot.bottom)));
        area.CloseFigure();
        Gdiplus::LinearGradientBrush grad(Gdiplus::PointF(0, float(plot.top)), Gdiplus::PointF(0, float(plot.bottom)),
                                          ToColor(color, IsDark() ? 120 : 90), ToColor(color, 0));
        g.FillPath(&grad, &area);
    }
    if (IsDark()) {  // neon glow under the line
        Gdiplus::Pen glow(ToColor(color, 45), width * 4);
        glow.SetLineJoin(Gdiplus::LineJoinRound);
        g.DrawCurve(&glow, pts.data(), count, kTension);
    }
    Gdiplus::Pen pen(ToColor(color), width);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    g.DrawCurve(&pen, pts.data(), count, kTension);
    g.ResetClip();
}

// Value of the series where it crosses the plot's right edge (for the live dot).
double EdgeValue(const std::vector<double>& data, double phase) {
    if (data.empty()) return 0;
    if (data.size() < 2) return data.back();
    const double a = data[data.size() - 2], b = data.back();
    return a + (b - a) * std::clamp(phase, 0.0, 1.0);
}

// Fraction of the current sample interval that has elapsed.
double PhaseOf(const Stopwatch& sincePush) {
    if (!SmoothAnimation()) return 1.0;
    return std::clamp(sincePush.Seconds() * 1000.0 / double(UpdateIntervalMs()), 0.0, 1.0);
}

// Draws a detail line whose parts are separated by '\t' in fixed, equal-width columns, so
// a number gaining a digit ("8%" -> "10%") never pushes the next part sideways.
void DrawColumns(HDC dc, const std::wstring& line, const RECT& r) {
    constexpr UINT kFlags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
    std::vector<std::wstring> parts;
    for (size_t start = 0;;) {
        const size_t tab = line.find(L'\t', start);
        parts.push_back(line.substr(start, tab == std::wstring::npos ? std::wstring::npos : tab - start));
        if (tab == std::wstring::npos) break;
        start = tab + 1;
    }
    const int n = static_cast<int>(parts.size());
    const int colW = (r.right - r.left) / n;
    for (int i = 0; i < n; ++i) {
        RECT c{r.left + i * colW, r.top, i == n - 1 ? r.right : r.left + (i + 1) * colW - Scale(8), r.bottom};
        DrawTextW(dc, parts[size_t(i)].c_str(), -1, &c, kFlags);
    }
}

std::vector<Widget*>& LiveWidgets() {
    static std::vector<Widget*> widgets;
    return widgets;
}

}  // namespace

// ---------------------------------------------------------------- Widget

void Widget::Register() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWidgetClass;
    RegisterClassExW(&wc);
}

void Widget::CreateWidget() {
    hwnd_ = CreateWindowExW(0, kWidgetClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, PageParent(), nullptr, g_inst, this);
    LiveWidgets().push_back(this);
}

Widget::~Widget() {
    auto& live = LiveWidgets();
    live.erase(std::remove(live.begin(), live.end(), this), live.end());
    back_.Free();
    static_.Free();
}

bool Widget::Surface::Ensure(HDC ref, int w, int h) {
    if (dc && size.cx == w && size.cy == h) return false;
    Free();
    dc = CreateCompatibleDC(ref);
    // A 32-bit top-down DIB section: GDI+ renders into it directly, which is several
    // times faster than going through a device-dependent bitmap.
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bmp = CreateDIBSection(ref, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) bmp = CreateCompatibleBitmap(ref, w, h);
    old = SelectObject(dc, bmp);
    size = {w, h};
    return true;
}

void Widget::Surface::Free() {
    if (!dc) return;
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    dc = nullptr;
    bmp = nullptr;
}

void Widget::OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (rc.right > 0 && rc.bottom > 0) {
        back_.Ensure(dc, rc.right, rc.bottom);
        if (Layered()) {
            const bool fresh = static_.Ensure(dc, rc.right, rc.bottom);
            if (fresh || !staticValid_ || staticGeneration_ != VisualGeneration()) {
                PaintStatic(static_.dc, rc);
                staticValid_ = true;
                staticGeneration_ = VisualGeneration();
            }
            BitBlt(back_.dc, 0, 0, rc.right, rc.bottom, static_.dc, 0, 0, SRCCOPY);
            PaintDynamic(back_.dc, rc);
        } else {
            Paint(back_.dc, rc);
        }
        BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
               back_.dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    }
    EndPaint(hwnd_, &ps);
}

void Widget::AnimateFrame() {
    for (Widget* w : LiveWidgets())
        if (w->hwnd_ && IsWindowVisible(w->hwnd_) && w->Animating()) w->Invalidate();
}

LRESULT CALLBACK Widget::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<Widget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (self) {
            self->OnPaint();
            return 0;
        }
        break;
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
        TrackMouseEvent(&tme);  // so widgets get WM_MOUSELEAVE for hover effects
        if (self) self->OnMouse(msg, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
        if (self) self->OnMouse(msg, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL:
        if (self && self->OnWheel(GET_WHEEL_DELTA_WPARAM(wp))) return 0;
        break;  // not ours: DefWindowProc passes it to the parent
    case WM_MOUSELEAVE:
        if (self) self->OnMouse(msg, -1, -1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

RECT PaintCard(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF r(float(rc.left) + 0.5f, float(rc.top) + 0.5f, float(rc.right - rc.left) - 1.0f,
                               float(rc.bottom - rc.top) - 1.0f);
        FillRoundRect(g, g_pal.surface, r, ScaleF(12));
        StrokeRoundRect(g, g_pal.border, r, ScaleF(12), 1.0f, IsDark() ? 255 : 200);
    }
    const int pad = Scale(16);
    return RECT{rc.left + pad, rc.top + Scale(14), rc.right - pad, rc.bottom - Scale(14)};
}

// ---------------------------------------------------------------- Graph

void Graph::Create(Tint tint, double fixedMax) {
    tint_ = tint;
    fixedMax_ = fixedMax;
    CreateWidget();
}

void Graph::Push(double value) {
    data_.push_back(value);
    if (data_.size() > kCapacity + 1) data_.erase(data_.begin());  // +1 covers the left edge while scrolling
    sincePush_.Reset();
    Invalidate();
}

void Graph::SetText(std::wstring title, std::wstring value, std::wstring details) {
    if (title == title_ && value == value_ && details == details_) return;
    title_ = std::move(title);
    value_ = std::move(value);
    details_ = std::move(details);
    InvalidateStatic();
}

void Graph::PaintStatic(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    const COLORREF color = TintColor(tint_);
    SetBkMode(dc, TRANSPARENT);

    // Header: dot + caps title, then the big value.
    const int titleH = Scale(18);
    RECT title{in.left + Scale(14), in.top, in.right, in.top + titleH};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, ValueFont());
    SetTextColor(dc, g_pal.text);
    // With several series the legend stacks to the right of the big value; extra rows push the plot down.
    const int legendRowH = Scale(19);
    const int legendRows = series_.size() >= 2 ? static_cast<int>(series_.size()) : 0;
    RECT value{in.left, in.top + titleH, in.right, in.top + titleH + std::max(Scale(40), legendRows * legendRowH + Scale(4))};
    DrawTextW(dc, value_.c_str(), -1, &value, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SIZE valueSize{};
    GetTextExtentPoint32W(dc, value_.c_str(), static_cast<int>(value_.size()), &valueSize);
    std::vector<RECT> legendDots;
    if (legendRows) {
        // Anchored past the widest likely value ("100%"), so the legend stays put as digits come and go.
        SIZE widest{};
        GetTextExtentPoint32W(dc, L"100%", 4, &widest);
        const int left = in.left + std::max(valueSize.cx, widest.cx) + Scale(20);
        for (int i = 0; i < legendRows; ++i) {
            const RECT row{left, value.top + Scale(4) + i * legendRowH, in.right, value.top + Scale(4) + (i + 1) * legendRowH};
            legendDots.push_back(row);
            const std::wstring& v = size_t(i) < legendValues_.size() ? legendValues_[size_t(i)] : std::wstring();
            SelectObject(dc, UiFontBold());
            SIZE vs{};
            GetTextExtentPoint32W(dc, v.c_str(), static_cast<int>(v.size()), &vs);
            RECT vr = row;
            SetTextColor(dc, TintColor(series_[size_t(i)].tint));
            DrawTextW(dc, v.c_str(), -1, &vr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE wide{};  // reserve room for "100%" so the label never re-truncates as digits change
            GetTextExtentPoint32W(dc, L"100%", 4, &wide);
            const int reserve = std::max(vs.cx, wide.cx);
            RECT lr{row.left + Scale(16), row.top, std::max(row.left + Scale(16), row.right - reserve - Scale(10)), row.bottom};
            SelectObject(dc, UiFont());
            SetTextColor(dc, g_pal.textDim);
            DrawTextW(dc, series_[size_t(i)].label.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
    }

    const int detailsH = Scale(46);
    plot_ = RECT{in.left, value.bottom + Scale(6), in.right, in.bottom - detailsH - Scale(8)};
    if (plot_.bottom < plot_.top + Scale(16)) plot_.bottom = plot_.top + Scale(16);
    // Details: one line per "\n", each clipped with an ellipsis rather than wrapping.
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textDim);
    int y = plot_.bottom + Scale(10);
    size_t start = 0;
    while (start <= details_.size() && y < in.bottom) {
        const size_t nl = details_.find(L'\n', start);
        const std::wstring line = details_.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        RECT lr{in.left, y, in.right, y + Scale(20)};
        DrawColumns(dc, line, lr);
        y += Scale(22);
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush dot(ToColor(color));
    const float d = ScaleF(7);
    g.FillEllipse(&dot, float(in.left), float(in.top) + (float(titleH) - d) / 2, d, d);
    for (size_t i = 0; i < legendDots.size(); ++i) {  // legend swatches: short coloured line with a dot
        const RECT& r = legendDots[i];
        const COLORREF c = TintColor(series_[i].tint);
        const float cy = float(r.top + r.bottom) / 2;
        Gdiplus::Pen line(ToColor(c), ScaleF(2.5f));
        line.SetStartCap(Gdiplus::LineCapRound);
        line.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&line, float(r.left), cy, float(r.left) + ScaleF(10), cy);
    }
    Gdiplus::Pen grid(ToColor(g_pal.text, IsDark() ? 14 : 22), 1);
    for (int i = 0; i <= 4; ++i) {
        const float gy = float(plot_.top) + float(plot_.bottom - plot_.top) * float(i) / 4.0f;
        g.DrawLine(&grid, float(plot_.left), gy, float(plot_.right), gy);
    }
}

void Graph::SetSeries(std::vector<Series> series) {
    bool same = series.size() == series_.size();
    for (size_t i = 0; same && i < series.size(); ++i)
        same = series[i].label == series_[i].label && series[i].tint == series_[i].tint;
    if (same) return;
    series_ = std::move(series);
    multi_.assign(series_.size(), {});
    legendValues_.clear();
    InvalidateStatic();
}

void Graph::PushMany(const std::vector<double>& values, std::vector<std::wstring> legendValues) {
    for (size_t i = 0; i < multi_.size(); ++i) {
        multi_[i].push_back(i < values.size() ? values[i] : 0);
        if (multi_[i].size() > kCapacity + 1) multi_[i].erase(multi_[i].begin());
    }
    if (legendValues != legendValues_) {
        legendValues_ = std::move(legendValues);
        InvalidateStatic();
    }
    sincePush_.Reset();
    Invalidate();
}

void Graph::PaintDynamic(HDC dc, const RECT&) {
    // Single series (data_) or several (multi_), drawn the same way.
    std::vector<std::pair<const std::vector<double>*, COLORREF>> lines;
    if (!multi_.empty()) {
        for (size_t i = 0; i < multi_.size(); ++i) lines.push_back({&multi_[i], TintColor(series_[i].tint)});
    } else {
        lines.push_back({&data_, TintColor(tint_)});
    }
    double maxV = fixedMax_;
    if (maxV <= 0) {
        maxV = 1;
        for (const auto& [d, c] : lines)
            for (double v : *d) maxV = std::max(maxV, v * 1.25);
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const double phase = PhaseOf(sincePush_);
    const bool several = lines.size() > 1;
    // Draw in reverse so the first series (e.g. the dedicated GPU) ends up on top.
    for (size_t k = lines.size(); k-- > 0;) {
        const auto& [data, color] = lines[k];
        std::vector<float> values;
        values.reserve(data->size());
        for (double v : *data) values.push_back(static_cast<float>(v));
        // Only one area fill, or overlapping gradients turn to mud.
        DrawSeries(g, values, kCapacity, plot_, maxV, color, !several || k == 0, ScaleF(several ? 2.0f : 1.8f), phase);
        if (!values.empty()) {  // live point, riding the line where it meets the right edge
            const double v = std::clamp(EdgeValue(*data, phase) / maxV, 0.0, 1.0);
            const float y = float(plot_.bottom) - float(v) * float(plot_.bottom - plot_.top);
            const float r = ScaleF(3.5f);
            Gdiplus::SolidBrush dot(ToColor(color));
            Gdiplus::SolidBrush halo(ToColor(color, 60));
            g.FillEllipse(&halo, float(plot_.right) - r * 2, y - r * 2, r * 4, r * 4);
            g.FillEllipse(&dot, float(plot_.right) - r, y - r, r * 2, r * 2);
        }
    }
}

// ---------------------------------------------------------------- CoreGrid

void CoreGrid::SetCores(std::vector<Core> cores, std::wstring subtitle) {
    // Only the card text and the layout live in the static layer; bars and the per-core
    // numbers ease toward the new sample in PaintDynamic.
    if (cores.size() != cores_.size() || subtitle != subtitle_) InvalidateStatic();
    cores_ = std::move(cores);
    subtitle_ = std::move(subtitle);
    Invalidate();
}

bool CoreGrid::Animating() const {
    if (!SmoothAnimation() || shown_.size() != cores_.size() || vel_.size() != cores_.size()) return false;
    for (size_t i = 0; i < cores_.size(); ++i)
        if (std::abs(shown_[i] - cores_[i].usage) > 0.05 || std::abs(vel_[i]) > 0.05 ||
            std::abs(shownMhz_[i] - double(cores_[i].mhz)) > 0.5 || std::abs(velMhz_[i]) > 0.5)
            return true;
    return false;
}

void CoreGrid::PaintStatic(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    SetBkMode(dc, TRANSPARENT);
    const int titleH = Scale(18);
    RECT title{in.left + Scale(14), in.top, in.right, in.top + titleH};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, L"Logical processors", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, UiFont());
    DrawTextW(dc, subtitle_.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);

    bars_.clear();
    labels_.clear();
    const size_t n = cores_.size();
    if (!n) return;
    const int cols = static_cast<int>(n <= 8 ? n : (n <= 16 ? 8 : (n <= 32 ? 16 : 32)));
    const int rows = static_cast<int>((n + cols - 1) / cols);
    const int top = in.top + titleH + Scale(12);
    const int gap = Scale(6);
    const int cellW = (in.right - in.left - gap * (cols - 1)) / cols;
    const int cellH = (in.bottom - top - gap * (rows - 1)) / rows;
    const int labelH = Scale(30);
    for (size_t i = 0; i < n; ++i) {
        const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
        const RECT cell{in.left + c * (cellW + gap), top + r * (cellH + gap), in.left + c * (cellW + gap) + cellW,
                        top + r * (cellH + gap) + cellH};
        bars_.push_back(RECT{cell.left, cell.top, cell.right, cell.bottom - labelH});
        labels_.push_back(RECT{cell.left, cell.bottom - labelH + Scale(2), cell.right, cell.bottom});
    }

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (const RECT& b : bars_) {
        const Gdiplus::RectF track(float(b.left), float(b.top), float(b.right - b.left), float(b.bottom - b.top));
        if (track.Height >= 4) FillRoundRect(g, g_pal.surface2, track, ScaleF(5));
    }
}

void CoreGrid::PaintDynamic(HDC dc, const RECT&) {
    // Bars and read-outs follow each new sample with a critically damped spring rather
    // than exponential easing. Easing restarts at full speed whenever a sample lands,
    // which reads as a jerk once a second; the spring carries its velocity across
    // samples, so motion stays continuous (no kinks) and settles without overshoot.
    // Its speed scales with the update interval, so it takes about one interval to arrive.
    const size_t n = cores_.size();
    const double dt = std::min(frameClock_.Seconds(), 0.1);
    frameClock_.Reset();
    const bool smooth = SmoothAnimation();
    if (shown_.size() != n || shownMhz_.size() != n || vel_.size() != n || velMhz_.size() != n) {
        shown_.assign(n, 0);
        shownMhz_.assign(n, 0);
        vel_.assign(n, 0);
        velMhz_.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            shown_[i] = cores_[i].usage;
            shownMhz_[i] = cores_[i].mhz;
        }
    }
    const double interval = std::max(0.25, UpdateIntervalMs() / 1000.0);
    const double omega = 5.0 / interval;  // ~95% of the way in one interval
    // Exact step of x'' = omega^2 (target - x) - 2 omega x' over dt (stable at any frame rate).
    auto spring = [&](double& x, double& v, double target) {
        if (!smooth) {
            x = target;
            v = 0;
            return;
        }
        const double y = x - target;
        const double e = std::exp(-omega * dt);
        const double k = v + omega * y;
        x = target + (y + k * dt) * e;
        v = (v - omega * k * dt) * e;
    };
    for (size_t i = 0; i < n; ++i) {
        spring(shown_[i], vel_[i], cores_[i].usage);
        spring(shownMhz_[i], velMhz_[i], double(cores_[i].mhz));
    }

    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (size_t i = 0; i < std::min(n, bars_.size()); ++i) {
            const RECT& b = bars_[i];
            const Gdiplus::RectF track(float(b.left), float(b.top), float(b.right - b.left), float(b.bottom - b.top));
            if (track.Height < 4) continue;
            const float fillH = track.Height * float(std::clamp(shown_[i] / 100.0, 0.0, 1.0));
            if (fillH <= 1) continue;
            const Gdiplus::RectF fill(track.X, track.Y + track.Height - fillH, track.Width, fillH);
            Gdiplus::GraphicsPath path;
            RoundRectPath(path, fill, ScaleF(5));
            Gdiplus::LinearGradientBrush grad(Gdiplus::PointF(0, track.Y), Gdiplus::PointF(0, track.Y + track.Height),
                                              ToColor(Blend(g_pal.cpu, g_pal.mem, 0.6)), ToColor(g_pal.cpu));
            g.FillPath(&grad, &path);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, MonoFont());
    for (size_t i = 0; i < std::min(n, labels_.size()); ++i) {
        RECT label = labels_[i];
        SetTextColor(dc, g_pal.text);
        const std::wstring pct = Format(L"%.0f%%", std::clamp(shown_[i], 0.0, 100.0));
        DrawTextW(dc, pct.c_str(), -1, &label, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SetTextColor(dc, g_pal.textFaint);
        const std::wstring ghz = Format(L"%.2f", std::max(0.0, shownMhz_[i]) / 1000.0);
        label.top += Scale(14);
        DrawTextW(dc, ghz.c_str(), -1, &label, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- Bar

void Bar::SetFraction(double f) {
    fraction_ = std::clamp(f, 0.0, 1.0);
    Invalidate();
}

void Bar::Paint(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float h = float(rc.bottom - rc.top);
    const Gdiplus::RectF track(0, 0, float(rc.right), h);
    FillRoundRect(g, g_pal.surface2, track, h / 2);
    const float w = float(rc.right) * float(fraction_);
    if (w >= h) {
        const Gdiplus::RectF fill(0, 0, w, h);
        Gdiplus::GraphicsPath path;
        RoundRectPath(path, fill, h / 2);
        Gdiplus::LinearGradientBrush grad(Gdiplus::PointF(0, 0), Gdiplus::PointF(w, 0),
                                          ToColor(Blend(g_pal.accent, g_pal.mem, 0.5)), ToColor(g_pal.accent));
        g.FillPath(&grad, &path);
    }
}

// ---------------------------------------------------------------- Timeline

void Timeline::Create(std::function<void(size_t)> onSeek) {
    onSeek_ = std::move(onSeek);
    CreateWidget();
}

void Timeline::SetData(std::vector<float> cpu, std::vector<float> mem, uint64_t firstTime, uint64_t lastTime) {
    cpu_ = std::move(cpu);
    mem_ = std::move(mem);
    first_ = firstTime;
    last_ = lastTime;
    if (cursor_ >= cpu_.size()) cursor_ = cpu_.empty() ? 0 : cpu_.size() - 1;
    Invalidate();
}

void Timeline::SetCursor(size_t index) {
    cursor_ = index;
    Invalidate();
}

RECT Timeline::PlotRect(const RECT& client) const {
    const int pad = Scale(16);
    return RECT{client.left + pad, client.top + Scale(38), client.right - pad, client.bottom - Scale(30)};
}

void Timeline::Paint(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    const RECT plot = PlotRect(rc);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, CapsFont());
    RECT title{in.left, in.top, in.right, in.top + Scale(18)};
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, L"Timeline", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, UiFont());
    const std::wstring legend = L"CPU  ·  Memory";
    SetTextColor(dc, g_pal.textFaint);
    DrawTextW(dc, legend.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, MonoFont());
    RECT times{plot.left, plot.bottom + Scale(6), plot.right, in.bottom};
    if (!cpu_.empty()) {
        DrawTextW(dc, FormatFileTime(first_).c_str(), -1, &times, DT_LEFT | DT_TOP | DT_SINGLELINE);
        DrawTextW(dc, FormatFileTime(last_).c_str(), -1, &times, DT_RIGHT | DT_TOP | DT_SINGLELINE);
    } else {
        DrawTextW(dc, L"No samples yet", -1, &times, DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen grid(ToColor(g_pal.text, IsDark() ? 14 : 22), 1);
    for (int i = 0; i <= 2; ++i) {
        const float y = float(plot.top) + float(plot.bottom - plot.top) * float(i) / 2.0f;
        g.DrawLine(&grid, float(plot.left), y, float(plot.right), y);
    }
    if (cpu_.size() >= 2) {
        DrawSeries(g, mem_, mem_.size(), plot, 100, g_pal.mem, false, ScaleF(1.4f));
        DrawSeries(g, cpu_, cpu_.size(), plot, 100, g_pal.cpu, true, ScaleF(1.6f));
        const float x = float(plot.left) + float(plot.right - plot.left) * float(cursor_) / float(cpu_.size() - 1);
        Gdiplus::Pen cursor(ToColor(g_pal.text, 200), ScaleF(1.5f));
        g.DrawLine(&cursor, x, float(plot.top) - ScaleF(4), x, float(plot.bottom));
        Gdiplus::SolidBrush knob(ToColor(g_pal.accent));
        const float r = ScaleF(5);
        g.FillEllipse(&knob, x - r, float(plot.top) - ScaleF(4) - r, r * 2, r * 2);
    }
}

void Timeline::OnMouse(UINT msg, int x, int) {
    if (msg == WM_LBUTTONDOWN) {
        dragging_ = true;
        SetCapture(hwnd_);
    } else if (msg == WM_LBUTTONUP) {
        dragging_ = false;
        ReleaseCapture();
    }
    if (!dragging_ && msg != WM_LBUTTONUP) return;
    if (cpu_.size() < 2) return;
    RECT client;
    GetClientRect(hwnd_, &client);
    const RECT plot = PlotRect(client);
    const double t = std::clamp(double(x - plot.left) / double(std::max(1L, plot.right - plot.left)), 0.0, 1.0);
    const size_t index = static_cast<size_t>(t * double(cpu_.size() - 1) + 0.5);
    if (index != cursor_ || msg == WM_LBUTTONDOWN) {
        cursor_ = index;
        Invalidate();
        if (onSeek_) onSeek_(index);
    }
}

// ---------------------------------------------------------------- Tile

void Tile::Create(Graph::Tint tint, double fixedMax) {
    tint_ = tint;
    fixedMax_ = fixedMax;
    CreateWidget();
}

void Tile::Push(double value) {
    data_.push_back(value);
    if (data_.size() > kCapacity + 1) data_.erase(data_.begin());  // +1 covers the left edge while scrolling
    sincePush_.Reset();
    Invalidate();
}

void Tile::SetText(std::wstring title, std::wstring value, std::wstring detail) {
    if (title == title_ && value == value_ && detail == detail_) return;
    title_ = std::move(title);
    value_ = std::move(value);
    detail_ = std::move(detail);
    InvalidateStatic();
}

void Tile::PaintStatic(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    const COLORREF color = TintColor(tint_);
    SetBkMode(dc, TRANSPARENT);
    const int titleH = Scale(18);
    RECT title{in.left + Scale(14), in.top, in.right, in.top + titleH};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT value{in.left, title.bottom + Scale(2), in.right, title.bottom + Scale(40)};
    SelectObject(dc, ValueFont());
    SetTextColor(dc, g_pal.text);
    DrawTextW(dc, value_.c_str(), -1, &value, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    RECT detail{in.left, value.bottom, in.right, value.bottom + Scale(20)};
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textDim);
    DrawColumns(dc, detail_, detail);
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::SolidBrush dot(ToColor(color));
    const float d = ScaleF(7);
    g.FillEllipse(&dot, float(in.left), float(in.top) + (float(titleH) - d) / 2, d, d);
    plot_ = RECT{in.left, detail.bottom + Scale(6), in.right, in.bottom};
}

void Tile::PaintDynamic(HDC dc, const RECT&) {
    if (plot_.bottom - plot_.top < Scale(12)) return;
    double maxV = fixedMax_;
    if (maxV <= 0) {
        maxV = 1;
        for (double v : data_) maxV = std::max(maxV, v * 1.25);
    }
    std::vector<float> values;
    values.reserve(data_.size());
    for (double v : data_) values.push_back(static_cast<float>(v));
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    DrawSeries(g, values, kCapacity, plot_, maxV, TintColor(tint_), true, ScaleF(1.5f), PhaseOf(sincePush_));
}

// ---------------------------------------------------------------- ListCard

void ListCard::Create(std::wstring title) {
    title_ = std::move(title);
    CreateWidget();
}

void ListCard::SetLines(std::vector<Line> lines, std::wstring subtitle) {
    lines_ = std::move(lines);
    subtitle_ = std::move(subtitle);
    Invalidate();
}

void ListCard::Paint(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    SetBkMode(dc, TRANSPARENT);
    RECT title{in.left, in.top, in.right, in.top + Scale(18)};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textFaint);
    DrawTextW(dc, subtitle_.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    struct Meter {
        RECT rc;
        double fraction;
        COLORREF color;
    };
    std::vector<Meter> meters;
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, g_dpi);
    int y = title.bottom + Scale(10);
    for (const Line& line : lines_) {
        const bool hasMeter = line.fraction >= 0;
        const int lineH = hasMeter ? Scale(36) : Scale(26);
        if (y + lineH > in.bottom + Scale(4)) break;  // card full: drop the rest
        RECT text{in.left, y, in.right, y + Scale(24)};
        if (line.icon) {
            DrawIconEx(dc, text.left, text.top + (Scale(24) - iconSize) / 2, line.icon, iconSize, iconSize, 0, nullptr, DI_NORMAL);
            text.left += iconSize + Scale(8);
        }
        SelectObject(dc, UiFontBold());
        SIZE vs{};
        GetTextExtentPoint32W(dc, line.value.c_str(), static_cast<int>(line.value.size()), &vs);
        SetTextColor(dc, line.color != CLR_NONE && !hasMeter ? line.color : g_pal.text);
        // The value gets at most 60% of the row (ellipsis beyond) so the label always shows.
        const int cap = line.label.empty() ? int(text.right - text.left) : int(text.right - text.left) * 3 / 5;
        RECT valueRc = text;
        valueRc.left = std::max<LONG>(text.left, text.right - std::min<int>(vs.cx, cap));
        DrawTextW(dc, line.value.c_str(), -1, &valueRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        RECT label = text;
        label.right = std::max(label.left, valueRc.left - Scale(12));
        SelectObject(dc, UiFont());
        SetTextColor(dc, g_pal.textDim);
        DrawTextW(dc, line.label.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (hasMeter)
            meters.push_back({RECT{text.left, text.bottom + Scale(3), in.right, text.bottom + Scale(7)},
                              std::clamp(line.fraction, 0.0, 1.0), line.color != CLR_NONE ? line.color : g_pal.accent});
        y += lineH;
    }
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    for (const Meter& m : meters) {
        const float h = float(m.rc.bottom - m.rc.top);
        const Gdiplus::RectF track(float(m.rc.left), float(m.rc.top), float(m.rc.right - m.rc.left), h);
        FillRoundRect(g, g_pal.surface2, track, h / 2);
        const float w = track.Width * float(m.fraction);
        if (w >= h) FillRoundRect(g, m.color, Gdiplus::RectF(track.X, track.Y, w, h), h / 2);
    }
}

// ---------------------------------------------------------------- ActionCard

void ActionCard::Create(wchar_t glyph, std::wstring title, Graph::Tint tint, std::function<void()> onClick) {
    glyph_ = glyph;
    title_ = std::move(title);
    tint_ = tint;
    onClick_ = std::move(onClick);
    CreateWidget();
}

void ActionCard::SetText(std::wstring value, std::wstring line1, std::wstring line2) {
    if (value == value_ && line1 == line1_ && line2 == line2_) return;
    value_ = std::move(value);
    line1_ = std::move(line1);
    line2_ = std::move(line2);
    Invalidate();
}

void ActionCard::SetSelected(bool selected) {
    if (selected == selected_) return;
    selected_ = selected;
    Invalidate();
}

void ActionCard::OnMouse(UINT msg, int x, int) {
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
        const bool hover = x >= 0;
        if (hover && onClick_) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        if (hover != hover_) {
            hover_ = hover;
            Invalidate();
        }
    } else if (msg == WM_LBUTTONUP && onClick_) {
        onClick_();
    }
}

void ActionCard::Paint(HDC dc, const RECT& rc) {
    const COLORREF color = TintColor(tint_);
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF r(float(rc.left) + 0.5f, float(rc.top) + 0.5f, float(rc.right - rc.left) - 1.0f,
                               float(rc.bottom - rc.top) - 1.0f);
        FillRoundRect(g, hover_ ? Blend(g_pal.surface, g_pal.hover, 0.6) : g_pal.surface, r, ScaleF(12));
        if (selected_) {
            StrokeRoundRect(g, color, r, ScaleF(12), ScaleF(1.5f));
            // soft tinted wash so the selected card reads at a glance
            FillRoundRect(g, color, r, ScaleF(12), IsDark() ? 18 : 14);
        } else {
            StrokeRoundRect(g, g_pal.border, r, ScaleF(12), 1.0f, IsDark() ? 255 : 200);
        }
        // Glyph badge.
        const float b = ScaleF(36);
        const Gdiplus::RectF badge(float(rc.left) + ScaleF(16), float(rc.top) + ScaleF(16), b, b);
        FillRoundRect(g, color, badge, ScaleF(9), IsDark() ? 48 : 36);
    }
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(16), b = Scale(36);
    RECT badge{rc.left + pad, rc.top + pad, rc.left + pad + b, rc.top + pad + b};
    HGDIOBJ old = SelectObject(dc, IconFont());
    SetTextColor(dc, color);
    const wchar_t glyph[2] = {glyph_, 0};
    DrawTextW(dc, glyph, 1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT title{badge.right + Scale(12), badge.top, rc.right - pad, badge.top + Scale(18)};
    SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT sub{title.left, title.bottom, title.right, badge.bottom};
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textFaint);
    DrawTextW(dc, line2_.c_str(), -1, &sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    RECT value{rc.left + pad, badge.bottom + Scale(10), rc.right - pad, badge.bottom + Scale(50)};
    SelectObject(dc, ValueFont());
    SetTextColor(dc, value_.empty() ? g_pal.textFaint : g_pal.text);
    const std::wstring v = value_.empty() ? L"—" : value_;
    DrawTextW(dc, v.c_str(), -1, &value, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT line{value.left, value.bottom + Scale(2), value.right, value.bottom + Scale(22)};
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textDim);
    DrawTextW(dc, line1_.c_str(), -1, &line, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- Chart

void Chart::Create(Graph::Tint tint) {
    tint_ = tint;
    CreateWidget();
}

void Chart::SetSeries(std::vector<double> values, double interval, std::wstring title, std::wstring unit,
                      std::wstring note) {
    values_ = std::move(values);
    interval_ = interval > 0 ? interval : 1;
    title_ = std::move(title);
    unit_ = std::move(unit);
    note_ = std::move(note);
    Invalidate();
}

void Chart::OnMouse(UINT msg, int x, int) {
    if (msg != WM_MOUSEMOVE && msg != WM_MOUSELEAVE) return;
    const int hx = x >= plot_.left && x <= plot_.right ? x : -1;
    if (hx != hoverX_) {
        hoverX_ = hx;
        Invalidate();
    }
}

namespace {
// Rounds up to 1, 2 or 5 x 10^n so axis labels are tidy.
double NiceCeil(double v) {
    if (v <= 0) return 1;
    const double p = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (v <= m * p) return m * p;
    return 10 * p;
}

std::wstring AxisNumber(double v) {
    if (v >= 10000) return Format(L"%.0fk", v / 1000);
    if (v >= 100 || v == std::floor(v)) return Format(L"%.0f", v);
    return Format(L"%.1f", v);
}
}  // namespace

void Chart::Paint(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    const COLORREF color = TintColor(tint_);
    SetBkMode(dc, TRANSPARENT);
    RECT title{in.left, in.top, in.right, in.top + Scale(18)};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textFaint);
    DrawTextW(dc, note_.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (values_.size() < 2) {
        RECT msg{in.left, title.bottom, in.right, in.bottom};
        DrawTextW(dc, L"No timeline for this result.", -1, &msg, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        plot_ = {};
        SelectObject(dc, old);
        return;
    }

    double peak = 0, sum = 0;
    for (double v : values_) {
        peak = std::max(peak, v);
        sum += v;
    }
    const double avg = sum / double(values_.size());
    const double maxV = NiceCeil(peak * 1.1);
    const int axisW = Scale(44), axisH = Scale(20);
    plot_ = RECT{in.left + axisW, title.bottom + Scale(14), in.right, in.bottom - axisH};
    if (plot_.bottom <= plot_.top + Scale(20)) plot_.bottom = plot_.top + Scale(20);

    // Y labels (value + unit on top) and X labels (seconds).
    SelectObject(dc, MonoFont());
    SetTextColor(dc, g_pal.textFaint);
    for (int i = 0; i <= 4; ++i) {
        const int y = plot_.bottom - (plot_.bottom - plot_.top) * i / 4;
        RECT lr{in.left, y - Scale(9), plot_.left - Scale(8), y + Scale(9)};
        const std::wstring text = AxisNumber(maxV * i / 4);
        DrawTextW(dc, text.c_str(), -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    const double duration = interval_ * double(values_.size() - 1);
    // Whole-second ticks (1, 2 or 5 s apart), each placed at its true position.
    const double step = duration <= 8 ? 1 : duration <= 16 ? 2 : NiceCeil(duration / 8);
    for (double s = 0; s <= duration + 1e-6; s += step) {
        const int x = plot_.left + static_cast<int>((plot_.right - plot_.left) * (s / std::max(duration, 1e-9)));
        if (s > 0 && plot_.right - x < Scale(24)) break;  // would collide with the edge
        RECT lr{s == 0 ? x : x - Scale(30), plot_.bottom + Scale(4), s == 0 ? x + Scale(60) : x + Scale(30), in.bottom + Scale(4)};
        const std::wstring text = Format(L"%.0fs", s);
        DrawTextW(dc, text.c_str(), -1, &lr, (s == 0 ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE);
    }

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen grid(ToColor(g_pal.text, IsDark() ? 14 : 22), 1);
    for (int i = 0; i <= 4; ++i) {
        const float gy = float(plot_.top) + float(plot_.bottom - plot_.top) * float(i) / 4.0f;
        g.DrawLine(&grid, float(plot_.left), gy, float(plot_.right), gy);
    }
    std::vector<float> values;
    values.reserve(values_.size());
    for (double v : values_) values.push_back(static_cast<float>(v));
    DrawSeries(g, values, values.size(), plot_, maxV, color, true, ScaleF(2.0f));

    // Average line.
    const float h = float(plot_.bottom - plot_.top);
    const float ay = float(plot_.bottom) - float(avg / maxV) * h;
    Gdiplus::Pen dash(ToColor(g_pal.textDim, 160), ScaleF(1.2f));
    dash.SetDashStyle(Gdiplus::DashStyleDash);
    g.DrawLine(&dash, float(plot_.left), ay, float(plot_.right), ay);
    // "avg" read-out in a small pill on the line's right end, readable over the curve.
    SelectObject(dc, UiFont());
    const std::wstring avgText = L"avg " + AxisNumber(avg) + L" " + unit_;
    SIZE as{};
    GetTextExtentPoint32W(dc, avgText.c_str(), static_cast<int>(avgText.size()), &as);
    const int pw = as.cx + Scale(14), ph = as.cy + Scale(4);
    const int py = std::clamp(int(ay) - ph / 2, int(plot_.top), int(plot_.bottom) - ph);
    RECT avgLabel{plot_.right - pw, py, plot_.right, py + ph};
    const Gdiplus::RectF pill(float(avgLabel.left), float(avgLabel.top), float(pw), float(ph));
    FillRoundRect(g, g_pal.surface2, pill, float(ph) / 2);
    StrokeRoundRect(g, g_pal.border, pill, float(ph) / 2);
    g.Flush(Gdiplus::FlushIntentionSync);
    SetTextColor(dc, g_pal.textDim);
    DrawTextW(dc, avgText.c_str(), -1, &avgLabel, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Hover read-out.
    if (hoverX_ >= 0) {
        const double t = double(hoverX_ - plot_.left) / double(std::max(1L, plot_.right - plot_.left));
        const size_t i = std::min(values_.size() - 1, static_cast<size_t>(std::lround(t * double(values_.size() - 1))));
        const float px = float(plot_.left) + float(i) / float(values_.size() - 1) * float(plot_.right - plot_.left);
        const float py = float(plot_.bottom) - float(std::clamp(values_[i] / maxV, 0.0, 1.0)) * h;
        Gdiplus::Pen cursor(ToColor(g_pal.text, 90), 1);
        g.DrawLine(&cursor, px, float(plot_.top), px, float(plot_.bottom));
        const float r = ScaleF(4);
        Gdiplus::SolidBrush halo(ToColor(color, 70)), dot(ToColor(color));
        g.FillEllipse(&halo, px - r * 2, py - r * 2, r * 4, r * 4);
        g.FillEllipse(&dot, px - r, py - r, r * 2, r * 2);

        const std::wstring tip = Format(L"%.2fs  ·  %s %s", interval_ * double(i), AxisNumber(values_[i]).c_str(), unit_.c_str());
        SelectObject(dc, UiFontBold());
        SIZE ts{};
        GetTextExtentPoint32W(dc, tip.c_str(), static_cast<int>(tip.size()), &ts);
        const int bw = ts.cx + Scale(16), bh = ts.cy + Scale(8);
        int bx = int(px) + Scale(10);
        if (bx + bw > plot_.right) bx = int(px) - Scale(10) - bw;
        const int by = plot_.top;
        FillRoundRect(g, g_pal.surface2, Gdiplus::RectF(float(bx), float(by), float(bw), float(bh)), ScaleF(6));
        StrokeRoundRect(g, g_pal.border, Gdiplus::RectF(float(bx), float(by), float(bw), float(bh)), ScaleF(6));
        g.Flush(Gdiplus::FlushIntentionSync);
        RECT tr{bx, by, bx + bw, by + bh};
        SetTextColor(dc, g_pal.text);
        DrawTextW(dc, tip.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- Rich tooltip

namespace {

struct TipState {
    HWND hwnd = nullptr;
    std::wstring title;
    std::vector<TipSection> sections;
} g_tip;

constexpr int kTipWidth = 360;

// Lays out (or paints, when `paint`) the tooltip content; returns the height used.
int TipLayout(HDC dc, int width, bool paint) {
    const int pad = Scale(14);
    const UINT wrap = DT_LEFT | DT_WORDBREAK | DT_NOPREFIX;
    int y = pad;
    HGDIOBJ old = SelectObject(dc, UiFontBold());
    RECT r{pad, y, width - pad, y};
    DrawTextW(dc, g_tip.title.c_str(), -1, &r, wrap | DT_CALCRECT);
    r.right = width - pad;
    if (paint) {
        SetTextColor(dc, g_pal.text);
        DrawTextW(dc, g_tip.title.c_str(), -1, &r, wrap);
    }
    y = r.bottom + Scale(8);
    for (const TipSection& s : g_tip.sections) {
        if (!s.heading.empty()) {
            SelectObject(dc, CapsFont());
            RECT h{pad, y, width - pad, y + Scale(16)};
            if (paint) {
                SetTextColor(dc, g_pal.accent);
                DrawCapsText(dc, s.heading, h, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            y = h.bottom + Scale(2);
        }
        SelectObject(dc, UiFont());
        RECT t{pad, y, width - pad, y};
        DrawTextW(dc, s.text.c_str(), -1, &t, wrap | DT_CALCRECT);
        t.right = width - pad;
        if (paint) {
            SetTextColor(dc, g_pal.textDim);
            DrawTextW(dc, s.text.c_str(), -1, &t, wrap);
        }
        y = t.bottom + Scale(10);
    }
    SelectObject(dc, old);
    return y - Scale(10) + pad;
}

LRESULT CALLBACK TipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        FillRect(mem, &rc, BrushFor(g_pal.surface2));
        HBRUSH border = CreateSolidBrush(g_pal.border);
        FrameRect(mem, &rc, border);
        DeleteObject(border);
        SetBkMode(mem, TRANSPARENT);
        TipLayout(mem, rc.right, true);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void ShowRichTip(const RECT& anchor, const std::wstring& title, const std::vector<TipSection>& sections) {
    if (!g_tip.hwnd) {
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = TipProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"MtmRichTip";
        wc.style = CS_DROPSHADOW;
        RegisterClassExW(&wc);
        g_tip.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                                     0, 0, 0, 0, g_main, nullptr, g_inst, nullptr);
        const DWORD round = 3;  // DWMWCP_ROUNDSMALL
        DwmSetWindowAttribute(g_tip.hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    }
    g_tip.title = title;
    g_tip.sections = sections;
    const int width = Scale(kTipWidth);
    HDC dc = GetDC(g_tip.hwnd);
    const int height = TipLayout(dc, width, false);
    ReleaseDC(g_tip.hwnd, dc);

    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &mi);
    const RECT work = mi.rcWork;
    const int gap = Scale(6);
    int x = std::clamp<int>(anchor.left, work.left, std::max<int>(work.left, work.right - width));
    int y = anchor.bottom + gap;
    if (y + height > work.bottom) y = anchor.top - gap - height;  // no room below: go above
    if (y < work.top) {  // still no room: beside the anchor
        y = std::clamp<int>(anchor.top, work.top, std::max<int>(work.top, work.bottom - height));
        x = anchor.right + gap + width <= work.right ? anchor.right + gap : anchor.left - gap - width;
    }
    SetWindowPos(g_tip.hwnd, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_tip.hwnd, nullptr, FALSE);
}

void HideRichTip() {
    if (g_tip.hwnd) ShowWindow(g_tip.hwnd, SW_HIDE);
}

// ---------------------------------------------------------------- OptionGrid

void OptionGrid::Create(std::function<void(size_t)> onSelect) {
    onSelect_ = std::move(onSelect);
    CreateWidget();
}

void OptionGrid::SetItems(std::vector<Item> items) {
    items_ = std::move(items);
    if (selected_ != SIZE_MAX && selected_ >= items_.size()) selected_ = SIZE_MAX;
    Invalidate();
}

void OptionGrid::SetBadge(size_t index, std::wstring badge, COLORREF color) {
    if (index >= items_.size()) return;
    items_[index].badge = std::move(badge);
    items_[index].badgeColor = color;
    Invalidate();
}

void OptionGrid::SetSelected(size_t index) {
    if (index == selected_) return;
    selected_ = index;
    Invalidate();
}

void OptionGrid::Grid(int width, int& cols, int& tileW) const {
    const int gap = Scale(10);
    cols = std::max(1, (width + gap) / (Scale(250) + gap));
    tileW = (width - gap * (cols - 1)) / cols;
}

int OptionGrid::HeightFor(int width) const {
    int cols, tileW;
    Grid(width, cols, tileW);
    const int rows = static_cast<int>((items_.size() + size_t(cols) - 1) / size_t(cols));
    return rows * Scale(92) + std::max(0, rows - 1) * Scale(10);
}

int OptionGrid::HitTest(int x, int y) const {
    for (size_t i = 0; i < tiles_.size(); ++i)
        if (x >= tiles_[i].left && x < tiles_[i].right && y >= tiles_[i].top && y < tiles_[i].bottom) return static_cast<int>(i);
    return -1;
}

void OptionGrid::OnMouse(UINT msg, int x, int y) {
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
        const int hit = x < 0 ? -1 : HitTest(x, y);
        if (hit >= 0) SetCursor(LoadCursorW(nullptr, IDC_HAND));
        if (hit == hover_) return;
        hover_ = hit;
        Invalidate();
        if (hit < 0 || items_[size_t(hit)].tip.empty()) {
            HideRichTip();
            return;
        }
        RECT r = tiles_[size_t(hit)];
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&r), 2);
        ShowRichTip(r, items_[size_t(hit)].tipTitle, items_[size_t(hit)].tip);
    } else if (msg == WM_LBUTTONUP) {
        const int hit = HitTest(x, y);
        if (hit < 0) return;
        SetSelected(size_t(hit));
        if (onSelect_) onSelect_(size_t(hit));
    }
}

void OptionGrid::Paint(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    tiles_.clear();
    // Scroll when the tiles don't fit (small windows): reserve a thin bar on the right.
    const int barW = Scale(4);
    const int viewH = rc.bottom - rc.top;
    const bool scrolls = HeightFor(rc.right - rc.left) > viewH;
    const int gridW = (rc.right - rc.left) - (scrolls ? barW + Scale(8) : 0);
    const int contentH = HeightFor(gridW);
    scroll_ = std::clamp(scroll_, 0, std::max(0, contentH - viewH));
    int cols, tileW;
    Grid(gridW, cols, tileW);
    const int gap = Scale(10), tileH = Scale(92), pad = Scale(14);
    for (size_t i = 0; i < items_.size(); ++i) {
        const int c = static_cast<int>(i) % cols, r = static_cast<int>(i) / cols;
        const int top = rc.top + r * (tileH + gap) - scroll_;
        tiles_.push_back(RECT{rc.left + c * (tileW + gap), top, rc.left + c * (tileW + gap) + tileW, top + tileH});
    }
    if (scrolls) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float x = float(rc.right - barW), r = float(barW) / 2;
        FillRoundRect(g, g_pal.surface2, Gdiplus::RectF(x, float(rc.top), float(barW), float(viewH)), r);
        const float thumbH = std::max(ScaleF(24), float(viewH) * float(viewH) / float(contentH));
        const float thumbY = float(rc.top) + (float(viewH) - thumbH) * float(scroll_) / float(std::max(1, contentH - viewH));
        FillRoundRect(g, g_pal.textFaint, Gdiplus::RectF(x, thumbY, float(barW), thumbH), r);
    }
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const RECT& t = tiles_[i];
            const Gdiplus::RectF f(float(t.left) + 0.5f, float(t.top) + 0.5f, float(t.right - t.left) - 1, float(t.bottom - t.top) - 1);
            const bool hover = static_cast<int>(i) == hover_;
            FillRoundRect(g, hover ? Blend(g_pal.surface, g_pal.hover, 0.6) : g_pal.surface, f, ScaleF(10));
            if (i == selected_) {
                FillRoundRect(g, g_pal.accent, f, ScaleF(10), IsDark() ? 22 : 16);
                StrokeRoundRect(g, g_pal.accent, f, ScaleF(10), ScaleF(1.6f));
            } else {
                StrokeRoundRect(g, g_pal.border, f, ScaleF(10), 1.0f, IsDark() ? 255 : 200);
            }
            if (items_[i].marked) {  // "in use" chip, top right
                HGDIOBJ o = SelectObject(dc, CapsFont());
                SIZE s{};
                const std::wstring caps = ToUpper(items_[i].markText);
                GetTextExtentPoint32W(dc, caps.c_str(), static_cast<int>(caps.size()), &s);
                s.cx += Scale(2) * static_cast<int>(caps.size());  // DrawCapsText letter spacing
                SelectObject(dc, o);
                const float w = float(s.cx) + ScaleF(14), h = ScaleF(18);
                FillRoundRect(g, g_pal.good, Gdiplus::RectF(float(t.right) - pad - w, float(t.top) + ScaleF(12), w, h), h / 2,
                              IsDark() ? 50 : 40);
            }
        }
    }
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, UiFont());
    for (size_t i = 0; i < tiles_.size(); ++i) {
        const Item& it = items_[i];
        const RECT& t = tiles_[i];
        int right = t.right - pad;
        if (it.marked) {
            SelectObject(dc, CapsFont());
            SIZE s{};
            const std::wstring caps = ToUpper(it.markText);
            GetTextExtentPoint32W(dc, caps.c_str(), static_cast<int>(caps.size()), &s);
            s.cx += Scale(2) * static_cast<int>(caps.size());
            RECT chip{t.right - pad - s.cx - Scale(14), t.top + Scale(12), t.right - pad, t.top + Scale(30)};
            SetTextColor(dc, g_pal.good);
            DrawCapsText(dc, it.markText, chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            right = chip.left - Scale(8);
        }
        if (!it.badge.empty()) {  // latency, left of the "in use" chip when both show
            SelectObject(dc, UiFontBold());
            RECT b{t.left + pad, t.top + Scale(12), right, t.top + Scale(30)};
            SetTextColor(dc, it.badgeColor != CLR_NONE ? it.badgeColor : g_pal.textDim);
            DrawTextW(dc, it.badge.c_str(), -1, &b, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SIZE s{};
            GetTextExtentPoint32W(dc, it.badge.c_str(), static_cast<int>(it.badge.size()), &s);
            right = b.right - s.cx - Scale(8);
        }
        SelectObject(dc, UiFontBold());
        SetTextColor(dc, g_pal.text);
        RECT title{t.left + pad, t.top + Scale(12), right, t.top + Scale(30)};
        DrawTextW(dc, it.title.c_str(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, MonoFont());
        SetTextColor(dc, g_pal.textDim);
        RECT detail{t.left + pad, title.bottom + Scale(6), t.right - pad, title.bottom + Scale(24)};
        DrawTextW(dc, it.detail.c_str(), -1, &detail, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, CapsFont());
        SetTextColor(dc, i == selected_ ? g_pal.accent : g_pal.textFaint);
        RECT tags{t.left + pad, detail.bottom + Scale(6), t.right - pad, detail.bottom + Scale(24)};
        DrawCapsText(dc, it.tags, tags, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, UiFont());
    }
    SelectObject(dc, old);
}

// ---------------------------------------------------------------- Treemap

namespace {

struct RectD {
    double x, y, w, h;
};

// Squarified treemap layout (Bruls, Huizing & van Wijk): areas must be sorted descending
// and sum to rect.w * rect.h. Rows are added while they keep blocks closer to square.
std::vector<RectD> Squarify(const std::vector<double>& areas, RectD rect) {
    std::vector<RectD> out(areas.size());
    size_t i = 0;
    while (i < areas.size() && rect.w > 0.5 && rect.h > 0.5) {
        const double side = std::min(rect.w, rect.h);
        size_t end = i;
        double sum = 0, rmin = 1e300, rmax = 0, worst = 1e300;
        while (end < areas.size()) {
            const double a = std::max(areas[end], 1e-9);
            const double s = sum + a, mn = std::min(rmin, a), mx = std::max(rmax, a);
            const double ratio = std::max(side * side * mx / (s * s), s * s / (side * side * mn));
            if (end > i && ratio > worst) break;
            sum = s;
            rmin = mn;
            rmax = mx;
            worst = ratio;
            ++end;
        }
        if (rect.w >= rect.h) {  // vertical strip on the left
            const double colW = sum / rect.h;
            double y = rect.y;
            for (size_t k = i; k < end; ++k) {
                const double h = areas[k] / colW;
                out[k] = {rect.x, y, colW, h};
                y += h;
            }
            rect.x += colW;
            rect.w -= colW;
        } else {  // horizontal strip on top
            const double rowH = sum / rect.w;
            double x = rect.x;
            for (size_t k = i; k < end; ++k) {
                const double w = areas[k] / rowH;
                out[k] = {x, rect.y, w, rowH};
                x += w;
            }
            rect.y += rowH;
            rect.h -= rowH;
        }
        i = end;
    }
    return out;
}

COLORREF BlockColor(size_t index) {
    const COLORREF palette[] = {g_pal.cpu, g_pal.mem, g_pal.disk, g_pal.net, g_pal.gpu, g_pal.good, g_pal.warn};
    const COLORREF base = palette[index % ARRAYSIZE(palette)];
    return Blend(base, g_pal.surface, IsDark() ? 0.35 : 0.2);
}

COLORREF TextOn(COLORREF fill) {
    const double luma = 0.299 * GetRValue(fill) + 0.587 * GetGValue(fill) + 0.114 * GetBValue(fill);
    return luma > 150 ? RGB(15, 20, 28) : RGB(245, 248, 252);
}

}  // namespace

void Treemap::Create(std::function<void(uint64_t, bool)> onPick) {
    onPick_ = std::move(onPick);
    CreateWidget();
}

void Treemap::SetItems(std::vector<Item> items, std::wstring title) {
    constexpr size_t kMaxBlocks = 40;
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& it) { return it.size == 0; }), items.end());
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.size > b.size; });
    if (items.size() > kMaxBlocks) {  // fold the long tail into one block
        Item other{L"", 0, kOtherTag};
        for (size_t i = kMaxBlocks - 1; i < items.size(); ++i) other.size += items[i].size;
        other.label = Format(L"%zu more", items.size() - (kMaxBlocks - 1));
        items.resize(kMaxBlocks - 1);
        items.push_back(std::move(other));
    }
    total_ = 0;
    for (const Item& it : items) total_ += it.size;
    items_ = std::move(items);
    title_ = std::move(title);
    hover_ = -1;
    Invalidate();
}

void Treemap::SetSelected(uint64_t tag) {
    if (tag == selected_) return;
    selected_ = tag;
    Invalidate();
}

int Treemap::HitTest(int x, int y) const {
    const POINT pt{x, y};
    for (size_t i = 0; i < blocks_.size(); ++i)
        if (PtInRect(&blocks_[i].rc, pt)) return static_cast<int>(i);
    return -1;
}

void Treemap::Paint(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    SetBkMode(dc, TRANSPARENT);
    RECT title{in.left, in.top, in.right, in.top + Scale(18)};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // Right side: the hovered block's details, otherwise a hint.
    std::wstring info = L"Click to select  ·  double-click a folder to open it";
    if (hover_ >= 0 && hover_ < static_cast<int>(blocks_.size())) {
        const Item& it = items_[blocks_[static_cast<size_t>(hover_)].item];
        info = Format(L"%s  ·  %s  ·  %.1f%%", it.label.c_str(), FormatBytes(it.size).c_str(),
                      total_ ? 100.0 * double(it.size) / double(total_) : 0.0);
    }
    SelectObject(dc, UiFont());
    SetTextColor(dc, hover_ >= 0 ? g_pal.text : g_pal.textFaint);
    DrawTextW(dc, info.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    const RECT area{in.left, title.bottom + Scale(10), in.right, in.bottom};
    blocks_.clear();
    if (items_.empty() || !total_ || area.right <= area.left || area.bottom <= area.top) {
        SetTextColor(dc, g_pal.textFaint);
        RECT empty = area;
        DrawTextW(dc, L"Scan a folder to see what's using the space.", -1, &empty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);
        return;
    }
    const double W = area.right - area.left, H = area.bottom - area.top;
    std::vector<double> areas;
    areas.reserve(items_.size());
    for (const Item& it : items_) areas.push_back(double(it.size) / double(total_) * W * H);
    const auto rects = Squarify(areas, RectD{double(area.left), double(area.top), W, H});
    for (size_t i = 0; i < rects.size(); ++i) {
        const RectD& r = rects[i];
        RECT b{static_cast<LONG>(r.x + 0.5), static_cast<LONG>(r.y + 0.5), static_cast<LONG>(r.x + r.w + 0.5),
               static_cast<LONG>(r.y + r.h + 0.5)};
        if (b.right - b.left >= 2 && b.bottom - b.top >= 2) blocks_.push_back({b, i});
    }

    // Shapes first (GDI+), then labels (GDI ClearType text).
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float gap = ScaleF(1.5f);
        for (size_t k = 0; k < blocks_.size(); ++k) {
            const Block& blk = blocks_[k];
            const Item& it = items_[blk.item];
            COLORREF fill = it.tag == kOtherTag ? g_pal.surface2 : BlockColor(blk.item);
            if (static_cast<int>(k) == hover_) fill = Blend(fill, RGB(255, 255, 255), 0.15);
            const Gdiplus::RectF rf(float(blk.rc.left) + gap, float(blk.rc.top) + gap,
                                    float(blk.rc.right - blk.rc.left) - 2 * gap, float(blk.rc.bottom - blk.rc.top) - 2 * gap);
            if (rf.Width <= 0 || rf.Height <= 0) continue;
            FillRoundRect(g, fill, rf, ScaleF(5));
            if (it.tag == selected_ && it.tag != kOtherTag) StrokeRoundRect(g, g_pal.accent, rf, ScaleF(5), ScaleF(2));
        }
    }
    for (size_t k = 0; k < blocks_.size(); ++k) {
        const Block& blk = blocks_[k];
        const Item& it = items_[blk.item];
        const int w = blk.rc.right - blk.rc.left, h = blk.rc.bottom - blk.rc.top;
        if (w < Scale(48) || h < Scale(22)) continue;  // too small to label legibly
        const COLORREF fill = it.tag == kOtherTag ? g_pal.surface2 : BlockColor(blk.item);
        SetTextColor(dc, it.tag == kOtherTag ? g_pal.textDim : TextOn(fill));
        RECT text{blk.rc.left + Scale(8), blk.rc.top + Scale(6), blk.rc.right - Scale(8), blk.rc.bottom - Scale(4)};
        SelectObject(dc, UiFontBold());
        RECT name = text;
        name.bottom = name.top + Scale(20);
        DrawTextW(dc, it.label.c_str(), -1, &name, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        if (h >= Scale(44)) {
            SelectObject(dc, UiFont());
            RECT size = text;
            size.top = name.bottom;
            const std::wstring s = Format(L"%s  ·  %.1f%%", FormatBytes(it.size).c_str(), 100.0 * double(it.size) / double(total_));
            DrawTextW(dc, s.c_str(), -1, &size, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
    }
    SelectObject(dc, old);
}

void Treemap::OnMouse(UINT msg, int x, int y) {
    const int hit = msg == WM_MOUSELEAVE ? -1 : HitTest(x, y);
    if (msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) {
        if (hit != hover_) {
            hover_ = hit;
            SetCursor(LoadCursorW(nullptr, hit >= 0 ? IDC_HAND : IDC_ARROW));
            Invalidate();
        }
        return;
    }
    if (hit < 0) return;
    const Item& it = items_[blocks_[static_cast<size_t>(hit)].item];
    if (it.tag == kOtherTag) return;
    if (msg == WM_LBUTTONDOWN) {
        SetSelected(it.tag);
        if (onPick_) onPick_(it.tag, false);
    } else if (msg == WM_LBUTTONDBLCLK) {
        if (onPick_) onPick_(it.tag, true);
    }
}

}  // namespace mtm

namespace mtm {

// ---------------------------------------------------------------- TextCard

void TextCard::Create(std::wstring title) {
    title_ = std::move(title);
    CreateWidget();
}

void TextCard::SetSections(std::vector<TipSection> sections, std::wstring subtitle) {
    sections_ = std::move(sections);
    subtitle_ = std::move(subtitle);
    Invalidate();
}

void TextCard::ScrollTo(int offset) {
    const int maxScroll = std::max(0, content_ - view_);
    offset = std::clamp(offset, 0, maxScroll);
    if (offset == scroll_) return;
    scroll_ = offset;
    Invalidate();
}

bool TextCard::OnWheel(int delta) {
    if (content_ <= view_) return false;  // nothing to scroll: let the page have it
    ScrollTo(scroll_ - delta * Scale(48) / WHEEL_DELTA);
    return true;
}

void TextCard::OnMouse(UINT msg, int x, int y) {
    const bool overBar = content_ > view_ && x >= track_.left - Scale(6) && x < track_.right + Scale(4) &&
                         y >= track_.top && y < track_.bottom;
    if (msg == WM_LBUTTONDOWN && overBar) {
        if (y < thumb_.top || y >= thumb_.bottom) {  // click on the track: jump there
            const int trackH = track_.bottom - track_.top, thumbH = thumb_.bottom - thumb_.top;
            ScrollTo(static_cast<int>(double(y - track_.top - thumbH / 2) / std::max(1, trackH - thumbH) *
                                      (content_ - view_)));
        }
        dragFrom_ = y;
        dragScroll_ = scroll_;
        SetCapture(hwnd_);
    } else if (msg == WM_LBUTTONUP) {
        if (dragFrom_ >= 0) ReleaseCapture();
        dragFrom_ = -1;
    } else if (msg == WM_MOUSEMOVE && dragFrom_ >= 0) {
        const int trackH = track_.bottom - track_.top, thumbH = thumb_.bottom - thumb_.top;
        ScrollTo(dragScroll_ + static_cast<int>(double(y - dragFrom_) / std::max(1, trackH - thumbH) * (content_ - view_)));
    }
    if ((msg == WM_MOUSEMOVE || msg == WM_MOUSELEAVE) && (overBar || dragFrom_ >= 0) != hoverBar_) {
        hoverBar_ = overBar || dragFrom_ >= 0;
        Invalidate();
    }
}

void TextCard::Paint(HDC dc, const RECT& rc) {
    const RECT in = PaintCard(dc, rc);
    SetBkMode(dc, TRANSPARENT);
    RECT title{in.left, in.top, in.right, in.top + Scale(18)};
    HGDIOBJ old = SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textDim);
    DrawCapsText(dc, title_, title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, UiFont());
    SetTextColor(dc, g_pal.textFaint);
    DrawTextW(dc, subtitle_.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    const RECT body{in.left, title.bottom + Scale(12), in.right, in.bottom};
    view_ = std::max<int>(0, body.bottom - body.top);
    const int barW = Scale(4);
    // Measure at the narrower width first; if it all fits, use the full width (no scrollbar).
    auto layout = [&](int right, bool paint) {
        int y = body.top - scroll_;
        for (const TipSection& s : sections_) {
            if (!s.heading.empty()) {
                RECT h{in.left, y, right, y};
                SelectObject(dc, UiFontBold());
                DrawTextW(dc, s.heading.c_str(), -1, &h, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
                h.right = right;
                if (paint) {
                    SetTextColor(dc, g_pal.text);
                    DrawTextW(dc, s.heading.c_str(), -1, &h, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
                }
                y = h.bottom + Scale(3);
            }
            SelectObject(dc, UiFont());
            RECT t{in.left, y, right, y};
            DrawTextW(dc, s.text.c_str(), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            t.right = right;
            if (paint) {
                SetTextColor(dc, g_pal.textDim);
                DrawTextW(dc, s.text.c_str(), -1, &t, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            }
            y = t.bottom + Scale(10);
        }
        return y - Scale(10) - (body.top - scroll_);
    };
    content_ = layout(in.right, false);
    const bool scrolls = content_ > view_;
    const int textRight = scrolls ? in.right - barW - Scale(10) : in.right;
    if (scrolls) content_ = layout(textRight, false);
    scroll_ = std::clamp(scroll_, 0, std::max(0, content_ - view_));

    const int saved = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top, body.right, body.bottom);
    layout(textRight, true);
    RestoreDC(dc, saved);
    SelectObject(dc, old);

    track_ = thumb_ = RECT{};
    if (!scrolls) return;
    track_ = RECT{in.right - barW, body.top, in.right, body.bottom};
    const int trackH = track_.bottom - track_.top;
    const int thumbH = std::max(Scale(24), trackH * view_ / std::max(1, content_));
    const int thumbY = track_.top + (trackH - thumbH) * scroll_ / std::max(1, content_ - view_);
    thumb_ = RECT{track_.left, thumbY, track_.right, thumbY + thumbH};
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float r = float(barW) / 2;
    FillRoundRect(g, g_pal.surface2, Gdiplus::RectF(float(track_.left), float(track_.top), float(barW), float(trackH)), r);
    FillRoundRect(g, hoverBar_ ? g_pal.textDim : g_pal.textFaint,
                  Gdiplus::RectF(float(thumb_.left), float(thumb_.top), float(barW), float(thumbH)), r);
    // Soft fade at the bottom edge hints that there is more below.
    if (scroll_ < content_ - view_) {
        const float fadeH = ScaleF(22);
        Gdiplus::LinearGradientBrush fade(Gdiplus::PointF(0, float(body.bottom) - fadeH), Gdiplus::PointF(0, float(body.bottom) + 1),
                                          ToColor(g_pal.surface, 0), ToColor(g_pal.surface, 255));
        g.FillRectangle(&fade, float(body.left), float(body.bottom) - fadeH, float(textRight - body.left), fadeH);
    }
}

}  // namespace mtm

namespace mtm {

bool OptionGrid::OnWheel(int delta) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const int maxScroll = std::max(0, HeightFor(rc.right - rc.left - Scale(12)) - int(rc.bottom - rc.top));
    if (maxScroll == 0) return false;
    const int next = std::clamp(scroll_ - delta * Scale(60) / WHEEL_DELTA, 0, maxScroll);
    if (next != scroll_) {
        scroll_ = next;
        hover_ = -1;  // tiles moved under the cursor
        HideRichTip();
        Invalidate();
    }
    return true;
}

}  // namespace mtm

namespace mtm {

// ---------------------------------------------------------------- InfoBadge

void InfoBadge::Create(std::wstring title, std::vector<TipSection> tip) {
    title_ = std::move(title);
    tip_ = std::move(tip);
    CreateWidget();
}

void InfoBadge::OnMouse(UINT msg, int x, int) {
    if (msg != WM_MOUSEMOVE && msg != WM_MOUSELEAVE) return;
    const bool hover = x >= 0;
    if (hover == hover_) return;
    hover_ = hover;
    Invalidate();
    if (!hover) {
        HideRichTip();
        return;
    }
    RECT r;
    GetWindowRect(hwnd_, &r);
    ShowRichTip(r, title_, tip_);
}

void InfoBadge::Paint(HDC dc, const RECT& rc) {
    FillRect(dc, &rc, BrushFor(g_pal.bg));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, IconFont());
    SetTextColor(dc, hover_ ? g_pal.text : g_pal.accent);
    const wchar_t glyph[2] = {0xE946, 0};  // Info
    RECT r = rc;
    DrawTextW(dc, glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

}  // namespace mtm
