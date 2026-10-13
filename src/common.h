#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

namespace mtm {

// Posted to the main window to run a callback on the UI thread.
constexpr UINT WM_APP_RUN = WM_APP + 1;
constexpr wchar_t kAppName[] = L"Kernova";

extern HINSTANCE g_inst;
extern HWND g_main;
// Scrollable container that page controls live in (forwards their messages to g_main).
// Falls back to g_main until it exists.
extern HWND g_pageHost;
inline HWND PageParent() { return g_pageHost ? g_pageHost : g_main; }
extern UINT g_dpi;

int Scale(int px);
float ScaleF(float px);

// Sampling interval (set by the main window) and whether graphs animate between samples.
UINT UpdateIntervalMs();
void SetUpdateIntervalMs(UINT ms);
bool SmoothAnimation();
void SetSmoothAnimation(bool on);

// Bumped whenever fonts or the palette change, so cached renderings know to redraw.
unsigned VisualGeneration();
void BumpVisualGeneration();

// Fonts (recreated on DPI change).
HFONT UiFont();     // body text
HFONT UiFontBold(); // semibold body
HFONT TitleFont();  // page titles
HFONT ValueFont();  // big numbers on cards
HFONT CapsFont();   // small uppercase labels
HFONT MonoFont();   // tabular numbers in the footer and grids
HFONT IconFont();   // Segoe Fluent Icons / MDL2 glyphs
void RecreateFonts();

// Runs fn on the UI thread. Safe to call from any thread; dropped if the window is gone.
void PostToUi(std::function<void()> fn);

std::wstring Format(const wchar_t* fmt, ...);
std::wstring FormatBytes(uint64_t bytes);
std::wstring FormatCount(uint64_t n);
std::wstring FormatFileTime(const FILETIME& ft, bool withTime = true);
std::wstring FormatFileTime(uint64_t ft, bool withTime = true);
std::wstring FormatDuration(uint64_t seconds);
std::wstring ErrorText(DWORD err);
std::wstring ToLower(std::wstring s);
std::wstring ToUpper(std::wstring s);

HWND CreateChild(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle = 0);
void SetText(HWND h, const std::wstring& text);
std::wstring GetText(HWND h);
void ShowAll(std::initializer_list<HWND> windows, bool show);
void Place(HWND h, int x, int y, int w, int h2);

// Draws text in uppercase with a little letter spacing (the "caps label" style).
void DrawCapsText(HDC dc, const std::wstring& text, RECT rc, UINT format);

// Settings under HKCU\Software\Kernova (copied once from an older BetterTaskManager / MyTaskManager key).
DWORD ReadSetting(const wchar_t* name, DWORD fallback);
void WriteSetting(const wchar_t* name, DWORD value);
bool ReadSettingBinary(const wchar_t* name, void* data, DWORD size);
void WriteSettingBinary(const wchar_t* name, const void* data, DWORD size);

bool IsElevated();
void CopyToClipboard(const std::wstring& text);
void ShowError(const std::wstring& message);
bool Confirm(const std::wstring& message);
void ShowInExplorer(const std::wstring& path, bool selectItem);

// Shell file pickers. Return an empty string when cancelled.
std::wstring PickFile(bool save, const wchar_t* filterName, const wchar_t* pattern, const wchar_t* defaultExt,
                      const wchar_t* defaultName = nullptr);
std::wstring PickFolder();

class Stopwatch {
public:
    Stopwatch() { Reset(); }
    void Reset() { QueryPerformanceCounter(&start_); }
    double Seconds() const {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        return double(now.QuadPart - start_.QuadPart) / double(freq.QuadPart);
    }

private:
    LARGE_INTEGER start_{};
};

}  // namespace mtm
