#include "common.h"

#include <shellapi.h>
#include <shobjidl.h>

#include <cstdarg>
#include <algorithm>
#include <cstring>

namespace mtm {

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
HWND g_pageHost = nullptr;
UINT g_dpi = 96;

namespace {

constexpr wchar_t kSettingsKey[] = L"Software\\Kernova";

enum FontSlot { kUi, kUiBold, kTitle, kValue, kCaps, kMono, kIcon, kFontCount };
HFONT g_fonts[kFontCount] = {};
// The previous generation stays alive until the next DPI change so controls never
// paint with a deleted font while WM_SETFONT is being propagated.
HFONT g_prevFonts[kFontCount] = {};

bool FontExists(const wchar_t* face) {
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, face);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(
        dc, &lf,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *reinterpret_cast<bool*>(p) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

const wchar_t* FirstFont(std::initializer_list<const wchar_t*> faces) {
    for (const wchar_t* f : faces)
        if (FontExists(f)) return f;
    return L"Segoe UI";
}

HFONT MakeFont(const wchar_t* face, int px, int weight) {
    LOGFONTW lf{};
    lf.lfHeight = -Scale(px);
    lf.lfWeight = weight;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    wcscpy_s(lf.lfFaceName, face);
    return CreateFontIndirectW(&lf);
}

}  // namespace

int Scale(int px) { return MulDiv(px, static_cast<int>(g_dpi), 96); }
float ScaleF(float px) { return px * float(g_dpi) / 96.0f; }

namespace {
UINT g_updateMs = 1000;
bool g_smooth = true;
}  // namespace

UINT UpdateIntervalMs() { return g_updateMs; }
void SetUpdateIntervalMs(UINT ms) { g_updateMs = ms ? ms : 1000; }
bool SmoothAnimation() { return g_smooth; }
void SetSmoothAnimation(bool on) { g_smooth = on; }

namespace {
unsigned g_visualGeneration = 1;
}  // namespace
unsigned VisualGeneration() { return g_visualGeneration; }
void BumpVisualGeneration() { ++g_visualGeneration; }

HFONT UiFont() { return g_fonts[kUi]; }
HFONT UiFontBold() { return g_fonts[kUiBold]; }
HFONT TitleFont() { return g_fonts[kTitle]; }
HFONT ValueFont() { return g_fonts[kValue]; }
HFONT CapsFont() { return g_fonts[kCaps]; }
HFONT MonoFont() { return g_fonts[kMono]; }
HFONT IconFont() { return g_fonts[kIcon]; }

void RecreateFonts() {
    BumpVisualGeneration();
    static const wchar_t* text = FirstFont({L"Segoe UI Variable Text", L"Segoe UI"});
    static const wchar_t* display = FirstFont({L"Segoe UI Variable Display", L"Segoe UI"});
    static const wchar_t* mono = FirstFont({L"Cascadia Mono", L"Consolas"});
    static const wchar_t* icons = FirstFont({L"Segoe Fluent Icons", L"Segoe MDL2 Assets"});
    for (int i = 0; i < kFontCount; ++i) {
        if (g_prevFonts[i]) DeleteObject(g_prevFonts[i]);
        g_prevFonts[i] = g_fonts[i];
    }
    g_fonts[kUi] = MakeFont(text, 13, FW_NORMAL);
    g_fonts[kUiBold] = MakeFont(text, 13, FW_SEMIBOLD);
    g_fonts[kTitle] = MakeFont(display, 26, FW_SEMIBOLD);
    g_fonts[kValue] = MakeFont(display, 30, FW_LIGHT);
    g_fonts[kCaps] = MakeFont(text, 11, FW_SEMIBOLD);
    g_fonts[kMono] = MakeFont(mono, 12, FW_NORMAL);
    g_fonts[kIcon] = MakeFont(icons, 16, FW_NORMAL);
}

void PostToUi(std::function<void()> fn) {
    auto* boxed = new std::function<void()>(std::move(fn));
    if (!g_main || !PostMessageW(g_main, WM_APP_RUN, 0, reinterpret_cast<LPARAM>(boxed))) delete boxed;
}

std::wstring Format(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int n = _vscwprintf(fmt, args);
    va_end(args);
    std::wstring s;
    if (n > 0) {
        s.resize(static_cast<size_t>(n) + 1);
        vswprintf_s(s.data(), s.size(), fmt, copy);
        s.resize(static_cast<size_t>(n));
    }
    va_end(copy);
    return s;
}

std::wstring FormatBytes(uint64_t bytes) {
    static const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB", L"PB"};
    double v = static_cast<double>(bytes);
    int i = 0;
    while (v >= 1024.0 && i < 5) {
        v /= 1024.0;
        ++i;
    }
    if (i == 0) return Format(L"%llu B", static_cast<unsigned long long>(bytes));
    return Format(v < 10 ? L"%.2f %s" : v < 100 ? L"%.1f %s" : L"%.0f %s", v, units[i]);
}

std::wstring FormatCount(uint64_t n) {
    std::wstring s = std::to_wstring(n);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), L",");
    return s;
}

std::wstring FormatFileTime(const FILETIME& ft, bool withTime) {
    if (!ft.dwLowDateTime && !ft.dwHighDateTime) return L"";
    SYSTEMTIME utc, local;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"";
    if (!withTime) return Format(L"%04u-%02u-%02u", local.wYear, local.wMonth, local.wDay);
    return Format(L"%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute,
                  local.wSecond);
}

std::wstring FormatFileTime(uint64_t ft, bool withTime) {
    FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    return FormatFileTime(f, withTime);
}

std::wstring FormatDuration(uint64_t seconds) {
    const uint64_t d = seconds / 86400, h = seconds / 3600 % 24, m = seconds / 60 % 60, s = seconds % 60;
    return Format(L"%llu:%02llu:%02llu:%02llu", d, h, m, s);
}

std::wstring ErrorText(DWORD err) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr,
                   err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring s = buf ? buf : L"Unknown error";
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return Format(L"%s (%lu)", s.c_str(), err);
}

std::wstring ToLower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::wstring ToUpper(std::wstring s) {
    if (!s.empty()) CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

HWND CreateChild(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle) {
    HWND h = CreateWindowExW(exStyle, cls, text, style | WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, PageParent(), nullptr,
                             g_inst, nullptr);
    if (h) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(UiFont()), FALSE);
    return h;
}

void SetText(HWND h, const std::wstring& text) {
    // Avoid flicker from re-setting identical text every tick.
    if (GetText(h) != text) SetWindowTextW(h, text.c_str());
}

std::wstring GetText(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, s.data(), n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}

void ShowAll(std::initializer_list<HWND> windows, bool show) {
    for (HWND h : windows) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

void Place(HWND h, int x, int y, int w, int h2) { MoveWindow(h, x, y, w < 0 ? 0 : w, h2 < 0 ? 0 : h2, TRUE); }

void DrawCapsText(HDC dc, const std::wstring& text, RECT rc, UINT format) {
    const int old = SetTextCharacterExtra(dc, std::max(1, Scale(1)));
    std::wstring upper = ToUpper(text);
    DrawTextW(dc, upper.c_str(), -1, &rc, format | DT_NOPREFIX);
    SetTextCharacterExtra(dc, old);
}

namespace {
// Earlier builds were called BetterTaskManager and, before that, MyTaskManager; the first
// time settings are touched, copy the newest old ones (theme, accent, window position, last
// page...) across. Runs once per process.
void MigrateOldSettings() {
    static const bool done = [] {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            RegCloseKey(key);  // new key already exists: nothing to do
            return true;
        }
        HKEY oldKey = nullptr, newKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\BetterTaskManager", 0, KEY_READ, &oldKey) != ERROR_SUCCESS &&
            RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\MyTaskManager", 0, KEY_READ, &oldKey) != ERROR_SUCCESS)
            return true;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &newKey, nullptr) ==
            ERROR_SUCCESS) {
            RegCopyTreeW(oldKey, nullptr, newKey);
            RegCloseKey(newKey);
        }
        RegCloseKey(oldKey);
        return true;
    }();
    (void)done;
}
}  // namespace

DWORD ReadSetting(const wchar_t* name, DWORD fallback) {
    MigrateOldSettings();
    DWORD value = 0, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS
               ? value
               : fallback;
}

namespace {
void WriteRaw(const wchar_t* name, const void* data, DWORD size, DWORD type) {
    MigrateOldSettings();
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, type, static_cast<const BYTE*>(data), size);
        RegCloseKey(key);
    }
}
}  // namespace

void WriteSetting(const wchar_t* name, DWORD value) { WriteRaw(name, &value, sizeof value, REG_DWORD); }

bool ReadSettingBinary(const wchar_t* name, void* data, DWORD size) {
    MigrateOldSettings();
    DWORD got = size;
    return RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_BINARY, nullptr, data, &got) == ERROR_SUCCESS &&
           got == size;
}

void WriteSettingBinary(const wchar_t* name, const void* data, DWORD size) { WriteRaw(name, data, size, REG_BINARY); }

bool IsElevated() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    HANDLE token = nullptr;
    cached = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION e{};
        DWORD size = 0;
        if (GetTokenInformation(token, TokenElevation, &e, sizeof e, &size) && e.TokenIsElevated) cached = 1;
        CloseHandle(token);
    }
    return cached == 1;
}

void CopyToClipboard(const std::wstring& text) {
    if (!OpenClipboard(g_main)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(h)) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

void ShowError(const std::wstring& message) { MessageBoxW(g_main, message.c_str(), kAppName, MB_OK | MB_ICONWARNING); }

bool Confirm(const std::wstring& message) {
    return MessageBoxW(g_main, message.c_str(), kAppName, MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK;
}

void ShowInExplorer(const std::wstring& path, bool selectItem) {
    if (path.empty()) return;
    if (selectItem) {
        const std::wstring args = L"/select,\"" + path + L"\"";
        ShellExecuteW(g_main, nullptr, L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else {
        ShellExecuteW(g_main, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

std::wstring PickFile(bool save, const wchar_t* filterName, const wchar_t* pattern, const wchar_t* defaultExt,
                      const wchar_t* defaultName) {
    std::wstring result;
    IFileDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                save ? IID_IFileSaveDialog : IID_IFileOpenDialog, reinterpret_cast<void**>(&dlg))))
        return result;
    const COMDLG_FILTERSPEC filters[] = {{filterName, pattern}, {L"All files", L"*.*"}};
    dlg->SetFileTypes(2, filters);
    dlg->SetDefaultExtension(defaultExt);
    if (defaultName) dlg->SetFileName(defaultName);
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (SUCCEEDED(dlg->Show(g_main))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

std::wstring PickFolder() {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dlg->Show(g_main))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

}  // namespace mtm
