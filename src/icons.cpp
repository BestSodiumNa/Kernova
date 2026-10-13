#include "icons.h"

#include "processes.h"

#include "gdip.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <mutex>

#include <unordered_map>

namespace mtm {
namespace {

struct Entry {
    std::wstring name;  // process name when resolved, to catch PID reuse
    HICON icon = nullptr;
    bool resolved = false;
};

std::unordered_map<DWORD, Entry> g_byPid;
// One icon per executable path, shared by every instance (svchost, browser tabs...).
// Kept for the app's lifetime: the set of distinct executables is small.
std::unordered_map<std::wstring, HICON> g_byPath;

HICON DefaultIcon() {
    static HICON icon = [] {
        SHSTOCKICONINFO info{};
        info.cbSize = sizeof info;
        return SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_SMALLICON, &info))
                   ? info.hIcon
                   : LoadIconW(nullptr, IDI_APPLICATION);
    }();
    return icon;
}

HICON IconForPath(const std::wstring& path) {
    const std::wstring key = ToLower(path);
    auto it = g_byPath.find(key);
    if (it != g_byPath.end()) return it->second;
    SHFILEINFOW info{};
    HICON icon = nullptr;
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof info, SHGFI_ICON | SHGFI_SMALLICON)) icon = info.hIcon;
    g_byPath[key] = icon;  // nullptr is cached too: "no icon, use the default"
    return icon;
}

}  // namespace

HICON ProcessIcon(DWORD pid) {
    auto it = g_byPid.find(pid);
    if (it == g_byPid.end()) {
        g_byPid.emplace(pid, Entry{});
        return DefaultIcon();
    }
    return it->second.icon ? it->second.icon : DefaultIcon();
}

void ResolveProcessIcons() {
    std::unordered_map<DWORD, const std::wstring*> live;
    for (const ProcInfo& p : g_procs.Processes()) live[p.pid] = &p.name;

    for (auto it = g_byPid.begin(); it != g_byPid.end();) {
        auto l = live.find(it->first);
        const bool gone = l == live.end();
        const bool reused = !gone && it->second.resolved && it->second.name != *l->second;
        if (gone || reused) it = g_byPid.erase(it);
        else ++it;
    }

    // OpenProcess + SHGetFileInfo cost about a millisecond each; spread the first
    // launch's ~200 lookups over several ticks instead of freezing the window.
    int budget = 32;
    for (auto& [pid, e] : g_byPid) {
        if (e.resolved) continue;
        if (budget-- <= 0) break;
        e.resolved = true;
        e.name = *live[pid];
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;  // protected/system processes keep the default icon
        wchar_t buf[MAX_PATH * 4];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) e.icon = IconForPath(std::wstring(buf, size));
        CloseHandle(h);
    }
}

}  // namespace mtm

namespace mtm {
namespace {

std::mutex g_locMutex;
std::unordered_map<std::wstring, HICON> g_byLocation;  // guarded by g_locMutex

int SmallIconSize() { return GetSystemMetricsForDpi(SM_CXSMICON, g_dpi); }

std::wstring Expand(const std::wstring& s) {
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = ExpandEnvironmentStringsW(s.c_str(), buf, ARRAYSIZE(buf));
    return n && n <= ARRAYSIZE(buf) ? std::wstring(buf) : s;
}

// Store app logos are PNGs, often only present as "Logo.scale-200.png" etc.
std::wstring ResolvePng(const std::wstring& path) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) return path;
    const size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return L"";
    const std::wstring folder = slash == std::wstring::npos ? L"" : path.substr(0, slash + 1);
    const std::wstring pattern = path.substr(0, dot) + L".*" + path.substr(dot);
    std::wstring best;
    int bestRank = 1 << 30;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return L"";
    do {
        const std::wstring name = ToLower(fd.cFileName);
        // Prefer plain small renditions; skip high-contrast and light-theme variants.
        int rank = 50;
        if (name.find(L"altform-lightunplated") != std::wstring::npos) rank += 100;
        if (name.find(L"targetsize-32") != std::wstring::npos) rank = 0;
        else if (name.find(L"targetsize-24") != std::wstring::npos) rank = 1;
        else if (name.find(L"targetsize-48") != std::wstring::npos) rank = 2;
        else if (name.find(L"scale-200") != std::wstring::npos) rank = 3;
        else if (name.find(L"scale-100") != std::wstring::npos) rank = 4;
        if (name.find(L"contrast") != std::wstring::npos) rank += 1000;
        if (rank < bestRank) {
            bestRank = rank;
            best = folder + fd.cFileName;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return best;
}

// Scales an image file to a small icon with high-quality filtering (keeps alpha).
HICON IconFromImage(const std::wstring& path) {
    Gdiplus::Bitmap src(path.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok || !src.GetWidth()) return nullptr;
    const int size = SmallIconSize();
    Gdiplus::Bitmap dst(size, size, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&dst);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        // Fit, keeping the aspect ratio.
        const float w = float(src.GetWidth()), h = float(src.GetHeight());
        const float s = float(size) / std::max(w, h);
        g.DrawImage(&src, (float(size) - w * s) / 2, (float(size) - h * s) / 2, w * s, h * s);
    }
    HICON icon = nullptr;
    dst.GetHICON(&icon);
    return icon;
}

HICON ShellFileIcon(const std::wstring& path) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof info, SHGFI_ICON | SHGFI_SMALLICON)) return info.hIcon;
    return nullptr;
}

HICON Resolve(std::wstring loc) {
    // A quoted path may be followed by arguments or ",index": keep the path and any index.
    if (!loc.empty() && loc.front() == L'"') {
        const size_t close = loc.find(L'"', 1);
        if (close != std::wstring::npos) {
            std::wstring rest = loc.substr(close + 1);
            const size_t c = rest.find(L',');
            loc = loc.substr(1, close - 1) + (c != std::wstring::npos ? rest.substr(c) : L"");
        }
    } else {
        // Unquoted command lines (C:\x\unins000.exe /S): cut after ".exe".
        const size_t exe = ToLower(loc).find(L".exe ");
        if (exe != std::wstring::npos) loc.resize(exe + 4);
    }
    // Trim whitespace and quotes: DisplayIcon values come in every shape.
    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'"')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'"')) s.pop_back();
    };
    trim(loc);
    if (loc.empty()) return nullptr;
    if (loc.rfind(L"file:", 0) == 0) {
        wchar_t path[MAX_PATH * 2];
        DWORD len = ARRAYSIZE(path);
        if (FAILED(PathCreateFromUrlW(loc.c_str(), path, &len, 0))) return nullptr;
        loc = path;
    }
    int index = 0;
    const size_t comma = loc.find_last_of(L',');
    if (comma != std::wstring::npos && comma > 0) {
        std::wstring num = loc.substr(comma + 1);
        trim(num);
        wchar_t* end = nullptr;
        const long v = wcstol(num.c_str(), &end, 10);
        if (!num.empty() && end && *end == 0) {
            index = static_cast<int>(v);
            loc.resize(comma);
            trim(loc);
        }
    }
    loc = Expand(loc);
    const std::wstring lower = ToLower(loc);
    auto ends = [&](const wchar_t* ext) {
        const size_t n = wcslen(ext);
        return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
    };
    if (ends(L".png") || ends(L".jpg") || ends(L".bmp")) {
        const std::wstring png = ResolvePng(loc);
        return png.empty() ? nullptr : IconFromImage(png);
    }
    if (GetFileAttributesW(loc.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    if (ends(L".exe") || ends(L".dll") || ends(L".ico") || ends(L".icl") || ends(L".cpl") || ends(L".ocx")) {
        HICON smallIcon = nullptr;  // not "small": the RPC headers #define that
        const int size = SmallIconSize();
        if (SUCCEEDED(SHDefExtractIconW(loc.c_str(), index, 0, nullptr, &smallIcon, MAKELONG(size * 2, size))) && smallIcon)
            return smallIcon;
    }
    return ShellFileIcon(loc);
}

HICON Cached(const std::wstring& key, const std::function<HICON()>& make) {
    {
        std::lock_guard lock(g_locMutex);
        auto it = g_byLocation.find(key);
        if (it != g_byLocation.end()) return it->second ? it->second : DefaultIcon();
    }
    HICON icon = make();  // outside the lock: shell lookups can be slow
    std::lock_guard lock(g_locMutex);
    auto [it, inserted] = g_byLocation.emplace(key, icon);
    if (!inserted && icon && icon != it->second) DestroyIcon(icon);  // another thread won the race
    return it->second ? it->second : DefaultIcon();
}

}  // namespace

HICON GenericAppIcon() { return DefaultIcon(); }

HICON IconFromLocation(const std::wstring& location) {
    if (location.empty()) return DefaultIcon();
    return Cached(L"L|" + ToLower(location), [&] { return Resolve(location); });
}

HICON FileIcon(const std::wstring& path) {
    if (path.empty()) return DefaultIcon();
    return Cached(L"F|" + ToLower(path), [&] { return ShellFileIcon(Expand(path)); });
}

}  // namespace mtm
