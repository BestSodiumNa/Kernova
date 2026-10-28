#include "gdip.h"
#include "graph.h"
#include "inputdev.h"
#include "page.h"
#include "theme.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>

// Mouse & keyboard tester. Reads Raw Input, so it works with any device and needs no
// vendor software. Input is only looked at while this page is open and Kernova is the
// foreground window, and nothing is stored: the counters live in memory until Reset.

namespace mtm {
namespace {

constexpr double kChatterMs = 30;  // a second press this soon after a release is a worn-switch suspect

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / freq;
}

// ---- keyboard layouts ---------------------------------------------------------------------

struct KeyDef {
    const wchar_t* label;
    int id;
    float x, y, w = 1, h = 1;
};

// Full-size ANSI layout in key units (1u = one letter key). The smaller ones are cut from it.
std::vector<KeyDef> FullLayout() {
    std::vector<KeyDef> k = {{L"Esc", 0x01, 0, 0}};
    const wchar_t* f[] = {L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};
    const int fsc[] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    for (int i = 0; i < 12; ++i) k.push_back({f[i], fsc[i], 2.0f + i + (i / 4) * 0.5f, 0});
    k.insert(k.end(), {{L"PrtSc", 0x137, 15.25f, 0}, {L"ScrLk", 0x46, 16.25f, 0}, {L"Pause", kPauseId, 17.25f, 0}});
    const wchar_t* num[] = {L"`", L"1", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"0", L"-", L"="};
    const int numSc[] = {0x29, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D};
    for (int i = 0; i < 13; ++i) k.push_back({num[i], numSc[i], float(i), 1.5f});
    k.push_back({L"Backspace", 0x0E, 13, 1.5f, 2});
    k.push_back({L"Tab", 0x0F, 0, 2.5f, 1.5f});
    const wchar_t* r1[] = {L"Q", L"W", L"E", L"R", L"T", L"Y", L"U", L"I", L"O", L"P", L"[", L"]"};
    for (int i = 0; i < 12; ++i) k.push_back({r1[i], 0x10 + i, 1.5f + i, 2.5f});
    k.push_back({L"\\", 0x2B, 13.5f, 2.5f, 1.5f});
    k.push_back({L"Caps", 0x3A, 0, 3.5f, 1.75f});
    const wchar_t* r2[] = {L"A", L"S", L"D", L"F", L"G", L"H", L"J", L"K", L"L", L";", L"'"};
    for (int i = 0; i < 11; ++i) k.push_back({r2[i], 0x1E + i, 1.75f + i, 3.5f});
    k.push_back({L"Enter", 0x1C, 12.75f, 3.5f, 2.25f});
    k.push_back({L"Shift", 0x2A, 0, 4.5f, 2.25f});
    const wchar_t* r3[] = {L"Z", L"X", L"C", L"V", L"B", L"N", L"M", L",", L".", L"/"};
    for (int i = 0; i < 10; ++i) k.push_back({r3[i], 0x2C + i, 2.25f + i, 4.5f});
    k.push_back({L"Shift", 0x36, 12.25f, 4.5f, 2.75f});
    k.insert(k.end(), {{L"Ctrl", 0x1D, 0, 5.5f, 1.25f},       {L"Win", 0x15B, 1.25f, 5.5f, 1.25f},
                       {L"Alt", 0x38, 2.5f, 5.5f, 1.25f},     {L"Space", 0x39, 3.75f, 5.5f, 6.25f},
                       {L"Alt", 0x138, 10, 5.5f, 1.25f},      {L"Win", 0x15C, 11.25f, 5.5f, 1.25f},
                       {L"Menu", 0x15D, 12.5f, 5.5f, 1.25f},  {L"Ctrl", 0x11D, 13.75f, 5.5f, 1.25f}});
    k.insert(k.end(), {{L"Ins", 0x152, 15.25f, 1.5f}, {L"Home", 0x147, 16.25f, 1.5f}, {L"PgUp", 0x149, 17.25f, 1.5f},
                       {L"Del", 0x153, 15.25f, 2.5f}, {L"End", 0x14F, 16.25f, 2.5f},  {L"PgDn", 0x151, 17.25f, 2.5f},
                       {L"↑", 0x148, 16.25f, 4.5f},   {L"←", 0x14B, 15.25f, 5.5f},    {L"↓", 0x150, 16.25f, 5.5f},
                       {L"→", 0x14D, 17.25f, 5.5f}});
    k.insert(k.end(), {{L"Num", kNumLockId, 18.5f, 1.5f}, {L"/", 0x135, 19.5f, 1.5f},  {L"*", 0x37, 20.5f, 1.5f},
                       {L"-", 0x4A, 21.5f, 1.5f},         {L"7", 0x47, 18.5f, 2.5f},   {L"8", 0x48, 19.5f, 2.5f},
                       {L"9", 0x49, 20.5f, 2.5f},         {L"+", 0x4E, 21.5f, 2.5f, 1, 2}, {L"4", 0x4B, 18.5f, 3.5f},
                       {L"5", 0x4C, 19.5f, 3.5f},         {L"6", 0x4D, 20.5f, 3.5f},   {L"1", 0x4F, 18.5f, 4.5f},
                       {L"2", 0x50, 19.5f, 4.5f},         {L"3", 0x51, 20.5f, 4.5f},   {L"Enter", 0x11C, 21.5f, 4.5f, 1, 2},
                       {L"0", 0x52, 18.5f, 5.5f, 2},      {L".", 0x53, 20.5f, 5.5f}});
    return k;
}

std::vector<KeyDef> LayoutFor(KeyboardForm form) {
    std::vector<KeyDef> full = FullLayout(), out;
    if (form == KeyboardForm::Full) return full;
    if (form == KeyboardForm::Tkl) {
        for (const KeyDef& k : full)
            if (k.x < 18) out.push_back(k);
        return out;
    }
    // 60 / 65 / 75: the main block without its function row, moved to the top.
    const float top = form == KeyboardForm::Compact75 ? 1.0f : 0.0f;
    for (KeyDef k : full)
        if (k.x < 15 && k.y >= 1.5f) {
            k.y = k.y - 1.5f + top;
            out.push_back(k);
        }
    if (form == KeyboardForm::Compact60) return out;
    // 65 and 75: arrows tucked under a shorter right Shift, plus a column of navigation keys.
    std::erase_if(out, [](const KeyDef& k) { return k.id == 0x15C || k.id == 0x15D; });  // right Win, Menu
    for (KeyDef& k : out) {
        if (k.id == 0x36) k.w = 1.75f;           // right Shift
        if (k.id == 0x138) k.w = 1, k.x = 10;   // right Alt
        if (k.id == 0x11D) k.w = 1, k.x = 11;   // right Ctrl
    }
    const float r = top;  // first main row
    out.insert(out.end(), {{L"↑", 0x148, 14, r + 3}, {L"←", 0x14B, 13, r + 4}, {L"↓", 0x150, 14, r + 4}, {L"→", 0x14D, 15, r + 4}});
    if (form == KeyboardForm::Compact65) {
        out.insert(out.end(), {{L"Del", 0x153, 15, r}, {L"PgUp", 0x149, 15, r + 1}, {L"PgDn", 0x151, 15, r + 2}, {L"End", 0x14F, 15, r + 3}});
        return out;
    }
    // 75: a compact function row on top, Delete at its end.
    out.push_back({L"Esc", 0x01, 0, 0});
    const wchar_t* f[] = {L"F1", L"F2", L"F3", L"F4", L"F5", L"F6", L"F7", L"F8", L"F9", L"F10", L"F11", L"F12"};
    const int fsc[] = {0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58};
    for (int i = 0; i < 12; ++i) out.push_back({f[i], fsc[i], 1.0f + i, 0});
    out.insert(out.end(), {{L"PrtSc", 0x137, 13, 0}, {L"Del", 0x153, 15, 0}, {L"Home", 0x147, 15, r},
                           {L"PgUp", 0x149, 15, r + 1}, {L"PgDn", 0x151, 15, r + 2}, {L"End", 0x14F, 15, r + 3}});
    return out;
}

const wchar_t* FormName(KeyboardForm f) {
    switch (f) {
        case KeyboardForm::Full: return L"Full size (100%)";
        case KeyboardForm::Tkl: return L"TKL (80%)";
        case KeyboardForm::Compact75: return L"75%";
        case KeyboardForm::Compact65: return L"65%";
        default: return L"60%";
    }
}

struct KeyState {
    bool down = false, tested = false, chatter = false;
    double lastUp = -1e9;
};

class KeyboardView final : public Widget {
public:
    void Create() { CreateWidget(); }
    const std::map<int, KeyState>* keys = nullptr;
    const std::map<int, float>* depths = nullptr;  // 0..1 per key id, for Hall-effect keyboards
    float travelMm = 4.0f;
    std::vector<KeyDef> layout;
    std::wstring title, hint;

private:
    void Paint(HDC dc, const RECT& rc) override {
        const RECT in = PaintCard(dc, rc);
        SetBkMode(dc, TRANSPARENT);
        RECT head{in.left, in.top, in.right, in.top + Scale(18)};
        HGDIOBJ old = SelectObject(dc, CapsFont());
        SetTextColor(dc, g_pal.textDim);
        DrawCapsText(dc, title, head, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, UiFont());
        SetTextColor(dc, g_pal.textFaint);
        DrawTextW(dc, hint.c_str(), -1, &head, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        if (layout.empty()) return;

        float cols = 0, rows = 0;
        for (const KeyDef& k : layout) cols = std::max(cols, k.x + k.w), rows = std::max(rows, k.y + k.h);
        const RECT area{in.left, head.bottom + Scale(12), in.right, in.bottom};
        const float unit = std::min(float(area.right - area.left) / cols, float(area.bottom - area.top) / rows);
        const float ox = area.left + (float(area.right - area.left) - unit * cols) / 2, oy = float(area.top);
        const float gap = std::max(2.0f, unit * 0.08f), radius = unit * 0.12f;
        LOGFONTW lf{};
        GetObjectW(UiFont(), sizeof lf, &lf);
        lf.lfHeight = -std::max(9, int(unit * 0.28f));
        HFONT keyFont = CreateFontIndirectW(&lf);
        lf.lfHeight = -std::max(8, int(unit * 0.22f));
        HFONT depthFont = CreateFontIndirectW(&lf);
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (const KeyDef& k : layout) {
            const KeyState* s = nullptr;
            if (keys)
                if (auto it = keys->find(k.id); it != keys->end()) s = &it->second;
            float depth = 0;
            if (depths)
                if (auto it = depths->find(k.id); it != depths->end()) depth = it->second;
            const Gdiplus::RectF r(ox + k.x * unit + gap / 2, oy + k.y * unit + gap / 2, k.w * unit - gap, k.h * unit - gap);
            COLORREF fill = g_pal.surface2, text = g_pal.textDim;
            if (s && s->down) fill = g_pal.accent, text = g_pal.onAccent;
            else if (s && s->tested) fill = Blend(g_pal.surface2, g_pal.accent, 0.35), text = g_pal.text;
            FillRoundRect(g, fill, r, radius);
            if (depth > 0 && !(s && s->down)) {  // how far the key is pressed, filling up from the bottom
                Gdiplus::GraphicsPath clip;
                RoundRectPath(clip, r, radius);
                g.SetClip(&clip);
                Gdiplus::SolidBrush bar(ToColor(g_pal.accent, 170));
                g.FillRectangle(&bar, r.X, r.Y + r.Height * (1 - depth), r.Width, r.Height * depth);
                g.ResetClip();
                text = g_pal.text;
            }
            StrokeRoundRect(g, s && s->chatter ? g_pal.bad : g_pal.border, r, radius, s && s->chatter ? 2.0f : 1.0f);
            g.Flush();
            RECT tr{int(r.X), int(r.Y), int(r.X + r.Width), int(r.Y + r.Height)};
            if (depth > 0) tr.bottom -= int(r.Height * 0.3f);
            SelectObject(dc, keyFont);
            SetTextColor(dc, text);
            DrawTextW(dc, k.label, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            if (depth > 0) {
                RECT dr{tr.left, tr.bottom - int(r.Height * 0.05f), tr.right, int(r.Y + r.Height)};
                SelectObject(dc, depthFont);
                const std::wstring mm = Format(L"%.1f", depth * travelMm);
                DrawTextW(dc, mm.c_str(), -1, &dr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        }
        SelectObject(dc, old);
        DeleteObject(keyFont);
        DeleteObject(depthFont);
    }
};

// Top-down mouse with its buttons lighting up while pressed.
class MouseView final : public Widget {
public:
    void Create() { CreateWidget(); }
    std::array<bool, 5> down{};
    int buttons = 5;          // side buttons are only drawn when the mouse reports them
    int wheel = 0;            // +1 / -1 for a moment after a wheel tick
    std::wstring name;

private:
    void Paint(HDC dc, const RECT& rc) override {
        const RECT in = PaintCard(dc, rc);
        SetBkMode(dc, TRANSPARENT);
        RECT head{in.left, in.top, in.right, in.top + Scale(18)};
        HGDIOBJ old = SelectObject(dc, CapsFont());
        SetTextColor(dc, g_pal.textDim);
        DrawCapsText(dc, L"Buttons", head, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, old);

        const float areaTop = float(head.bottom + Scale(10)), areaH = float(in.bottom) - areaTop - Scale(22);
        const float bodyH = areaH, bodyW = std::min(bodyH * 0.58f, float(in.right - in.left) * 0.55f);
        // Right of centre, leaving room on the left for the side-button labels.
        const float bx = in.left + float(in.right - in.left) * 0.64f - bodyW / 2, by = areaTop;
        const Gdiplus::RectF body(bx, by, bodyW, bodyH);
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::GraphicsPath shape;
        RoundRectPath(shape, body, bodyW * 0.48f);
        Gdiplus::SolidBrush bodyBrush(ToColor(g_pal.surface2));
        g.FillPath(&bodyBrush, &shape);

        // Left / right buttons: the top 42% of the body, split down the middle.
        g.SetClip(&shape);
        const float split = by + bodyH * 0.42f, mid = bx + bodyW / 2;
        Gdiplus::SolidBrush on(ToColor(g_pal.accent));
        if (down[0]) g.FillRectangle(&on, bx, by, bodyW / 2, split - by);
        if (down[1]) g.FillRectangle(&on, mid, by, bodyW / 2, split - by);
        g.ResetClip();
        Gdiplus::Pen line(ToColor(g_pal.border), 1.5f);
        g.DrawLine(&line, mid, by, mid, split);
        g.DrawLine(&line, bx + bodyW * 0.04f, split, bx + bodyW * 0.96f, split);
        g.DrawPath(&line, &shape);

        // Wheel, with an up / down arrow flashing on each tick.
        const Gdiplus::RectF wheelR(mid - bodyW * 0.08f, by + bodyH * 0.1f, bodyW * 0.16f, bodyH * 0.2f);
        FillRoundRect(g, down[2] ? g_pal.accent : Blend(g_pal.surface2, g_pal.text, 0.25), wheelR, wheelR.Width / 2);
        if (wheel) {
            Gdiplus::SolidBrush arrow(ToColor(g_pal.accent));
            const float ax = mid, ay = wheel > 0 ? wheelR.Y - Scale(6) : wheelR.Y + wheelR.Height + Scale(6), s = ScaleF(5);
            const Gdiplus::PointF tri[3] = {{ax - s, ay + (wheel > 0 ? s : -s)}, {ax + s, ay + (wheel > 0 ? s : -s)}, {ax, ay}};
            g.FillPolygon(&arrow, tri, 3);
        }

        // Side buttons on the left flank: forward (5) above back (4).
        auto side = [&](int index, float yFrac, const wchar_t* label) {
            const Gdiplus::RectF r(bx - bodyW * 0.1f, by + bodyH * yFrac, bodyW * 0.16f, bodyH * 0.11f);
            FillRoundRect(g, down[size_t(index)] ? g_pal.accent : Blend(g_pal.surface2, g_pal.text, 0.18), r, r.Width / 2.5f);
            StrokeRoundRect(g, g_pal.border, r, r.Width / 2.5f);
            g.Flush();
            RECT lr{in.left, int(r.Y), int(r.X) - Scale(6), int(r.Y + r.Height)};
            HGDIOBJ o = SelectObject(dc, UiFont());
            SetTextColor(dc, down[size_t(index)] ? g_pal.accent : g_pal.textFaint);
            DrawTextW(dc, label, -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, o);
        };
        if (buttons >= 5 || down[4]) side(4, 0.44f, L"Forward (M5)");
        if (buttons >= 4 || down[3]) side(3, 0.58f, L"Back (M4)");

        g.Flush();
        RECT foot{in.left, int(in.bottom) - Scale(18), in.right, in.bottom};
        HGDIOBJ o = SelectObject(dc, UiFont());
        SetTextColor(dc, g_pal.textFaint);
        const std::wstring text = name.empty() ? L"Click any mouse button" : Format(L"%d buttons reported", buttons);
        DrawTextW(dc, text.c_str(), -1, &foot, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, o);
    }
};

// Key travel gauges for Hall-effect keyboards: one column per pressed key, filling
// downwards as the key goes down, with its actuation point and deepest point marked.
class DepthView final : public Widget {
public:
    struct Bar {
        std::wstring label;
        float depth = 0, peak = 0, actuation = -1;  // 0..1; actuation -1 = not seen yet
        bool actuated = false;
    };
    void Create() { CreateWidget(); }
    std::vector<Bar> bars;
    float travelMm = 4.0f;
    std::wstring empty = L"Press a key";

private:
    void Paint(HDC dc, const RECT& rc) override {
        const RECT in = PaintCard(dc, rc);
        SetBkMode(dc, TRANSPARENT);
        RECT head{in.left, in.top, in.right, in.top + Scale(18)};
        HGDIOBJ old = SelectObject(dc, CapsFont());
        SetTextColor(dc, g_pal.textDim);
        DrawCapsText(dc, L"Key travel", head, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, UiFont());
        SetTextColor(dc, g_pal.textFaint);
        // Legend: what the two marks on a gauge mean.
        RECT legend = head;
        DrawTextW(dc, L"deepest", -1, &legend, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SIZE sz{};
        GetTextExtentPoint32W(dc, L"deepest", 7, &sz);
        const int midY = (head.top + head.bottom) / 2;
        int lx = legend.right - sz.cx - Scale(22);
        {
            Gdiplus::Graphics lg(dc);
            Gdiplus::Pen peakPen(ToColor(g_pal.text, 140), 1.5f);
            lg.DrawLine(&peakPen, float(lx), float(midY), float(lx + Scale(16)), float(midY));
            lx -= Scale(14);
            legend.right = lx;
            DrawTextW(dc, L"actuated", -1, &legend, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            GetTextExtentPoint32W(dc, L"actuated", 8, &sz);
            lx -= sz.cx + Scale(22);
            Gdiplus::Pen actPen(ToColor(g_pal.warn), 2.0f);
            actPen.SetDashStyle(Gdiplus::DashStyleDash);
            lg.DrawLine(&actPen, float(lx), float(midY), float(lx + Scale(16)), float(midY));
        }
        if (bars.empty()) {
            RECT body{in.left, head.bottom, in.right, in.bottom};
            DrawTextW(dc, empty.c_str(), -1, &body, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, old);
            return;
        }
        const int n = int(std::min<size_t>(bars.size(), 4));
        const float top = float(head.bottom + Scale(14)), bottom = float(in.bottom - Scale(44));
        const float slot = float(in.right - in.left) / n, trackW = std::min(slot * 0.42f, ScaleF(46));
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        for (int i = 0; i < n; ++i) {
            const Bar& b = bars[size_t(i)];
            const float cx = in.left + slot * (i + 0.5f), x = cx - trackW / 2, h = bottom - top;
            const Gdiplus::RectF track(x, top, trackW, h);
            FillRoundRect(g, g_pal.surface2, track, ScaleF(6));
            Gdiplus::GraphicsPath clip;
            RoundRectPath(clip, track, ScaleF(6));
            g.SetClip(&clip);
            Gdiplus::SolidBrush fill(ToColor(b.actuated ? g_pal.accent : Blend(g_pal.surface2, g_pal.accent, 0.55)));
            g.FillRectangle(&fill, x, top, trackW, h * b.depth);
            g.ResetClip();
            StrokeRoundRect(g, g_pal.border, track, ScaleF(6));
            Gdiplus::Pen peak(ToColor(g_pal.text, 140), 1.5f);
            if (b.peak > b.depth + 0.01f) g.DrawLine(&peak, x - ScaleF(4), top + h * b.peak, x + trackW + ScaleF(4), top + h * b.peak);
            if (b.actuation >= 0) {  // where this key's switch actuated
                Gdiplus::Pen act(ToColor(g_pal.warn), 2.0f);
                act.SetDashStyle(Gdiplus::DashStyleDash);
                const float ay = top + h * b.actuation;
                g.DrawLine(&act, x - ScaleF(6), ay, x + trackW + ScaleF(6), ay);
            }
            g.Flush();
            RECT label{int(cx - slot / 2), int(bottom) + Scale(6), int(cx + slot / 2), int(bottom) + Scale(24)};
            SelectObject(dc, UiFontBold());
            SetTextColor(dc, g_pal.text);
            const std::wstring mm = Format(L"%.2f mm", b.depth * travelMm);
            DrawTextW(dc, mm.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            OffsetRect(&label, 0, Scale(18));
            SelectObject(dc, UiFont());
            SetTextColor(dc, g_pal.textDim);
            const std::wstring sub = b.label + (b.actuation >= 0 ? Format(L" · act %.1f", b.actuation * travelMm) : L"");
            DrawTextW(dc, sub.c_str(), -1, &label, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        }
        SelectObject(dc, old);
    }
};

// Movement trace: the last few thousand reports as a line, newest brightest.
class TracePad final : public Widget {
public:
    void Create() { CreateWidget(); }
    void Add(int dx, int dy) {
        points_.push_back({dx, dy});
        if (points_.size() > 3000) points_.pop_front();
    }
    void Clear() { points_.clear(); }
    std::wstring hint;

private:
    void Paint(HDC dc, const RECT& rc) override {
        const RECT in = PaintCard(dc, rc);
        SetBkMode(dc, TRANSPARENT);
        HGDIOBJ old = SelectObject(dc, CapsFont());
        SetTextColor(dc, g_pal.textDim);
        RECT title{in.left, in.top, in.right, in.top + Scale(18)};
        DrawCapsText(dc, L"Movement", title, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, UiFont());
        SetTextColor(dc, g_pal.textFaint);
        DrawTextW(dc, hint.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        if (points_.size() < 2) return;
        // Rebuild the path backwards from the newest point, then scale it to fit.
        std::vector<Gdiplus::PointF> path(points_.size());
        float x = 0, y = 0, minX = 0, maxX = 0, minY = 0, maxY = 0;
        for (size_t i = points_.size(); i-- > 0;) {
            path[i] = {x, y};
            x -= float(points_[i].first);
            y -= float(points_[i].second);
            minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
        }
        const RECT area{in.left, in.top + Scale(26), in.right, in.bottom};
        const float w = float(area.right - area.left), h = float(area.bottom - area.top);
        const float scale = std::min(w / std::max(1.0f, maxX - minX), h / std::max(1.0f, maxY - minY)) * 0.9f;
        const float cx = area.left + w / 2 - (minX + maxX) / 2 * scale, cy = area.top + h / 2 - (minY + maxY) / 2 * scale;
        for (auto& p : path) p = {cx + p.X * scale, cy + p.Y * scale};
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const size_t n = path.size(), chunk = std::max<size_t>(2, n / 12);
        for (size_t start = 0; start + 1 < n; start += chunk - 1) {  // older segments fade out
            const size_t len = std::min(chunk, n - start);
            const BYTE alpha = BYTE(40 + 215 * (start + len) / n);
            Gdiplus::Pen pen(ToColor(g_pal.accent, alpha), ScaleF(1.6f));
            pen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawLines(&pen, path.data() + start, int(len));
        }
        Gdiplus::SolidBrush dot(ToColor(g_pal.accent));
        g.FillEllipse(&dot, path.back().X - ScaleF(4), path.back().Y - ScaleF(4), ScaleF(8), ScaleF(8));
    }
    std::deque<std::pair<int, int>> points_;
};

// ---- the page ---------------------------------------------------------------------------

class InputPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Input tester"; }

    ~InputPage() override {
        Listen(false);
        if (sink_) DestroyWindow(sink_);
    }

    void Create() override {
        status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        reset_ = CreateButton(L"\xE72C  Reset");
        layoutPick_ = CreateCombo({L"Layout: auto-detect", L"Full size (100%)", L"TKL (80%)", L"75%", L"65%", L"60%"},
                                  int(std::min<DWORD>(ReadSetting(L"TesterLayout", 0), 5)));
        rateCard_.Create(0xE962, L"Mouse polling rate", Graph::Tint::Network, nullptr);
        clickCard_.Create(0xE7C9, L"Clicks", Graph::Tint::Cpu, nullptr);
        chatterCard_.Create(0xE7BA, L"Chatter", Graph::Tint::Memory, nullptr);
        keysCard_.Create(0xE765, L"Keyboard", Graph::Tint::Gpu, nullptr);
        rateGraph_.Create(Graph::Tint::Network, 0);
        mouseView_.Create();
        trace_.Create();
        keyboard_.Create();
        keyboard_.keys = &keys_;
        depthView_.Create();

        // Message-only window that receives WM_INPUT and drives a ~30 Hz refresh.
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = SinkProc;
            wc.hInstance = g_inst;
            wc.lpszClassName = L"KernovaInputSink";
            RegisterClassW(&wc);
            registered = true;
        }
        sink_ = CreateWindowExW(0, L"KernovaInputSink", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_inst, this);
        Reset();
    }

    static int CardH() { return Scale(140); }
    static int MidH() { return Scale(220); }
    int MinHeight(int) const override { return Scale(42) + CardH() + Scale(12) + MidH() + Scale(12) + Scale(260); }

    void Layout(RECT a) override {
        const int gap = Scale(12), w = a.right - a.left, h = Scale(34);
        Place(status_, a.left + Scale(2), a.top, w - Scale(400), h);
        Place(layoutPick_, a.right - Scale(388), a.top, Scale(260), Scale(300));
        Place(reset_, a.right - Scale(120), a.top, Scale(120), h);
        int y = a.top + h + Scale(8);
        const int cw = (w - 3 * gap) / 4;
        int x = a.left;
        for (ActionCard* c : {&rateCard_, &clickCard_, &chatterCard_, &keysCard_}) {
            Place(c->Handle(), x, y, c == &keysCard_ ? a.right - x : cw, CardH());
            x += cw + gap;
        }
        y += CardH() + gap;
        const int graphW = (w - 2 * gap) * 2 / 5, mouseW = (w - 2 * gap) / 5;
        Place(rateGraph_.Handle(), a.left, y, graphW, MidH());
        Place(mouseView_.Handle(), a.left + graphW + gap, y, mouseW, MidH());
        Place(trace_.Handle(), a.left + graphW + mouseW + 2 * gap, y, w - graphW - mouseW - 2 * gap, MidH());
        y += MidH() + gap;
        // The rest of the page, within limits: the key map keeps its shape and centres itself.
        // Hall-effect keyboards get the travel gauges beside it.
        const int kbH = std::clamp(int(a.bottom) - y, Scale(260), Scale(400));
        const int depthW = analog_.Running() ? std::min(Scale(320), w / 4) : 0;
        Place(keyboard_.Handle(), a.left, y, w - (depthW ? depthW + gap : 0), kbH);
        Place(depthView_.Handle(), a.right - depthW, y, depthW, kbH);
        ShowWindow(depthView_.Handle(), visible_ && depthW ? SW_SHOW : SW_HIDE);
    }

    void Show(bool v) override {
        ShowAll({status_, reset_, layoutPick_, rateCard_.Handle(), clickCard_.Handle(), chatterCard_.Handle(),
                 keysCard_.Handle(), rateGraph_.Handle(), mouseView_.Handle(), trace_.Handle(), keyboard_.Handle()},
                v);
        visible_ = v;
        Listen(v);
        if (v) {
            if (!keyboardDevice_) PickKeyboard();
            analog_.Start();  // only Wooting keyboards for now; quietly nothing otherwise
            keyboard_.depths = analog_.Running() ? &depthById_ : nullptr;
            keyboard_.travelMm = depthView_.travelMm = analog_.travelMm;
            ApplyLayout();
            SendMessageW(g_main, WM_SIZE, 0, 0);  // room for the travel gauges when there are any
            SetFocus(g_main);  // keep focus off the buttons, so Space / Enter while testing don't press them
        } else {
            analog_.Stop();
            keyboard_.depths = nullptr;
            ShowWindow(depthView_.Handle(), SW_HIDE);
        }
    }

    bool OnCommand(HWND control, WORD code) override {
        if (control == layoutPick_ && code == CBN_SELCHANGE) {
            WriteSetting(L"TesterLayout", DWORD(SendMessageW(layoutPick_, CB_GETCURSEL, 0, 0)));
            ApplyLayout();
            SetFocus(g_main);
            return true;
        }
        if (control != reset_ || code != BN_CLICKED) return false;
        Reset();
        SetFocus(g_main);
        return true;
    }

private:
    struct ButtonStat {
        unsigned clicks = 0;
        double lastUp = -1e9;
    };

    static LRESULT CALLBACK SinkProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<InputPage*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self && msg == WM_INPUT) {
            self->OnInput(reinterpret_cast<HRAWINPUT>(lp));
            return DefWindowProcW(h, msg, wp, lp);  // lets Windows free the input
        }
        if (self && msg == WM_TIMER) {
            self->Update();
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    // Raw Input for all mice and keyboards, delivered to the sink window. INPUTSINK is needed
    // because a message-only window is never "foreground"; OnInput checks Kernova is.
    void Listen(bool on) {
        if (on == listening_ || !sink_) return;
        RAWINPUTDEVICE rid[2] = {{0x01, 0x02, DWORD(on ? RIDEV_INPUTSINK : RIDEV_REMOVE), on ? sink_ : nullptr},
                                 {0x01, 0x06, DWORD(on ? RIDEV_INPUTSINK : RIDEV_REMOVE), on ? sink_ : nullptr}};
        if (RegisterRawInputDevices(rid, 2, sizeof rid[0])) listening_ = on;
        if (on) SetTimer(sink_, 1, 33, nullptr);
        else KillTimer(sink_, 1);
    }

    // Before anything is typed: the connected keyboard whose name gives away its size
    // (e.g. "Wooting 80HE"), else the first one with a USB id.
    void PickKeyboard() {
        UINT count = 0;
        GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST));
        std::vector<RAWINPUTDEVICELIST> list(count);
        if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST)) == UINT(-1)) return;
        HANDLE fallback = nullptr;
        for (const RAWINPUTDEVICELIST& d : list) {
            if (d.dwType != RIM_TYPEKEYBOARD) continue;
            const InputDeviceInfo& info = DescribeInputDevice(d.hDevice);
            if (GuessKeyboardForm(info.name) != KeyboardForm::Full) {
                keyboardDevice_ = d.hDevice;
                return;
            }
            if (!fallback && info.vid) fallback = d.hDevice;
        }
        keyboardDevice_ = fallback;
    }

    void ApplyLayout() {
        const int pick = static_cast<int>(SendMessageW(layoutPick_, CB_GETCURSEL, 0, 0));
        const InputDeviceInfo* kb = keyboardDevice_ ? &DescribeInputDevice(keyboardDevice_) : nullptr;
        const KeyboardForm form = pick > 0 ? KeyboardForm(pick - 1) : kb ? GuessKeyboardForm(kb->name) : KeyboardForm::Full;
        keyboard_.layout = LayoutFor(form);
        keyboard_.title = (kb ? kb->name + L"  ·  " : std::wstring()) + FormName(form) + (pick == 0 ? L" (detected)" : L"");
        keyboard_.Invalidate();
    }

    void OnInput(HRAWINPUT handle) {
        if (GetForegroundWindow() != g_main) return;  // never look at input meant for other apps
        alignas(8) BYTE buf[sizeof(RAWINPUT) + 64];
        UINT size = sizeof buf;
        if (GetRawInputData(handle, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == UINT(-1)) return;
        const RAWINPUT& ri = *reinterpret_cast<const RAWINPUT*>(buf);
        const double now = NowMs();
        if (ri.header.dwType == RIM_TYPEMOUSE) OnMouse(ri.header.hDevice, ri.data.mouse, now);
        else if (ri.header.dwType == RIM_TYPEKEYBOARD) OnKey(ri.header.hDevice, ri.data.keyboard, now);
    }

    void OnMouse(HANDLE device, const RAWMOUSE& m, double now) {
        if (device != mouse_) {  // another mouse took over: its own rate from scratch
            mouse_ = device;
            reports_.clear();
            const InputDeviceInfo& info = DescribeInputDevice(device);
            mouseView_.name = info.name;
            mouseView_.buttons = info.buttons ? info.buttons : 5;
        }
        if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE) && (m.lLastX || m.lLastY)) {
            reports_.push_back(now);
            while (!reports_.empty() && reports_.front() < now - 1000) reports_.pop_front();
            trace_.Add(m.lLastX, m.lLastY);
        }
        static constexpr USHORT down[5] = {RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN, RI_MOUSE_BUTTON_3_DOWN,
                                           RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN};
        static constexpr USHORT up[5] = {RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP, RI_MOUSE_BUTTON_4_UP,
                                         RI_MOUSE_BUTTON_5_UP};
        static constexpr const wchar_t* names[5] = {L"Left button", L"Right button", L"Middle button", L"Back button", L"Forward button"};
        for (int b = 0; b < 5; ++b) {
            if (m.usButtonFlags & down[b]) {
                ButtonStat& s = buttons_[size_t(b)];
                ++s.clicks;
                if (now - s.lastUp < kChatterMs) Chatter(names[b], now - s.lastUp, true);
                mouseView_.down[size_t(b)] = true;
                mouseView_.Invalidate();
            }
            if (m.usButtonFlags & up[b]) {
                buttons_[size_t(b)].lastUp = now;
                mouseView_.down[size_t(b)] = false;
                mouseView_.Invalidate();
            }
        }
        if (m.usButtonFlags & RI_MOUSE_WHEEL) {
            const int dir = short(m.usButtonData) > 0 ? 1 : -1;
            wheel_ += dir;
            ++wheelTicks_;
            mouseView_.wheel = dir;
            wheelAt_ = now;
            mouseView_.Invalidate();
        }
    }

    void OnKey(HANDLE device, const RAWKEYBOARD& k, double now) {
        if (k.VKey == 0xFF) return;                                                       // part of an escape sequence
        if ((k.Flags & RI_KEY_E0) && (k.MakeCode == 0x2A || k.MakeCode == 0x36)) return;  // fake shifts (PrtSc, Num Lock)
        if (device != keyboardDevice_) {  // typing on another keyboard: follow it
            keyboardDevice_ = device;
            ApplyLayout();
        }
        int id = k.MakeCode | ((k.Flags & RI_KEY_E0) ? 0x100 : 0);
        if (k.VKey == VK_NUMLOCK) id = kNumLockId;
        if (k.VKey == VK_PAUSE || (k.Flags & RI_KEY_E1)) id = kPauseId;
        KeyState& s = keys_[id];
        if (k.Flags & RI_KEY_BREAK) {
            s.down = false;
            s.lastUp = now;
        } else if (!s.down) {  // a new press, not the key repeating while held
            s.down = s.tested = true;
            ++presses_;
            LearnAnalogCode(id);
            if (now - s.lastUp < kChatterMs) {
                s.chatter = true;
                Chatter(KeyLabel(id), now - s.lastUp, false);
            }
        }
        int held = 0;
        for (const auto& [_, st] : keys_) held += st.down;
        held_ = held;
        maxHeld_ = std::max(maxHeld_, held);
        dirtyKeys_ = true;
    }

    // Which analog code is which key isn't documented, so learn it: a key's analog entry shows
    // up as soon as it starts moving, and Windows reports the press when it actuates. The
    // deepest code not yet matched to a key at that moment is the key that just went down.
    void LearnAnalogCode(int id) {
        if (!analog_.Running() || std::any_of(codeKey_.begin(), codeKey_.end(), [&](const auto& m) { return m.second == id; }))
            return;
        // Prefer a code the keyboard itself marks as actuated (fast typing has several keys moving).
        const std::vector<AnalogKeyboard::Key> now = analog_.Pressed();  // deepest first
        for (bool needActuated : {true, false})
            for (const AnalogKeyboard::Key& k : now)
                if (!codeKey_.count(k.code) && (k.actuated || !needActuated)) {
                    codeKey_[k.code] = id;
                    return;
                }
    }

    static std::wstring ShortLabel(int id) {
        for (const KeyDef& k : FullLayout())
            if (k.id == id) return k.label;
        return Format(L"0x%X", id);
    }
    static std::wstring KeyLabel(int id) { return ShortLabel(id) + L" key"; }

    // Depth of every moving key: for the key map (by key id) and the travel gauges.
    void UpdateAnalog() {
        if (!analog_.Running()) return;
        const std::vector<AnalogKeyboard::Key> now = analog_.Pressed();
        depthById_.clear();
        std::vector<DepthView::Bar> bars;
        for (const AnalogKeyboard::Key& k : now) {
            float& peak = peak_[k.code];
            peak = std::max(peak, k.depth);
            const auto key = codeKey_.find(k.code);
            if (key != codeKey_.end()) depthById_[key->second] = k.depth;
            bars.push_back({key != codeKey_.end() ? ShortLabel(key->second) : L"Key", k.depth, peak, k.actuationAt, k.actuated});
        }
        // Released keys start their next press with a fresh peak.
        for (auto it = peak_.begin(); it != peak_.end();)
            it = std::any_of(now.begin(), now.end(), [&](const auto& k) { return k.code == it->first; }) ? std::next(it) : peak_.erase(it);
        if (!bars.empty() || !depthView_.bars.empty()) {
            depthView_.bars = std::move(bars);
            depthView_.Invalidate();
            dirtyKeys_ = true;
        }
        depthView_.empty = L"Press a key on the " + analog_.name;
    }

    void Chatter(std::wstring what, double ms, bool mouse) {
        (mouse ? mouseChatter_ : keyChatter_)++;
        lastChatter_ = what + Format(L" pressed again %.0f ms after release", ms);
    }

    // Live polling rate: reports per second over the last 200 ms, only while the mouse moved
    // the whole time (a pause would read as a low rate).
    double LiveRate() const {
        const double now = NowMs();
        if (reports_.size() < 3 || reports_.back() < now - 30) return 0;
        size_t first = reports_.size() - 1;
        while (first > 0 && reports_[first - 1] >= now - 200) --first;
        const double span = reports_.back() - reports_[first];
        if (reports_[first] > now - 170 || span <= 0) return 0;
        return double(reports_.size() - 1 - first) * 1000.0 / span;
    }

    static std::wstring UsbRate(int hz) { return hz ? Format(L"USB %d Hz", hz) : L""; }

    void Update() {
        const double now = NowMs();
        const double rate = LiveRate();
        if (rate > 0) lastRate_ = rate;
        peakRate_ = std::max(peakRate_, rate);
        UpdateAnalog();
        if (++ticks_ % 3 == 0) {  // the graph at 10 Hz
            rateGraph_.Push(rate);
            rateGraph_.SetText(L"Mouse polling rate over time", rate > 0 ? Format(L"%.0f Hz", rate) : L"—",
                               L"Keep the mouse moving in circles; the line shows the rate it actually reports at");
        }
        const InputDeviceInfo* mouse = mouse_ ? &DescribeInputDevice(mouse_) : nullptr;
        std::wstring mouseLine = mouse ? mouse->name : L"No mouse input yet";
        if (mouse && mouse->usbHz) mouseLine += L" · " + UsbRate(mouse->usbHz) + L" link";
        rateCard_.SetText(rate > 0 ? Format(L"%.0f Hz", rate) : lastRate_ > 0 ? Format(L"%.0f Hz", lastRate_) : L"Move it",
                          mouseLine,
                          peakRate_ > 0 ? Format(L"Peak %.0f Hz · %s", peakRate_, RateClass(peakRate_)) : L"Move the mouse to measure");
        unsigned total = 0;
        for (const ButtonStat& b : buttons_) total += b.clicks;
        clickCard_.SetText(FormatCount(total), Format(L"Wheel: %d ticks (%+d net)", wheelTicks_, wheel_),
                           Format(L"L %u · R %u · M %u · M4 %u · M5 %u", buttons_[0].clicks, buttons_[1].clicks,
                                  buttons_[2].clicks, buttons_[3].clicks, buttons_[4].clicks));
        const int chatter = mouseChatter_ + keyChatter_;
        chatterCard_.SetText(chatter ? Format(L"%d suspect%s", chatter, chatter == 1 ? L"" : L"s") : L"None",
                             lastChatter_.empty() ? L"Double-presses faster than a finger can do show here" : lastChatter_,
                             Format(L"Mouse %d · Keyboard %d · under %.0f ms", mouseChatter_, keyChatter_, kChatterMs));

        // Keyboard: rollover, tested keys and its report rate.
        const InputDeviceInfo* kb = keyboardDevice_ ? &DescribeInputDevice(keyboardDevice_) : nullptr;
        const double analogRate = analog_.Running() ? analog_.ReportRate() : 0;
        if (analogRate > 0) lastAnalogRate_ = analogRate;
        std::wstring rateLine = kb && kb->usbHz ? UsbRate(kb->usbHz) : L"";
        if (analog_.Running())
            rateLine += (rateLine.empty() ? L"" : L" · ") +
                        (lastAnalogRate_ > 0 ? Format(L"depth updates %.0f/s", lastAnalogRate_) : std::wstring(L"press a key"));
        size_t tested = 0;
        std::wstring outside;
        for (const auto& [id, st] : keys_) {
            if (!st.tested) continue;
            const bool inLayout = std::any_of(keyboard_.layout.begin(), keyboard_.layout.end(), [&](const KeyDef& k) { return k.id == id; });
            if (inLayout) ++tested;
            else if (outside.size() < 40) outside += (outside.empty() ? L"" : L", ") + KeyLabel(id);
        }
        keysCard_.SetText(Format(L"%d at once", maxHeld_), rateLine.empty() ? (kb ? kb->name : L"Press any key") : rateLine,
                          Format(L"Holding %d · %zu of %zu keys tested", held_, tested, keyboard_.layout.size()));
        keyboard_.hint = analog_.Running()
                             ? L"Hall-effect: keys fill up as you press them (mm); each key is matched on its first press"
                             : outside.empty() ? L"" : L"Also pressed: " + outside;

        if (mouseView_.wheel && now - wheelAt_ > 150) {
            mouseView_.wheel = 0;
            mouseView_.Invalidate();
        }
        trace_.hint = reports_.empty() ? L"" : Format(L"%zu reports in the last second", reports_.size());
        trace_.Invalidate();
        if (dirtyKeys_) keyboard_.Invalidate();
        dirtyKeys_ = false;
        SetText(status_, GetForegroundWindow() == g_main
                             ? L"Move the mouse, click every button and press keys. Hold several keys together to test rollover."
                             : L"Paused: Kernova only reads input while it's the active window.");
    }

    static const wchar_t* RateClass(double hz) {
        static const std::array<std::pair<double, const wchar_t*>, 6> classes = {
            {{125, L"125 Hz class"}, {250, L"250 Hz class"}, {500, L"500 Hz class"}, {1000, L"1000 Hz class"},
             {2000, L"2000 Hz class"}, {4000, L"4000 Hz class"}}};
        if (hz >= 6000) return L"8000 Hz class";
        const wchar_t* best = classes[0].second;
        for (const auto& [limit, name] : classes)
            if (hz >= limit * 0.8) best = name;
        return best;
    }

    void Reset() {
        keys_.clear();
        buttons_ = {};
        reports_.clear();
        trace_.Clear();
        peakRate_ = lastRate_ = lastAnalogRate_ = 0;
        wheel_ = wheelTicks_ = 0;
        mouseChatter_ = keyChatter_ = 0;
        held_ = maxHeld_ = 0;
        presses_ = 0;
        lastChatter_.clear();
        mouseView_.down = {};
        mouseView_.Invalidate();
        dirtyKeys_ = true;
        Update();
    }

    HWND status_ = nullptr, reset_ = nullptr, layoutPick_ = nullptr, sink_ = nullptr;
    ActionCard rateCard_, clickCard_, chatterCard_, keysCard_;
    Graph rateGraph_;
    MouseView mouseView_;
    TracePad trace_;
    KeyboardView keyboard_;
    DepthView depthView_;
    AnalogKeyboard analog_;
    std::map<uint16_t, int> codeKey_;                 // learned: analog key code -> key id
    std::map<uint16_t, float> peak_;                  // per analog code: deepest point of this press
    std::map<int, float> depthById_;                  // current depth per key id, for the key map
    bool listening_ = false, dirtyKeys_ = false, visible_ = false;
    unsigned ticks_ = 0;

    HANDLE mouse_ = nullptr, keyboardDevice_ = nullptr;
    std::wstring lastChatter_;
    std::deque<double> reports_;  // arrival times (ms) of movement reports in the last second
    double peakRate_ = 0, lastRate_ = 0, lastAnalogRate_ = 0, wheelAt_ = 0;
    std::array<ButtonStat, 5> buttons_{};
    int wheel_ = 0, wheelTicks_ = 0, mouseChatter_ = 0, keyChatter_ = 0, held_ = 0, maxHeld_ = 0;
    unsigned presses_ = 0;
    std::map<int, KeyState> keys_;
};

}  // namespace

std::unique_ptr<Page> MakeInputPage() { return std::make_unique<InputPage>(); }

}  // namespace mtm
