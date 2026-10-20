#include "common.h"
#include "gdip.h"
#include "graph.h"
#include "defrag.h"
#include "icons.h"
#include "latency.h"
#include "net.h"
#include "page.h"
#include "power.h"
#include "processes.h"
#include "recorder.h"
#include "sysinfo.h"
#include "theme.h"
#include "tweaks.h"

#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace mtm {
namespace {

constexpr UINT_PTR kTimerId = 1;
constexpr UINT_PTR kAnimTimerId = 2;   // repaints animating graphs between samples

constexpr wchar_t kWindowClass[] = L"KernovaWindow";

enum : WORD {
    ID_FILE_ADMIN = 40010,
    ID_FILE_EXIT,
    ID_VIEW_REFRESH,
    ID_VIEW_TOPMOST,
    ID_SPEED_FAST,
    ID_SPEED_NORMAL,
    ID_SPEED_SLOW,
    ID_NEXT_TAB,
    ID_PREV_TAB,
    ID_THEME_SYSTEM,
    ID_THEME_DARK,
    ID_THEME_LIGHT,
    ID_ANIM_OFF,
    ID_ANIM_30,
    ID_ANIM_60,
    ID_ACCENT_FIRST = 40100,  // + accent index
};

// Hit-test ids for the sidebar.
constexpr int kNavNone = -1;
constexpr int kNavTheme = 1000;
constexpr int kNavSettings = 1001;

std::vector<std::unique_ptr<Page>> g_pages;
// Segoe Fluent Icons / MDL2 glyph per page, in page order.
// Index i must match g_pages[i] (registration order in OnCreate).
const wchar_t kPageIcons[] = {
    0xE80F,  // Summary (Home)
    0xE9D9,  // Performance
    0xE71D,  // Processes
    0xE912,  // Services
    0xE7E8,  // Startup apps
    0xE7B8,  // Installed apps
    0xE716,  // Users
    0xE945,  // Power & frequency
    0xE7FC,  // Game tweaks
    0xE916,  // Latency test (Stopwatch)
    0xEDA2,  // Disk space
    0xE95E,  // Drive Health (Health)
    0xE774,  // Connections
    0xE7C8,  // Flight Recorder (Record)
    0xEC4A,  // Benchmarks
    0xE962,  // Input tester (Mouse)
    0xE713,  // Mouse settings (Settings)
};
int g_current = -1;
int g_hover = kNavNone;
UINT g_interval = 1000;
bool g_topmost = false;

// ---------------------------------------------------------------- geometry

int SidebarWidth() { return Scale(224); }
int FooterHeight() { return Scale(34); }
int NavItemHeight() { return Scale(40); }
int NavTop() { return Scale(92); }

RECT NavItemRect(int id) {
    RECT client;
    GetClientRect(g_main, &client);
    const int x = Scale(12), w = SidebarWidth() - Scale(24), full = NavItemHeight();
    int y, h = full;
    if (id == kNavTheme) {
        y = client.bottom - Scale(16) - 2 * full;
    } else if (id == kNavSettings) {
        y = client.bottom - Scale(16) - full;
    } else {
        // The page entries share the room above the theme/settings buttons: full size when there is
        // plenty, closer together on a short window, so a long page list never runs into them.
        const int room = client.bottom - NavTop() - (Scale(16) + 2 * full + Scale(8));
        const int pitch = std::clamp(room / std::max<int>(1, int(g_pages.size())), Scale(30), full + Scale(2));
        h = pitch - Scale(2);
        y = NavTop() + id * pitch;
    }
    return RECT{x, y, x + w, y + h};
}

RECT FooterRect() {
    RECT client;
    GetClientRect(g_main, &client);
    return RECT{SidebarWidth(), client.bottom - FooterHeight(), client.right, client.bottom};
}

RECT PageArea() {
    RECT client;
    GetClientRect(g_main, &client);
    const int left = SidebarWidth() + Scale(28);
    RECT area{left, Scale(78), client.right - Scale(28), client.bottom - FooterHeight() - Scale(18)};
    if (area.bottom < area.top) area.bottom = area.top;
    if (area.right < area.left) area.right = area.left;
    return area;
}

int HitTest(POINT pt) {
    if (pt.x >= SidebarWidth()) return kNavNone;
    for (int id : {kNavTheme, kNavSettings}) {
        const RECT r = NavItemRect(id);
        if (PtInRect(&r, pt)) return id;
    }
    for (int i = 0; i < static_cast<int>(g_pages.size()); ++i) {
        const RECT r = NavItemRect(i);
        if (PtInRect(&r, pt)) return i;
    }
    return kNavNone;
}

// ---------------------------------------------------------------- painting

void DrawGlyph(HDC dc, wchar_t glyph, RECT rc, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, IconFont());
    SetTextColor(dc, color);
    DrawTextW(dc, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

void PaintNavItem(HDC dc, Gdiplus::Graphics& g, int id, wchar_t glyph, const wchar_t* label) {
    const RECT r = NavItemRect(id);
    const bool active = id == g_current;
    const bool hover = id == g_hover;
    if (active || hover) {
        const Gdiplus::RectF rf(float(r.left), float(r.top), float(r.right - r.left), float(r.bottom - r.top));
        FillRoundRect(g, active ? Blend(g_pal.sidebar, g_pal.accent, IsDark() ? 0.16 : 0.13) : Blend(g_pal.sidebar, g_pal.text, 0.06),
                      rf, ScaleF(8));
        if (active) {
            const Gdiplus::RectF bar(float(r.left), float(r.top) + ScaleF(10), ScaleF(3), float(r.bottom - r.top) - ScaleF(20));
            FillRoundRect(g, g_pal.accent, bar, ScaleF(1.5f));
        }
    }
    RECT icon{r.left + Scale(10), r.top, r.left + Scale(38), r.bottom};
    DrawGlyph(dc, glyph, icon, active ? g_pal.accent : (hover ? g_pal.text : g_pal.textDim));
    RECT text{r.left + Scale(44), r.top, r.right - Scale(8), r.bottom};
    HGDIOBJ old = SelectObject(dc, active ? UiFontBold() : UiFont());
    SetTextColor(dc, active || hover ? g_pal.text : g_pal.textDim);
    DrawTextW(dc, label, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
}

void PaintSidebar(HDC dc, const RECT& client) {
    RECT side{0, 0, SidebarWidth(), client.bottom};
    FillRect(dc, &side, BrushFor(g_pal.sidebar));
    RECT rule{side.right - 1, 0, side.right, client.bottom};
    FillRect(dc, &rule, BrushFor(g_pal.border));
    SetBkMode(dc, TRANSPARENT);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    // Logo: a gradient tile with the app glyph.
    const float tile = ScaleF(34), lx = ScaleF(22), ly = ScaleF(26);
    {
        Gdiplus::GraphicsPath path;
        RoundRectPath(path, Gdiplus::RectF(lx, ly, tile, tile), ScaleF(9));
        Gdiplus::LinearGradientBrush grad(Gdiplus::PointF(lx, ly), Gdiplus::PointF(lx + tile, ly + tile), ToColor(g_pal.accent),
                                          ToColor(g_pal.mem));
        g.FillPath(&grad, &path);
    }
    RECT tileRc{static_cast<LONG>(lx), static_cast<LONG>(ly), static_cast<LONG>(lx + tile), static_cast<LONG>(ly + tile)};
    DrawGlyph(dc, 0xE9D9, tileRc, RGB(255, 255, 255));
    RECT name{static_cast<LONG>(lx + tile) + Scale(12), static_cast<LONG>(ly) - Scale(1), side.right - Scale(8),
              static_cast<LONG>(ly) + Scale(19)};
    HGDIOBJ old = SelectObject(dc, UiFontBold());
    SetTextColor(dc, g_pal.text);
    DrawTextW(dc, kAppName, -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT sub = name;
    sub.top += Scale(18);
    sub.bottom += Scale(16);
    SelectObject(dc, CapsFont());
    SetTextColor(dc, g_pal.textFaint);
    DrawCapsText(dc, L"System monitor", sub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);

    for (int i = 0; i < static_cast<int>(g_pages.size()); ++i) {
        PaintNavItem(dc, g, i, kPageIcons[i], g_pages[i]->Title());
        if (g_pages[i]->NeedsAttention()) {  // reminder dot at the right end of the entry
            const RECT r = NavItemRect(i);
            const float d = ScaleF(8);
            Gdiplus::SolidBrush dot(ToColor(g_pal.warn));
            g.FillEllipse(&dot, float(r.right) - ScaleF(18), float(r.top + r.bottom) / 2 - d / 2, d, d);
        }
    }
    PaintNavItem(dc, g, kNavTheme, IsDark() ? 0xE706 : 0xE708, IsDark() ? L"Light mode" : L"Dark mode");
    PaintNavItem(dc, g, kNavSettings, 0xE713, L"Settings");
}

void PaintHeader(HDC dc) {
    if (g_current < 0) return;
    const RECT area = PageArea();
    RECT title{area.left, Scale(18), area.right, Scale(62)};
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(dc, TitleFont());
    SetTextColor(dc, g_pal.text);
    DrawTextW(dc, g_pages[static_cast<size_t>(g_current)]->Title(), -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, MonoFont());
    SetTextColor(dc, g_pal.textFaint);
    SYSTEMTIME now;
    GetLocalTime(&now);
    const std::wstring clock = Format(L"%04u-%02u-%02u  %02u:%02u:%02u", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    DrawTextW(dc, clock.c_str(), -1, &title, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old);
}

void PaintFooter(HDC dc) {
    const RECT f = FooterRect();
    FillRect(dc, &f, BrushFor(g_pal.sidebar));
    RECT rule{f.left, f.top, f.right, f.top + 1};
    FillRect(dc, &rule, BrushFor(g_pal.border));
    SetBkMode(dc, TRANSPARENT);
    const SystemSnapshot& s = Sys();
    const uint64_t used = s.memTotal - s.memAvail;
    struct Item {
        COLORREF dot;
        std::wstring text;
        const wchar_t* widest;  // fixed slot width, so neighbours never shift as digits change
    };
    const Item items[] = {
        {g_pal.cpu, Format(L"CPU %.0f%%", s.cpu), L"CPU 100%"},
        {g_pal.mem, Format(L"MEM %s / %s", FormatBytes(used).c_str(), FormatBytes(s.memTotal).c_str()), L"MEM 99.9 GB / 99.9 GB"},
        {g_pal.gpu, s.gpuAvailable ? Format(L"GPU %.0f%%", s.gpu) : L"GPU —", L"GPU 100%"},
        {g_pal.disk, Format(L"DISK %.0f%%", s.diskActive), L"DISK 100%"},
        {g_pal.net, L"NET " + FormatBytes(uint64_t(s.netIn + s.netOut)) + L"/s", L"NET 999.9 KB/s"},
    };
    HGDIOBJ old = SelectObject(dc, MonoFont());
    int x = f.left + Scale(28);
    const int cy = (f.top + f.bottom) / 2;
    std::vector<std::pair<int, COLORREF>> dots;
    for (const Item& it : items) {
        dots.push_back({x, it.dot});
        x += Scale(14);
        SIZE sz{};
        GetTextExtentPoint32W(dc, it.text.c_str(), static_cast<int>(it.text.size()), &sz);
        SetTextColor(dc, g_pal.textDim);
        TextOutW(dc, x, cy - sz.cy / 2, it.text.c_str(), static_cast<int>(it.text.size()));
        SIZE slot{};
        GetTextExtentPoint32W(dc, it.widest, static_cast<int>(wcslen(it.widest)), &slot);
        x += std::max(sz.cx, slot.cx) + Scale(22);
    }
    const std::wstring right = Format(L"%s PROC  ·  %s", FormatCount(g_procs.Processes().size()).c_str(),
                                      IsElevated() ? L"ADMIN" : L"STANDARD USER");
    SIZE rightSize{};
    GetTextExtentPoint32W(dc, right.c_str(), static_cast<int>(right.size()), &rightSize);
    if (x + rightSize.cx < f.right - Scale(28)) {  // only when it fits beside the metrics
        RECT rr{x, f.top, f.right - Scale(28), f.bottom};
        SetTextColor(dc, IsElevated() ? g_pal.accent : g_pal.textFaint);
        DrawTextW(dc, right.c_str(), -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);

    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float d = ScaleF(7);
    for (const auto& [dx, c] : dots) {
        Gdiplus::SolidBrush b(ToColor(c));
        g.FillEllipse(&b, float(dx), float(cy) - d / 2, d, d);
    }
}

void OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, BrushFor(g_pal.bg));
    PaintSidebar(mem, rc);
    PaintHeader(mem);
    PaintFooter(mem);
    BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, mem,
           ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- page host
// Page controls live in this child window rather than directly in the main window, so a
// page that doesn't fit can scroll without its controls sliding over the header or the
// footer. Control notifications are forwarded to the main window, so pages are unaware.

constexpr wchar_t kHostClass[] = L"MtmPageHost";
std::vector<int> g_scroll;  // per page, pixels
int g_hostThemeDark = -1;

int ScrollbarWidth() { return GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi); }

void LayoutPages();

void ScrollCurrentTo(int pos) {
    if (g_current < 0) return;
    SCROLLINFO si{sizeof si, SIF_ALL};
    if (!GetScrollInfo(g_pageHost, SB_VERT, &si)) return;
    const int maxPos = std::max(0, si.nMax - static_cast<int>(si.nPage) + 1);
    pos = std::clamp(pos, 0, maxPos);
    if (pos == g_scroll[size_t(g_current)]) return;
    g_scroll[size_t(g_current)] = pos;
    // Re-lay the page at the new offset with redraw suspended, then repaint once (no flicker).
    SendMessageW(g_pageHost, WM_SETREDRAW, FALSE, 0);
    LayoutPages();
    SendMessageW(g_pageHost, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(g_pageHost, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_FRAME);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, BrushFor(g_pal.bg));
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        FillRect(dc, &ps.rcPaint, BrushFor(g_pal.bg));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NOTIFY:
    case WM_COMMAND:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
    case WM_CONTEXTMENU:
    case WM_HSCROLL:
        return SendMessageW(g_main, msg, wp, lp);
    case WM_VSCROLL: {
        if (lp) return SendMessageW(g_main, msg, wp, lp);  // a control's own scrollbar / trackbar
        SCROLLINFO si{sizeof si, SIF_ALL};
        GetScrollInfo(hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= Scale(40); break;
        case SB_LINEDOWN: pos += Scale(40); break;
        case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
        case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = si.nMax; break;
        }
        ScrollCurrentTo(pos);
        return 0;
    }
    case WM_MOUSEWHEEL:
        // Reaches here when the control under the cursor didn't use it (labels, cards...).
        if (g_current >= 0) ScrollCurrentTo(g_scroll[size_t(g_current)] - GET_WHEEL_DELTA_WPARAM(wp) * Scale(90) / WHEEL_DELTA);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CreatePageHost(HWND main) {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = g_inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);
    g_pageHost = CreateWindowExW(0, kHostClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 0, 0,
                                 main, nullptr, g_inst, nullptr);
}

// Lays every page out inside the host. A page whose MinHeight doesn't fit gets a vertical
// scrollbar (only the current page's position is applied; the others start at the top).
void LayoutPages() {
    const RECT area = PageArea();
    RECT client;
    GetClientRect(g_main, &client);
    const int w = area.right - area.left, h = area.bottom - area.top;
    // The host reaches into the right margin, which is where its scrollbar goes.
    MoveWindow(g_pageHost, area.left, area.top, std::max<int>(w, client.right - area.left), h, TRUE);
    if (g_hostThemeDark != int(IsDark())) {
        g_hostThemeDark = int(IsDark());
        SetWindowTheme(g_pageHost, IsDark() ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }
    g_scroll.resize(g_pages.size(), 0);
    for (size_t i = 0; i < g_pages.size(); ++i) {
        const int need = g_pages[i]->MinHeight(w);
        const bool scrolls = need > h;
        const bool current = static_cast<int>(i) == g_current;
        int& pos = g_scroll[i];
        pos = scrolls ? std::clamp(pos, 0, need - h) : 0;
        const int offset = current ? pos : 0;
        g_pages[i]->Layout(RECT{0, -offset, w, (scrolls ? need : h) - offset});
        if (current) {
            SCROLLINFO si{sizeof si, SIF_RANGE | SIF_PAGE | SIF_POS};
            si.nMin = 0;
            si.nMax = scrolls ? need - 1 : 0;
            si.nPage = scrolls ? static_cast<UINT>(h) : 1;
            si.nPos = pos;
            SetScrollInfo(g_pageHost, SB_VERT, &si, TRUE);
            ShowScrollBar(g_pageHost, SB_VERT, scrolls);
            // Make Windows recompute the frame so the bar is actually laid out and drawn.
            SetWindowPos(g_pageHost, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
    }
}

// ---------------------------------------------------------------- behaviour

void Layout() {
    LayoutPages();
    InvalidateRect(g_main, nullptr, FALSE);
}

// Stable id for a page (FNV-1a of its title), used to remember the last open page.
DWORD PageId(const Page& page) {
    DWORD h = 2166136261u;
    for (const wchar_t* p = page.Title(); *p; ++p) h = (h ^ static_cast<DWORD>(*p)) * 16777619u;
    return h;
}

void SelectPage(int index) {
    if (index < 0 || index >= static_cast<int>(g_pages.size()) || index == g_current) return;
    if (g_current >= 0) g_pages[static_cast<size_t>(g_current)]->Show(false);
    g_current = index;
    LayoutPages();  // applies this page's scroll position and scrollbar
    g_pages[static_cast<size_t>(index)]->Show(true);
    g_pages[static_cast<size_t>(index)]->OnActivate();
    // Remembered by page name, so reordering the sidebar never reopens the wrong page.
    WriteSetting(L"LastTabId", PageId(*g_pages[static_cast<size_t>(index)]));
    InvalidateRect(g_main, nullptr, FALSE);
}

void Tick() {
    g_procs.Sample();
    ResolveProcessIcons();
    SampleSystem();
    g_recorder.Add(CaptureSample());  // always-on in-memory buffer; also streams to file while recording
    for (size_t i = 0; i < g_pages.size(); ++i) g_pages[i]->OnTick(static_cast<int>(i) == g_current);
    RECT f = FooterRect();
    InvalidateRect(g_main, &f, FALSE);
    const RECT area = PageArea();
    RECT header{area.left, 0, area.right, area.top};
    InvalidateRect(g_main, &header, FALSE);
}

UINT g_animFps = 60;

// 0 = off (graphs step once per sample). Windows timers tick at ~15.6 ms, so 16 ms ~ 60 fps.
void SetAnimationFps(UINT fps) {
    g_animFps = fps;
    SetSmoothAnimation(fps != 0);
    WriteSetting(L"AnimFps", fps);
    if (fps) SetTimer(g_main, kAnimTimerId, fps >= 60 ? 16 : 33, nullptr);
    else KillTimer(g_main, kAnimTimerId);
    InvalidateRect(g_main, nullptr, FALSE);
    RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void SetSpeed(UINT ms) {
    g_interval = ms;
    SetUpdateIntervalMs(ms);  // graphs scroll exactly one step per sample
    SetTimer(g_main, kTimerId, g_interval, nullptr);
    WriteSetting(L"IntervalMs", g_interval);
}

void SetTopmost(bool on) {
    g_topmost = on;
    SetWindowPos(g_main, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    WriteSetting(L"TopMost", on ? 1u : 0u);
}

void RestartElevated() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) DestroyWindow(g_main);
}

void ShowSettingsMenu() {
    const RECT item = NavItemRect(kNavSettings);
    POINT pt{item.right + Scale(4), item.top};
    ClientToScreen(g_main, &pt);
    HMENU menu = CreatePopupMenu();
    HMENU theme = CreatePopupMenu();
    AppendMenuW(theme, MF_STRING, ID_THEME_SYSTEM, L"Match Windows");
    AppendMenuW(theme, MF_STRING, ID_THEME_DARK, L"Dark");
    AppendMenuW(theme, MF_STRING, ID_THEME_LIGHT, L"Light");
    CheckMenuRadioItem(theme, ID_THEME_SYSTEM, ID_THEME_LIGHT, ID_THEME_SYSTEM + static_cast<UINT>(GetThemeMode()), MF_BYCOMMAND);
    HMENU accent = CreatePopupMenu();
    for (int i = 0; i < AccentCount(); ++i) AppendMenuW(accent, MF_STRING, ID_ACCENT_FIRST + i, AccentName(i));
    CheckMenuRadioItem(accent, ID_ACCENT_FIRST, ID_ACCENT_FIRST + AccentCount() - 1, ID_ACCENT_FIRST + AccentIndex(), MF_BYCOMMAND);
    HMENU speed = CreatePopupMenu();
    AppendMenuW(speed, MF_STRING, ID_SPEED_FAST, L"Fast (0.5 s)");
    AppendMenuW(speed, MF_STRING, ID_SPEED_NORMAL, L"Normal (1 s)");
    AppendMenuW(speed, MF_STRING, ID_SPEED_SLOW, L"Slow (2 s)");
    CheckMenuRadioItem(speed, ID_SPEED_FAST, ID_SPEED_SLOW,
                       g_interval <= 500 ? ID_SPEED_FAST : (g_interval >= 2000 ? ID_SPEED_SLOW : ID_SPEED_NORMAL), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(theme), L"Theme");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(accent), L"Accent colour");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"Update speed");
    HMENU anim = CreatePopupMenu();
    AppendMenuW(anim, MF_STRING, ID_ANIM_60, L"Smooth (60 fps)");
    AppendMenuW(anim, MF_STRING, ID_ANIM_30, L"Balanced (30 fps)");
    AppendMenuW(anim, MF_STRING, ID_ANIM_OFF, L"Off (step once per sample)");
    CheckMenuRadioItem(anim, ID_ANIM_OFF, ID_ANIM_60,
                       g_animFps >= 60 ? ID_ANIM_60 : (g_animFps ? ID_ANIM_30 : ID_ANIM_OFF), MF_BYCOMMAND);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(anim), L"Graph animation");
    AppendMenuW(menu, MF_STRING | (g_topmost ? MF_CHECKED : 0), ID_VIEW_TOPMOST, L"Always on top");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (IsElevated() ? MF_GRAYED : 0), ID_FILE_ADMIN, L"Restart as administrator");
    AppendMenuW(menu, MF_STRING, ID_VIEW_REFRESH, L"Refresh now\tF5");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_FILE_EXIT, L"Exit");
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTBUTTON, pt.x, pt.y, 0, g_main, nullptr));
    DestroyMenu(menu);
    if (cmd) SendMessageW(g_main, WM_COMMAND, cmd, 0);
}

void ChangeTheme(ThemeMode mode) {
    SetThemeMode(mode);
    ApplyTheme(g_main);
}

void OnCommand(WORD id) {
    switch (id) {
    case ID_FILE_ADMIN: RestartElevated(); break;
    case ID_FILE_EXIT: DestroyWindow(g_main); break;
    case ID_VIEW_REFRESH:
        if (g_current >= 0) g_pages[static_cast<size_t>(g_current)]->OnActivate();
        break;
    case ID_VIEW_TOPMOST: SetTopmost(!g_topmost); break;
    case ID_ANIM_OFF: SetAnimationFps(0); break;
    case ID_ANIM_30: SetAnimationFps(30); break;
    case ID_ANIM_60: SetAnimationFps(60); break;
    case ID_SPEED_FAST: SetSpeed(500); break;
    case ID_SPEED_NORMAL: SetSpeed(1000); break;
    case ID_SPEED_SLOW: SetSpeed(2000); break;
    case ID_NEXT_TAB: SelectPage((g_current + 1) % static_cast<int>(g_pages.size())); break;
    case ID_PREV_TAB: SelectPage((g_current + static_cast<int>(g_pages.size()) - 1) % static_cast<int>(g_pages.size())); break;
    case ID_THEME_SYSTEM: ChangeTheme(ThemeMode::System); break;
    case ID_THEME_DARK: ChangeTheme(ThemeMode::Dark); break;
    case ID_THEME_LIGHT: ChangeTheme(ThemeMode::Light); break;
    default:
        if (id >= ID_ACCENT_FIRST && id < ID_ACCENT_FIRST + AccentCount()) {
            SetAccentIndex(id - ID_ACCENT_FIRST);
            ApplyTheme(g_main);
        }
    }
}

void OnCreate(HWND hwnd) {
    g_main = hwnd;
    g_dpi = GetDpiForWindow(hwnd);
    RecreateFonts();

    CreatePageHost(hwnd);  // before the pages: their controls are created inside it
    // Sidebar order, top to bottom (kPageIcons must follow the same order).
    g_pages.push_back(MakeSummaryPage());
    g_pages.push_back(MakePerformancePage());
    g_pages.push_back(MakeProcessesPage());
    g_pages.push_back(MakeServicesPage());
    g_pages.push_back(MakeStartupPage());
    g_pages.push_back(MakeInstalledAppsPage());
    g_pages.push_back(MakeUsersPage());
    g_pages.push_back(MakePowerPage());
    g_pages.push_back(MakeTweaksPage());
    g_pages.push_back(MakeLatencyPage());
    g_pages.push_back(MakeDiskSpacePage());
    g_pages.push_back(MakeHealthPage());
    g_pages.push_back(MakeConnectionsPage());
    g_pages.push_back(MakeRecorderPage());
    g_pages.push_back(MakeBenchmarksPage());
    g_pages.push_back(MakeInputPage());
    g_pages.push_back(MakeDevicesPage());
    if (g_pages.size() != ARRAYSIZE(kPageIcons)) {
        MessageBoxW(hwnd, L"kPageIcons doesn't match the page list.", kAppName, MB_ICONERROR);
        ExitProcess(1);
    }
    for (auto& page : g_pages) {
        page->Create();
        page->Show(false);
    }

    SetSpeed(ReadSetting(L"IntervalMs", 1000));
    SetAnimationFps(ReadSetting(L"AnimFps", 60));
    if (ReadSetting(L"TopMost", 0)) SetTopmost(true);
    ApplyTheme(hwnd);

    g_procs.Sample();  // prime CPU deltas
    SampleSystem();
    Layout();
    const DWORD lastId = ReadSetting(L"LastTabId", 0);
    size_t last = 0;
    for (size_t i = 0; i < g_pages.size(); ++i)
        if (PageId(*g_pages[i]) == lastId) last = i;
    SelectPage(last < g_pages.size() ? static_cast<int>(last) : 0);
    Tick();
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        OnCreate(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        OnPaint(hwnd);
        return 0;
    case WM_SIZE:
        if (!g_pages.empty()) Layout();
        return 0;
    case WM_MOUSEWHEEL: {  // focus is on the main window (e.g. after a sidebar click): scroll the page
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        RECT hr;
        GetWindowRect(g_pageHost, &hr);
        if (PtInRect(&hr, pt)) return SendMessageW(g_pageHost, msg, wp, lp);
        break;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(960), Scale(760)};  // 12 sidebar items + settings
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerId) Tick();
        else if (wp == kAnimTimerId) Widget::AnimateFrame();
        return 0;
    case WM_MOUSEMOVE: {
        const int hit = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit != g_hover) {
            g_hover = hit;
            RECT side{0, 0, SidebarWidth(), 32767};
            InvalidateRect(hwnd, &side, FALSE);
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_hover != kNavNone) {
            g_hover = kNavNone;
            RECT side{0, 0, SidebarWidth(), 32767};
            InvalidateRect(hwnd, &side, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const int hit = HitTest(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        if (hit == kNavTheme) ChangeTheme(IsDark() ? ThemeMode::Light : ThemeMode::Dark);
        else if (hit == kNavSettings) ShowSettingsMenu();
        else if (hit >= 0) SelectPage(hit);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            SetCursor(LoadCursorW(nullptr, HitTest(pt) != kNavNone ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        LRESULT result = 0;
        if (CustomDrawSlider(nm, result)) return result;  // rounded sliders everywhere
        for (auto& page : g_pages)
            if (page->OnNotify(nm, result)) return result;
        break;
    }
    case WM_COMMAND:
        if (lp) {
            for (auto& page : g_pages)
                if (page->OnCommand(reinterpret_cast<HWND>(lp), HIWORD(wp))) return 0;
            break;
        }
        OnCommand(LOWORD(wp));
        return 0;
    case WM_HSCROLL:
        if (lp)
            for (auto& page : g_pages)
                if (page->OnScroll(reinterpret_cast<HWND>(lp), LOWORD(wp))) return 0;
        break;
    case WM_CONTEXTMENU:
        if (g_current >= 0 && g_pages[static_cast<size_t>(g_current)]->OnContextMenu(
                                  reinterpret_cast<HWND>(wp), POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}))
            return 0;
        break;
    case WM_DRAWITEM:
        if (DrawThemedItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lp))) return TRUE;
        break;
    case WM_MEASUREITEM:
        MeasureThemedItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lp));
        return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN:
        return ThemedCtlColor(msg, reinterpret_cast<HDC>(wp), reinterpret_cast<HWND>(lp));
    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0 && GetThemeMode() == ThemeMode::System) {
            RebuildPalette();
            ApplyTheme(hwnd);
        }
        break;
    case WM_APP_RUN: {
        auto* fn = reinterpret_cast<std::function<void()>*>(lp);
        (*fn)();
        delete fn;
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RecreateFonts();
        EnumChildWindows(
            hwnd,
            [](HWND child, LPARAM) -> BOOL {
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(UiFont()), TRUE);
                return TRUE;
            },
            0);
        const auto* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DESTROY: {
        WINDOWPLACEMENT wpl{};
        wpl.length = sizeof wpl;
        if (GetWindowPlacement(hwnd, &wpl)) WriteSettingBinary(L"Placement", &wpl, sizeof wpl);
        KillTimer(hwnd, kTimerId);
        KillTimer(hwnd, kAnimTimerId);
        g_recorder.Stop();  // join the writer and flush before global destruction
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RestorePlacement(HWND hwnd, int showCmd) {
    WINDOWPLACEMENT wpl{};
    if (ReadSettingBinary(L"Placement", &wpl, sizeof wpl) && wpl.length == sizeof wpl) {
        if (wpl.showCmd == SW_SHOWMINIMIZED || wpl.showCmd == SW_MINIMIZE) wpl.showCmd = SW_SHOWNORMAL;
        if (showCmd != SW_SHOWNORMAL && showCmd != SW_SHOWDEFAULT) wpl.showCmd = static_cast<UINT>(showCmd);
        SetWindowPlacement(hwnd, &wpl);
    } else {
        ShowWindow(hwnd, showCmd);
    }
}

}  // namespace
}  // namespace mtm

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    using namespace mtm;
    // Elevated helper mode (started by the DNS switcher): apply and exit, no window.
    if (int code = 0; HandleSetDnsCommandLine(code)) return code;
    if (int code = 0; HandleOptimizeCommandLine(code)) return code;  // elevated defrag / TRIM helper
    if (int code = 0; HandlePowerCommandLine(code)) return code;     // elevated power-plan / parking helper
    if (int code = 0; HandleTweakCommandLine(code)) return code;     // elevated game-tweaks helper
    if (int code = 0; HandleLatencyCommandLine(code)) return code;   // elevated latency-test helper + its background load
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = instance;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof icc;
    icc.dwICC = ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    LoadThemeSettings();
    Widget::Register();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    // App icon from res\app.rc (id 1): large for Alt+Tab / taskbar, small for the title bar.
    wc.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const UINT dpi = GetDpiForSystem();
    const std::wstring title = IsElevated() ? L"Kernova (Administrator)" : kAppName;
    HWND hwnd = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                CW_USEDEFAULT, MulDiv(1320, static_cast<int>(dpi), 96), MulDiv(840, static_cast<int>(dpi), 96),
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) return 1;
    RestorePlacement(hwnd, showCmd);

    ACCEL accels[] = {
        {FVIRTKEY, VK_F5, ID_VIEW_REFRESH},
        {FVIRTKEY | FCONTROL, VK_TAB, ID_NEXT_TAB},
        {FVIRTKEY | FCONTROL | FSHIFT, VK_TAB, ID_PREV_TAB},
    };
    HACCEL accel = CreateAcceleratorTableW(accels, ARRAYSIZE(accels));

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(hwnd, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyAcceleratorTable(accel);
    g_pages.clear();  // joins any background workers
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
