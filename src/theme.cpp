#include "theme.h"

#include "gdip.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <uxtheme.h>

#include <unordered_map>

namespace mtm {

Palette g_pal{};

namespace {

ThemeMode g_mode = ThemeMode::System;
int g_accent = 0;
bool g_dark = true;
std::unordered_map<COLORREF, HBRUSH> g_brushes;

struct Accent {
    const wchar_t* name;
    COLORREF dark;   // used on dark backgrounds
    COLORREF light;  // deeper variant for light backgrounds
};
const Accent kAccents[] = {
    {L"Cyan", RGB(0, 212, 255), RGB(0, 128, 178)},
    {L"Violet", RGB(167, 139, 250), RGB(109, 72, 220)},
    {L"Lime", RGB(163, 230, 53), RGB(72, 132, 10)},
    {L"Amber", RGB(251, 191, 36), RGB(176, 112, 0)},
    {L"Rose", RGB(251, 113, 133), RGB(208, 36, 78)},
};

bool SystemPrefersDark() {
    DWORD v = 1, size = sizeof v;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v == 0;
}

// Undocumented but stable since Windows 10 1903: lets popup menus follow dark mode.
void SetMenusDark(bool dark) {
    static HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    auto flush = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
    if (setMode) setMode(dark ? 2 /*ForceDark*/ : 3 /*ForceLight*/);
    if (flush) flush();
}

// --- control subclasses -------------------------------------------------------------

constexpr wchar_t kPropPrimary[] = L"mtm.primary";
constexpr wchar_t kPropHover[] = L"mtm.hover";
constexpr wchar_t kPropOnCard[] = L"mtm.onCard";

// What shows around a control's rounded corners: the card it sits on, or the page.
COLORREF Backdrop(HWND h) { return GetPropW(h, kPropOnCard) ? g_pal.surface : g_pal.bg; }

LRESULT CALLBACK ButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kPropHover)) {
            SetPropW(h, kPropHover, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kPropHover);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_NCDESTROY:
        RemovePropW(h, kPropHover);
        RemovePropW(h, kPropPrimary);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

constexpr wchar_t kPropGlyph[] = L"mtm.glyph";  // leading icon of an edit (search, folder...)

// Edits are drawn as rounded "pills": the frame, the leading icon and the padding all
// live in the non-client area, so the EDIT control itself only handles the text.
// Single-line edits can't centre text vertically, so the extra height becomes padding.
int EditLeftInset(HWND h) { return GetPropW(h, kPropGlyph) ? Scale(36) : Scale(14); }

LRESULT CALLBACK EditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
            HDC dc = GetDC(h);
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            TEXTMETRICW tm{};
            GetTextMetricsW(dc, &tm);
            SelectObject(dc, old);
            ReleaseDC(h, dc);
            RECT& r = p->rgrc[0];
            const int height = r.bottom - r.top;
            const int pad = std::max(0, (height - static_cast<int>(tm.tmHeight)) / 2);
            r.top += pad;
            r.bottom -= pad;
            r.left += EditLeftInset(h);
            r.right -= Scale(12);
            if (r.right < r.left) r.right = r.left;
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        // The padding is part of the box: clicking anywhere in it focuses the edit.
        RECT rc;
        GetWindowRect(h, &rc);
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (PtInRect(&rc, pt)) return HTCLIENT;
        break;
    }
    case WM_NCPAINT: {
        HDC wdc = GetWindowDC(h);
        RECT rc, client;
        GetWindowRect(h, &rc);
        GetClientRect(h, &client);
        MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&client), 2);
        OffsetRect(&client, -rc.left, -rc.top);
        OffsetRect(&rc, -rc.left, -rc.top);
        ExcludeClipRect(wdc, client.left, client.top, client.right, client.bottom);
        const bool focused = GetFocus() == h;
        const bool hover = GetPropW(h, kPropHover) != nullptr;
        const bool disabled = !IsWindowEnabled(h);
        {
            FillRect(wdc, &rc, BrushFor(Backdrop(h)));
            Gdiplus::Graphics g(wdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const float inset = focused ? ScaleF(1.0f) : 0.5f;
            const Gdiplus::RectF box(inset, inset, float(rc.right) - 2 * inset, float(rc.bottom) - 2 * inset);
            const float radius = std::min(ScaleF(10), box.Height / 2);
            FillRoundRect(g, g_pal.surface2, box, radius);
            if (focused) StrokeRoundRect(g, g_pal.accent, box, radius, ScaleF(1.6f));
            else StrokeRoundRect(g, hover ? g_pal.textFaint : g_pal.border, box, radius, 1.0f);
        }
        if (const auto glyph = static_cast<wchar_t>(reinterpret_cast<UINT_PTR>(GetPropW(h, kPropGlyph)))) {
            SetBkMode(wdc, TRANSPARENT);
            HGDIOBJ old = SelectObject(wdc, IconFont());
            SetTextColor(wdc, disabled ? g_pal.textFaint : focused ? g_pal.accent : g_pal.textDim);
            RECT icon{Scale(4), 0, EditLeftInset(h) - Scale(2), rc.bottom};
            const wchar_t s[2] = {glyph, 0};
            DrawTextW(wdc, s, 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(wdc, old);
        }
        ReleaseDC(h, wdc);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kPropHover)) {
            SetPropW(h, kPropHover, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kPropHover);
        RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        return r;
    }
    case WM_NCDESTROY:
        RemovePropW(h, kPropGlyph);
        RemovePropW(h, kPropHover);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Dropdowns (CBS_DROPDOWNLIST) are painted entirely here: a rounded field with the
// selection and a chevron. The open list is owner-drawn (DrawThemedItem) with rounded
// window corners, so the whole control matches the buttons and search boxes.
LRESULT CALLBACK ComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);
        const bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
        const bool focused = GetFocus() == h;
        const bool hover = GetPropW(h, kPropHover) != nullptr;
        const bool disabled = !IsWindowEnabled(h);
        {
            FillRect(mem, &rc, BrushFor(Backdrop(h)));
            Gdiplus::Graphics g(mem);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            const Gdiplus::RectF box(0.5f, 0.5f, float(rc.right) - 1, float(rc.bottom) - 1);
            const float radius = std::min(ScaleF(10), box.Height / 2);
            const COLORREF fill =
                disabled ? g_pal.surface : (hover || dropped) ? Blend(g_pal.surface2, g_pal.hover, 0.7) : g_pal.surface2;
            FillRoundRect(g, fill, box, radius);
            if (dropped || focused) StrokeRoundRect(g, g_pal.accent, box, radius, ScaleF(1.4f));
            else StrokeRoundRect(g, hover ? g_pal.textFaint : g_pal.border, box, radius, 1.0f);
        }
        SetBkMode(mem, TRANSPARENT);
        const int sel = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
        std::wstring text;
        if (sel >= 0) {
            text.resize(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(h, CB_GETLBTEXTLEN, sel, 0))) + 1);
            SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data()));
            text.resize(wcslen(text.c_str()));
        }
        HGDIOBJ oldFont = SelectObject(mem, UiFont());
        SetTextColor(mem, disabled ? g_pal.textFaint : g_pal.text);
        RECT tr{Scale(12), 0, rc.right - Scale(34), rc.bottom};
        DrawTextW(mem, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        // Chevron, drawn from the icon font at about 70% size for a lighter look.
        LOGFONTW lf{};
        GetObjectW(IconFont(), sizeof lf, &lf);
        lf.lfHeight = lf.lfHeight * 7 / 10;
        HFONT chevronFont = CreateFontIndirectW(&lf);
        SelectObject(mem, chevronFont);
        SetTextColor(mem, disabled ? g_pal.textFaint : dropped ? g_pal.accent : g_pal.textDim);
        RECT chev{rc.right - Scale(34), 0, rc.right - Scale(8), rc.bottom};
        const wchar_t glyph[2] = {static_cast<wchar_t>(dropped ? 0xE70E : 0xE70D), 0};  // chevron up / down
        DrawTextW(mem, glyph, 1, &chev, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(mem, oldFont);
        DeleteObject(chevronFont);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kPropHover)) {
            SetPropW(h, kPropHover, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kPropHover);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case CB_SETCURSEL:
    case CB_SHOWDROPDOWN:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_CTLCOLORLISTBOX:  // the open list asks its combo for colours
        SetTextColor(reinterpret_cast<HDC>(wp), g_pal.text);
        SetBkColor(reinterpret_cast<HDC>(wp), g_pal.surface2);
        return reinterpret_cast<LRESULT>(BrushFor(g_pal.surface2));
    case WM_NCDESTROY:
        RemovePropW(h, kPropHover);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Rounded corners and a themed scrollbar for a dropdown's list window.
void StyleComboList(HWND combo) {
    COMBOBOXINFO cbi{sizeof cbi};
    if (!GetComboBoxInfo(combo, &cbi) || !cbi.hwndList) return;
    const DWORD round = 3;  // DWMWCP_ROUNDSMALL
    DwmSetWindowAttribute(cbi.hwndList, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &round, sizeof round);
    SetWindowTheme(cbi.hwndList, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
}

std::wstring ClassOf(HWND h) {
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, ARRAYSIZE(cls));
    return cls;
}

}  // namespace

void RebuildPalette() {
    BumpVisualGeneration();
    g_dark = g_mode == ThemeMode::Dark || (g_mode == ThemeMode::System && SystemPrefersDark());
    const Accent& a = kAccents[g_accent];
    Palette p{};
    if (g_dark) {
        p.bg = RGB(12, 14, 19);
        p.sidebar = RGB(16, 19, 26);
        p.surface = RGB(21, 25, 33);
        p.surface2 = RGB(30, 35, 46);
        p.hover = RGB(38, 44, 58);
        p.border = RGB(40, 46, 60);
        p.text = RGB(232, 237, 245);
        p.textDim = RGB(148, 158, 176);
        p.textFaint = RGB(92, 102, 120);
        p.accent = a.dark;
        p.onAccent = RGB(8, 12, 18);
        p.good = RGB(52, 211, 153);
        p.warn = RGB(251, 191, 36);
        p.bad = RGB(248, 113, 113);
        p.cpu = a.dark;
        p.gpu = RGB(236, 72, 153);
        p.mem = RGB(139, 92, 246);
        p.disk = RGB(16, 185, 129);
        p.net = RGB(245, 158, 11);
    } else {
        p.bg = RGB(244, 246, 249);
        p.sidebar = RGB(234, 238, 243);
        p.surface = RGB(255, 255, 255);
        p.surface2 = RGB(231, 235, 241);
        p.hover = RGB(220, 226, 234);
        p.border = RGB(212, 218, 227);
        p.text = RGB(17, 23, 32);
        p.textDim = RGB(82, 94, 110);
        p.textFaint = RGB(138, 148, 163);
        p.accent = a.light;
        p.onAccent = RGB(255, 255, 255);
        p.good = RGB(5, 150, 105);
        p.warn = RGB(180, 110, 0);
        p.bad = RGB(220, 38, 38);
        p.cpu = a.light;
        p.gpu = RGB(219, 39, 119);
        p.mem = RGB(124, 58, 237);
        p.disk = RGB(5, 150, 105);
        p.net = RGB(217, 119, 6);
    }
    g_pal = p;
}

void LoadThemeSettings() {
    g_mode = static_cast<ThemeMode>(std::min<DWORD>(ReadSetting(L"Theme", 0), 2));
    g_accent = static_cast<int>(std::min<DWORD>(ReadSetting(L"Accent", 0), ARRAYSIZE(kAccents) - 1));
    RebuildPalette();
}

bool IsDark() { return g_dark; }
ThemeMode GetThemeMode() { return g_mode; }

void SetThemeMode(ThemeMode mode) {
    g_mode = mode;
    WriteSetting(L"Theme", static_cast<DWORD>(mode));
    RebuildPalette();
}

int AccentIndex() { return g_accent; }
int AccentCount() { return ARRAYSIZE(kAccents); }
const wchar_t* AccentName(int index) { return kAccents[index].name; }

void SetAccentIndex(int index) {
    g_accent = std::clamp(index, 0, AccentCount() - 1);
    WriteSetting(L"Accent", static_cast<DWORD>(g_accent));
    RebuildPalette();
}

COLORREF Blend(COLORREF a, COLORREF b, double t) {
    auto mix = [t](int x, int y) { return static_cast<BYTE>(std::clamp(x + (y - x) * t, 0.0, 255.0)); };
    return RGB(mix(GetRValue(a), GetRValue(b)), mix(GetGValue(a), GetGValue(b)), mix(GetBValue(a), GetBValue(b)));
}

HBRUSH BrushFor(COLORREF c) {
    auto it = g_brushes.find(c);
    if (it != g_brushes.end()) return it->second;
    HBRUSH b = CreateSolidBrush(c);
    g_brushes[c] = b;
    return b;
}

void ThemeControl(HWND h) {
    const std::wstring cls = ClassOf(h);
    if (cls == WC_LISTVIEWW) {
        ListView_SetBkColor(h, g_pal.bg);
        ListView_SetTextBkColor(h, g_pal.bg);
        ListView_SetTextColor(h, g_pal.text);
        // Explorer gives dark scrollbars; Table paints over the column lines this theme draws.
        SetWindowTheme(h, g_dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        SetWindowTheme(ListView_GetHeader(h), g_dark ? L"DarkMode_ItemsView" : L"ItemsView", nullptr);
    } else if (cls == WC_COMBOBOXW || cls == WC_EDITW) {
        SetWindowTheme(h, g_dark ? L"DarkMode_CFD" : L"CFD", nullptr);
        if (cls == WC_COMBOBOXW) StyleComboList(h);
        else RedrawWindow(h, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);  // the pill frame uses palette colours
    }
    InvalidateRect(h, nullptr, TRUE);
}

void ApplyTheme(HWND top) {
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(top, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof dark);
    const COLORREF caption = g_pal.sidebar, captionText = g_pal.text;
    DwmSetWindowAttribute(top, 35 /*DWMWA_CAPTION_COLOR*/, &caption, sizeof caption);
    DwmSetWindowAttribute(top, 36 /*DWMWA_TEXT_COLOR*/, &captionText, sizeof captionText);
    SetMenusDark(g_dark);
    EnumChildWindows(
        top,
        [](HWND child, LPARAM) -> BOOL {
            ThemeControl(child);
            return TRUE;
        },
        0);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

HWND CreateButton(const wchar_t* text, bool primary) {
    HWND h = CreateChild(WC_BUTTONW, text, WS_TABSTOP | BS_OWNERDRAW);
    if (primary) SetPropW(h, kPropPrimary, reinterpret_cast<HANDLE>(1));
    SetWindowSubclass(h, ButtonProc, 1, 0);
    return h;
}

void SetButtonPrimary(HWND h, bool primary) {
    if (primary) SetPropW(h, kPropPrimary, reinterpret_cast<HANDLE>(1));
    else RemovePropW(h, kPropPrimary);
    InvalidateRect(h, nullptr, FALSE);
}

HWND CreateEdit(const wchar_t* text, const wchar_t* cue, wchar_t glyph) {
    // Frame, icon and padding are drawn in EditProc's non-client area, so no WS_BORDER.
    HWND h = CreateChild(WC_EDITW, text, WS_TABSTOP | ES_AUTOHSCROLL);
    if (glyph) SetPropW(h, kPropGlyph, reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(glyph)));
    if (cue) SendMessageW(h, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue));
    SendMessageW(h, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    SetWindowSubclass(h, EditProc, 1, 0);
    // Recompute the non-client area now that the subclass decides its size.
    SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    ThemeControl(h);
    return h;
}

HWND CreateCombo(std::initializer_list<const wchar_t*> items, int selected) {
    // Owner-drawn list items (DrawThemedItem); the closed field is painted by ComboProc.
    HWND h = CreateChild(WC_COMBOBOXW, L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS);
    SendMessageW(h, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), ComboFieldHeight() - Scale(6));  // closed field
    SendMessageW(h, CB_SETITEMHEIGHT, 0, Scale(32));                                              // list rows
    for (const wchar_t* s : items) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    SendMessageW(h, CB_SETCURSEL, selected, 0);
    SetWindowSubclass(h, ComboProc, 1, 0);
    StyleComboList(h);
    ThemeControl(h);
    return h;
}

int ComboFieldHeight() { return Scale(34); }

HWND CreateLabel(const wchar_t* text, DWORD extraStyle) {
    return CreateChild(L"STATIC", text, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS | extraStyle);
}

void MeasureThemedItem(MEASUREITEMSTRUCT* mis) {
    if (mis->CtlType != ODT_COMBOBOX) return;
    mis->itemHeight = mis->itemID == static_cast<UINT>(-1) ? ComboFieldHeight() - Scale(6) : Scale(32);
}

// Rows of an open dropdown list: padded text, the hovered row tinted with the accent,
// the current choice marked with a check.
bool DrawComboItem(const DRAWITEMSTRUCT* dis) {
    if (dis->itemID == static_cast<UINT>(-1) || (dis->itemState & ODS_COMBOBOXEDIT)) return true;  // ComboProc paints the field
    HDC dc = dis->hDC;
    const RECT rc = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const int current = static_cast<int>(SendMessageW(dis->hwndItem, CB_GETCURSEL, 0, 0));
    FillRect(dc, &rc, BrushFor(g_pal.surface2));
    if (selected) {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF r(float(rc.left) + ScaleF(4), float(rc.top) + ScaleF(2), float(rc.right - rc.left) - ScaleF(8),
                               float(rc.bottom - rc.top) - ScaleF(4));
        FillRoundRect(g, Blend(g_pal.surface2, g_pal.accent, IsDark() ? 0.22 : 0.16), r, ScaleF(6));
    }
    std::wstring text(static_cast<size_t>(std::max<LRESULT>(0, SendMessageW(dis->hwndItem, CB_GETLBTEXTLEN, dis->itemID, 0))) + 1,
                      L'\0');
    SendMessageW(dis->hwndItem, CB_GETLBTEXT, dis->itemID, reinterpret_cast<LPARAM>(text.data()));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, static_cast<int>(dis->itemID) == current ? UiFontBold() : UiFont());
    SetTextColor(dc, g_pal.text);
    RECT tr{rc.left + Scale(14), rc.top, rc.right - Scale(30), rc.bottom};
    DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (static_cast<int>(dis->itemID) == current) {
        SelectObject(dc, IconFont());
        SetTextColor(dc, g_pal.accent);
        RECT cr{rc.right - Scale(30), rc.top, rc.right - Scale(8), rc.bottom};
        const wchar_t check[2] = {0xE73E, 0};
        DrawTextW(dc, check, 1, &cr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, old);
    return true;
}

namespace {
constexpr wchar_t kPropSliderHot[] = L"mtm.sliderHot";

// Paints a whole trackbar: background, slim rounded track filled with the accent up to
// the knob, and the round knob. Drawn off-screen and copied in one go.
void PaintSlider(HWND h, HDC target) {
    RECT client, thumb{}, channel{};
    GetClientRect(h, &client);
    const int w = client.right, hgt = client.bottom;
    if (w <= 0 || hgt <= 0) return;
    SendMessageW(h, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
    SendMessageW(h, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, w, hgt);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    {
        const RECT all{0, 0, w, hgt};
        FillRect(mem, &all, BrushFor(Backdrop(h)));
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const bool enabled = IsWindowEnabled(h) != FALSE;
        const bool dragging = GetCapture() == h;
        const bool hot = dragging || GetPropW(h, kPropSliderHot) != nullptr;
        const float cy = float(hgt) / 2;
        const float cx = float(thumb.left + thumb.right) / 2;
        const float left = float(channel.left), right = float(channel.right);
        const float th = ScaleF(6);
        FillRoundRect(g, g_pal.surface2, Gdiplus::RectF(left, cy - th / 2, right - left, th), th / 2);
        const COLORREF color = enabled ? g_pal.accent : g_pal.textFaint;
        if (cx > left) FillRoundRect(g, color, Gdiplus::RectF(left, cy - th / 2, cx - left, th), th / 2);
        const float r = ScaleF(hot ? 9.0f : 8.0f);
        if (hot) {  // soft halo while hovered / dragged
            Gdiplus::SolidBrush halo(ToColor(g_pal.accent, dragging ? 70 : 45));
            const float hr = r + ScaleF(5);
            g.FillEllipse(&halo, cx - hr, cy - hr, 2 * hr, 2 * hr);
        }
        Gdiplus::SolidBrush knob(ToColor(color));
        g.FillEllipse(&knob, cx - r, cy - r, 2 * r, 2 * r);
        Gdiplus::SolidBrush core(ToColor(Backdrop(h)));
        const float ir = r * 0.4f;
        g.FillEllipse(&core, cx - ir, cy - ir, 2 * ir, 2 * ir);
    }
    BitBlt(target, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void RepaintSlider(HWND h) {
    HDC dc = GetDC(h);
    PaintSlider(h, dc);
    ReleaseDC(h, dc);
    ValidateRect(h, nullptr);
}

// The trackbar keeps its logic (range, dragging, keys, WM_HSCROLL), but all painting is
// ours: WM_PAINT is handled here, and after anything that can move the knob the whole
// control is redrawn at once. The stock control draws its knob directly while dragging,
// which is what left smeared copies behind.
LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        PAINTSTRUCT ps;
        HDC dc = msg == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
        PaintSlider(h, dc);
        if (msg == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        RECT thumb{};
        SendMessageW(h, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        InflateRect(&thumb, Scale(4), Scale(4));
        const bool hot = PtInRect(&thumb, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) != FALSE;
        if (hot) SetPropW(h, kPropSliderHot, reinterpret_cast<HANDLE>(1));
        else RemovePropW(h, kPropSliderHot);
        TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        RepaintSlider(h);
        return r;
    }
    case WM_MOUSELEAVE:
        RemovePropW(h, kPropSliderHot);
        RepaintSlider(h);
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case WM_MOUSEWHEEL:
    case WM_CAPTURECHANGED:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_SIZE:
    case TBM_SETPOS:
    case TBM_SETPOSNOTIFY:
    case TBM_SETRANGE:
    case TBM_SETRANGEMIN:
    case TBM_SETRANGEMAX: {
        const LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (IsWindowVisible(h)) RepaintSlider(h);
        return r;
    }
    case WM_NCDESTROY:
        RemovePropW(h, kPropSliderHot);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}
}  // namespace

void SetOnCard(HWND control) {
    SetPropW(control, kPropOnCard, reinterpret_cast<HANDLE>(1));  // page controls live as long as the app
    InvalidateRect(control, nullptr, FALSE);
}

void ThemeSlider(HWND h) {
    // A fixed thumb length matching the drawn knob, and no dotted focus rectangle.
    SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) | TBS_FIXEDLENGTH);
    SendMessageW(h, TBM_SETTHUMBLENGTH, Scale(20), 0);
    SendMessageW(h, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
    SetWindowSubclass(h, SliderProc, 1, 0);
}

// Sliders paint themselves in SliderProc; if the control still asks via custom draw,
// tell it to draw nothing.
bool CustomDrawSlider(NMHDR* nm, LRESULT& result) {
    if (nm->code != NM_CUSTOMDRAW || ClassOf(nm->hwndFrom) != TRACKBAR_CLASSW) return false;
    result = CDRF_SKIPDEFAULT;
    return true;
}

bool DrawThemedItem(const DRAWITEMSTRUCT* dis) {
    if (dis->CtlType == ODT_COMBOBOX) return DrawComboItem(dis);
    if (dis->CtlType != ODT_BUTTON) return false;
    HWND h = dis->hwndItem;
    HDC dc = dis->hDC;
    const RECT rc = dis->rcItem;
    const bool primary = GetPropW(h, kPropPrimary) != nullptr;
    const bool hover = GetPropW(h, kPropHover) != nullptr;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool focus = (dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT);

    COLORREF fill, border, text;
    if (disabled) {
        fill = g_pal.surface;
        border = g_pal.surface;
        text = g_pal.textFaint;
    } else if (primary) {
        fill = pressed ? Blend(g_pal.accent, RGB(0, 0, 0), 0.2) : hover ? Blend(g_pal.accent, RGB(255, 255, 255), 0.15) : g_pal.accent;
        border = fill;
        text = g_pal.onAccent;
    } else {
        fill = pressed ? Blend(g_pal.surface2, g_pal.text, 0.12) : hover ? g_pal.hover : g_pal.surface2;
        border = hover || focus ? Blend(g_pal.border, g_pal.accent, 0.6) : g_pal.border;
        text = g_pal.text;
    }

    FillRect(dc, &rc, BrushFor(Backdrop(h)));
    {
        Gdiplus::Graphics g(dc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF r(float(rc.left) + 0.5f, float(rc.top) + 0.5f, float(rc.right - rc.left) - 1.0f,
                               float(rc.bottom - rc.top) - 1.0f);
        FillRoundRect(g, fill, r, ScaleF(6));
        StrokeRoundRect(g, border, r, ScaleF(6));
    }

    std::wstring label = GetText(h);
    wchar_t icon = 0;
    if (!label.empty() && label[0] >= 0xE000 && label[0] <= 0xF8FF) {
        icon = label[0];
        label.erase(0, 1);
        while (!label.empty() && label[0] == L' ') label.erase(0, 1);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    SIZE iconSize{}, labelSize{};
    const int gap = icon && !label.empty() ? Scale(8) : 0;
    HGDIOBJ old = SelectObject(dc, IconFont());
    if (icon) GetTextExtentPoint32W(dc, &icon, 1, &iconSize);
    SelectObject(dc, UiFontBold());
    if (!label.empty()) GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &labelSize);
    int x = rc.left + (rc.right - rc.left - (iconSize.cx + gap + labelSize.cx)) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    if (icon) {
        SelectObject(dc, IconFont());
        TextOutW(dc, x, cy - iconSize.cy / 2, &icon, 1);
        x += iconSize.cx + gap;
    }
    SelectObject(dc, UiFontBold());
    TextOutW(dc, x, cy - labelSize.cy / 2, label.c_str(), static_cast<int>(label.size()));
    SelectObject(dc, old);
    return true;
}

LRESULT ThemedCtlColor(UINT msg, HDC dc, HWND) {
    switch (msg) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, g_pal.text);
        SetBkColor(dc, g_pal.surface2);
        return reinterpret_cast<LRESULT>(BrushFor(g_pal.surface2));
    case WM_CTLCOLORSTATIC:
        SetTextColor(dc, g_pal.textDim);
        SetBkColor(dc, g_pal.bg);
        return reinterpret_cast<LRESULT>(BrushFor(g_pal.bg));
    default:
        SetBkColor(dc, g_pal.bg);
        return reinterpret_cast<LRESULT>(BrushFor(g_pal.bg));
    }
}

}  // namespace mtm
