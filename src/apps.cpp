#include "apps.h"

#include <msi.h>

namespace mtm {
namespace {

constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";

std::wstring ReadString(HKEY key, const wchar_t* value) {
    DWORD size = 0;
    if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < 2) return L"";
    std::wstring s(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, nullptr, value, RRF_RT_REG_SZ, nullptr, s.data(), &size) != ERROR_SUCCESS) return L"";
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

DWORD ReadDword(HKEY key, const wchar_t* value, DWORD fallback = 0) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(key, nullptr, value, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : fallback;
}

// "20240131" -> "2024-01-31". Some installers write YYYYDDMM; swap when the month is impossible.
std::wstring NiceDate(const std::wstring& d) {
    if (d.size() != 8 || d.find_first_not_of(L"0123456789") != std::wstring::npos) return d;
    std::wstring month = d.substr(4, 2), day = d.substr(6, 2);
    if (std::stoi(month) > 12 && std::stoi(day) <= 12) std::swap(month, day);
    return d.substr(0, 4) + L"-" + month + L"-" + day;
}

std::wstring MsiProp(const wchar_t* code, const wchar_t* sid, MSIINSTALLCONTEXT ctx, const wchar_t* prop) {
    wchar_t buf[1024];
    DWORD len = ARRAYSIZE(buf);
    return MsiGetProductInfoExW(code, sid, ctx, prop, buf, &len) == ERROR_SUCCESS ? std::wstring(buf, len) : L"";
}

// First .exe in `folder` (not recursive), preferring one named like the app and
// skipping uninstallers / updaters.
std::wstring ExeIn(const std::wstring& folder, const std::wstring& appName) {
    if (folder.empty()) return L"";
    std::wstring dir = folder;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'"')) dir.pop_back();
    if (!dir.empty() && dir.front() == L'"') dir.erase(dir.begin());
    const std::wstring first = ToLower(appName.substr(0, appName.find(L' ')));
    std::wstring any, named;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*.exe").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return L"";
    do {
        const std::wstring name = ToLower(fd.cFileName);
        if (name.find(L"unins") != std::wstring::npos || name.find(L"update") != std::wstring::npos ||
            name.find(L"setup") != std::wstring::npos || name.find(L"crash") != std::wstring::npos)
            continue;
        if (any.empty()) any = dir + L"\\" + fd.cFileName;
        if (!first.empty() && name.rfind(first, 0) == 0) {
            named = dir + L"\\" + fd.cFileName;
            break;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    return named.empty() ? any : named;
}

// For entries without DisplayIcon: the MSI product icon, an exe in the install folder,
// a Squirrel app's "app-<version>\" folder (Discord, Teams...), or the uninstaller.
std::wstring GuessIcon(const wchar_t* keyName, const InstalledApp& app) {
    if (keyName[0] == L'{') {
        wchar_t buf[1024];
        DWORD len = ARRAYSIZE(buf);
        if (MsiGetProductInfoW(keyName, INSTALLPROPERTY_PRODUCTICON, buf, &len) == ERROR_SUCCESS && len) return buf;
    }
    if (std::wstring exe = ExeIn(app.location, app.name); !exe.empty()) return exe;
    const std::wstring lower = ToLower(app.uninstall);
    if (lower.find(L"msiexec") != std::wstring::npos) return L"";
    // Squirrel installers: "...\Discord\Update.exe --uninstall"; the real exe sits in app-x.y.z\.
    const size_t upd = lower.find(L"\\update.exe");
    if (upd != std::wstring::npos) {
        std::wstring dir = app.uninstall.substr(0, upd);
        if (!dir.empty() && dir.front() == L'"') dir.erase(dir.begin());
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((dir + L"\\app-*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
        std::wstring latest;
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring(fd.cFileName) > latest) latest = fd.cFileName;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        if (!latest.empty())
            if (std::wstring exe = ExeIn(dir + L"\\" + latest, app.name); !exe.empty()) return exe;
    }
    return app.uninstall;  // the uninstaller often carries the app icon
}

}  // namespace

void CollectRegistryApps(std::vector<InstalledApp>& out, std::set<std::wstring>& keyNames) {
    struct View {
        HKEY root;
        REGSAM sam;
        const wchar_t* label;
    };
    const View views[] = {
        {HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"All users"},
        {HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"All users (32-bit)"},
        {HKEY_CURRENT_USER, KEY_WOW64_64KEY, L"Current user"},
        {HKEY_CURRENT_USER, KEY_WOW64_32KEY, L"Current user"},
    };
    // HKCU\Software isn't split into 32/64-bit views, so both HKCU passes return the
    // same keys; HKLM's two views are genuinely different keys.
    std::set<std::wstring> seen;
    for (const View& v : views) {
        HKEY root = nullptr;
        if (RegOpenKeyExW(v.root, kUninstallKey, 0, KEY_READ | v.sam, &root) != ERROR_SUCCESS) continue;
        for (DWORD i = 0;; ++i) {
            wchar_t sub[512];
            DWORD subLen = ARRAYSIZE(sub);
            if (RegEnumKeyExW(root, i, sub, &subLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::wstring lowerName = ToLower(sub);
            keyNames.insert(lowerName);
            const std::wstring identity =
                v.root == HKEY_CURRENT_USER ? L"user:" + lowerName : L"machine:" + std::to_wstring(v.sam) + L":" + lowerName;
            if (!seen.insert(identity).second) continue;
            HKEY key = nullptr;
            if (RegOpenKeyExW(root, sub, 0, KEY_READ | v.sam, &key) != ERROR_SUCCESS) continue;
            InstalledApp app;
            app.name = ReadString(key, L"DisplayName");
            const bool hidden = ReadDword(key, L"SystemComponent") == 1 || !ReadString(key, L"ParentKeyName").empty();
            const std::wstring release = ToLower(ReadString(key, L"ReleaseType"));
            const bool isUpdate = release.find(L"update") != std::wstring::npos || release == L"hotfix";
            if (!app.name.empty() && !hidden && !isUpdate) {
                app.publisher = ReadString(key, L"Publisher");
                app.version = ReadString(key, L"DisplayVersion");
                app.installDate = NiceDate(ReadString(key, L"InstallDate"));
                app.location = ReadString(key, L"InstallLocation");
                app.uninstall = ReadString(key, L"UninstallString");
                app.iconLocation = ReadString(key, L"DisplayIcon");
                if (app.iconLocation.empty()) app.iconLocation = GuessIcon(sub, app);
                app.sizeBytes = uint64_t(ReadDword(key, L"EstimatedSize")) * 1024;  // stored in KB
                app.source = v.label;
                out.push_back(std::move(app));
            }
            RegCloseKey(key);
        }
        RegCloseKey(root);
    }
}

void CollectMsiApps(std::vector<InstalledApp>& out, const std::set<std::wstring>& skipProductCodes) {
    for (DWORD i = 0;; ++i) {
        wchar_t code[39] = {};
        wchar_t sid[256] = {};
        DWORD sidLen = ARRAYSIZE(sid);
        MSIINSTALLCONTEXT ctx = MSIINSTALLCONTEXT_NONE;
        const UINT r = MsiEnumProductsExW(nullptr, nullptr, MSIINSTALLCONTEXT_ALL, i, code, &ctx, sid, &sidLen);
        if (r != ERROR_SUCCESS) break;  // ERROR_NO_MORE_ITEMS or an error: stop either way
        if (skipProductCodes.count(ToLower(code))) continue;
        const wchar_t* user = ctx == MSIINSTALLCONTEXT_MACHINE ? nullptr : sid;
        InstalledApp app;
        app.name = MsiProp(code, user, ctx, INSTALLPROPERTY_PRODUCTNAME);
        if (app.name.empty()) continue;
        app.publisher = MsiProp(code, user, ctx, INSTALLPROPERTY_PUBLISHER);
        app.version = MsiProp(code, user, ctx, INSTALLPROPERTY_VERSIONSTRING);
        app.installDate = NiceDate(MsiProp(code, user, ctx, INSTALLPROPERTY_INSTALLDATE));
        app.location = MsiProp(code, user, ctx, INSTALLPROPERTY_INSTALLLOCATION);
        app.uninstall = std::wstring(L"MsiExec.exe /X") + code;
        app.iconLocation = MsiProp(code, user, ctx, INSTALLPROPERTY_PRODUCTICON);
        app.source = L"Windows Installer";
        out.push_back(std::move(app));
    }
}

}  // namespace mtm
