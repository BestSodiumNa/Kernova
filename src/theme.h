#pragma once

#include "common.h"

namespace mtm {

struct Palette {
    COLORREF bg;         // window background
    COLORREF sidebar;    // navigation rail and title bar
    COLORREF surface;    // cards
    COLORREF surface2;   // buttons, inputs
    COLORREF hover;      // hovered surface
    COLORREF border;
    COLORREF text;
    COLORREF textDim;
    COLORREF textFaint;
    COLORREF accent;
    COLORREF onAccent;   // text drawn on top of the accent
    COLORREF good, warn, bad;
    COLORREF cpu, gpu, mem, disk, net;  // metric colours used across graphs
};

enum class ThemeMode : DWORD { System = 0, Dark = 1, Light = 2 };

extern Palette g_pal;

void LoadThemeSettings();  // reads saved mode/accent and builds the palette
bool IsDark();
ThemeMode GetThemeMode();
void SetThemeMode(ThemeMode mode);  // saves and rebuilds the palette (call ApplyTheme to repaint)
int AccentIndex();
int AccentCount();
const wchar_t* AccentName(int index);
void SetAccentIndex(int index);
void RebuildPalette();  // e.g. after the system theme changed

COLORREF Blend(COLORREF a, COLORREF b, double t);
HBRUSH BrushFor(COLORREF c);  // cached; never delete the result

// Applies the palette to the frame, popup menus and every child control, then repaints.
void ApplyTheme(HWND topLevel);
void ThemeControl(HWND control);

// Themed control factories (children of the main window).
// Button text may start with a Segoe icon glyph (U+E000..U+F8FF) followed by the label.
HWND CreateButton(const wchar_t* text, bool primary = false);
// Switches a button between the accent (primary) and plain look, e.g. for segmented tabs.
void SetButtonPrimary(HWND button, bool primary);
// Rounded search-style box; `glyph` is a leading icon (Segoe Fluent), 0 for none.
HWND CreateEdit(const wchar_t* text, const wchar_t* cue, wchar_t glyph = 0xE721 /* search */);
HWND CreateCombo(std::initializer_list<const wchar_t*> items, int selected = 0);
// Height of a dropdown when closed (it ignores the height passed to MoveWindow).
int ComboFieldHeight();
HWND CreateLabel(const wchar_t* text, DWORD extraStyle = 0);

// Handlers the main window forwards to.
bool DrawThemedItem(const DRAWITEMSTRUCT* dis);
void MeasureThemedItem(MEASUREITEMSTRUCT* mis);
// Rounded track + round knob for trackbars (call from WM_NOTIFY / NM_CUSTOMDRAW).
bool CustomDrawSlider(NMHDR* nm, LRESULT& result);
void ThemeSlider(HWND trackbar);
LRESULT ThemedCtlColor(UINT msg, HDC dc, HWND control);

}  // namespace mtm
