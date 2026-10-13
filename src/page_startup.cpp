#include "icons.h"
#include "page.h"
#include "table.h"
#include "theme.h"

#include <knownfolders.h>
#include <shlobj.h>

namespace mtm {
namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedBase[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";

struct StartupItem {
    std::wstring name, command, source, filePath;
    HKEY approvedRoot = nullptr;
    std::wstring approvedKey;   // ...\StartupApproved\Run, Run32 or StartupFolder
    std::wstring approvedValue; // value name inside it
    bool enabled = true;
};

// Explorer stores 12 bytes per item; an odd first byte means "disabled".
bool ReadEnabled(HKEY root, const std::wstring& key, const std::wstring& value) {
    BYTE data[12] = {};
    DWORD size = sizeof data;
    if (RegGetValueW(root, key.c_str(), value.c_str(), RRF_RT_REG_BINARY | RRF_SUBKEY_WOW6464KEY, nullptr, data,
                     &size) != ERROR_SUCCESS)
        return true;
    return (data[0] & 1) == 0;
}

DWORD WriteEnabled(const StartupItem& item, bool enable) {
    BYTE data[12] = {};
    data[0] = enable ? 0x02 : 0x03;
    if (!enable) {
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        memcpy(data + 4, &now, sizeof now);
    }
    HKEY key = nullptr;
    LSTATUS s = RegCreateKeyExW(item.approvedRoot, item.approvedKey.c_str(), 0, nullptr, 0,
                                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr);
    if (s != ERROR_SUCCESS) return static_cast<DWORD>(s);
    s = RegSetValueExW(key, item.approvedValue.c_str(), 0, REG_BINARY, data, sizeof data);
    RegCloseKey(key);
    return static_cast<DWORD>(s);
}

void ReadRunKey(HKEY root, REGSAM view, const wchar_t* source, const wchar_t* approved, std::vector<StartupItem>& out) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, kRunKey, 0, KEY_READ | view, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0;; ++i) {
        wchar_t name[16384];
        DWORD nameLen = ARRAYSIZE(name);
        DWORD type = 0, dataLen = 0;
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, nullptr, &dataLen) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        std::wstring data(dataLen / sizeof(wchar_t) + 1, L'\0');
        nameLen = ARRAYSIZE(name);
        DWORD bytes = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        if (RegEnumValueW(key, i, name, &nameLen, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &bytes) !=
            ERROR_SUCCESS)
            continue;
        data.resize(wcsnlen(data.c_str(), data.size()));
        StartupItem item;
        item.name = name;
        item.command = data;
        item.source = source;
        item.approvedRoot = root;
        item.approvedKey = std::wstring(kApprovedBase) + approved;
        item.approvedValue = name;
        item.enabled = ReadEnabled(root, item.approvedKey, name);
        out.push_back(std::move(item));
    }
    RegCloseKey(key);
}

void ReadStartupFolder(REFKNOWNFOLDERID id, HKEY approvedRoot, const wchar_t* source, std::vector<StartupItem>& out) {
    PWSTR folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &folder))) return;
    const std::wstring dir = folder;
    CoTaskMemFree(folder);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (_wcsicmp(fd.cFileName, L"desktop.ini") == 0) continue;
        StartupItem item;
        item.name = fd.cFileName;
        item.filePath = dir + L"\\" + fd.cFileName;
        item.command = item.filePath;
        item.source = source;
        item.approvedRoot = approvedRoot;
        item.approvedKey = std::wstring(kApprovedBase) + L"StartupFolder";
        item.approvedValue = fd.cFileName;
        item.enabled = ReadEnabled(approvedRoot, item.approvedKey, item.approvedValue);
        out.push_back(std::move(item));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// First token of a command line: the quoted part, or up to the first ".exe".
std::wstring ExecutableOf(const std::wstring& command) {
    wchar_t expanded[32768];
    if (!ExpandEnvironmentStringsW(command.c_str(), expanded, ARRAYSIZE(expanded))) return L"";
    std::wstring c = expanded;
    if (!c.empty() && c[0] == L'"') {
        const size_t end = c.find(L'"', 1);
        return c.substr(1, end == std::wstring::npos ? std::wstring::npos : end - 1);
    }
    const std::wstring lower = ToLower(c);
    const size_t exe = lower.find(L".exe");
    if (exe != std::wstring::npos) return c.substr(0, exe + 4);
    const size_t space = c.find(L' ');
    return c.substr(0, space);
}


// File whose icon represents a startup item. Squirrel apps (Discord, Teams, Slack...)
// start through "Update.exe --processStart App.exe"; use the newest app-x.y.z\App.exe.
std::wstring IconPathFor(const StartupItem& item) {
    if (!item.filePath.empty()) return item.filePath;
    const std::wstring exe = ExecutableOf(item.command);
    const std::wstring lower = ToLower(item.command);
    const size_t ps = lower.find(L"--processstart");
    if (ps == std::wstring::npos || ToLower(exe).find(L"update.exe") == std::wstring::npos) return exe;
    std::wstring target = item.command.substr(ps + 14);
    while (!target.empty() && (target.front() == L' ' || target.front() == L'"')) target.erase(target.begin());
    target = target.substr(0, target.find_first_of(L" \""));
    const std::wstring dir = exe.substr(0, exe.find_last_of(L'\\'));
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\app-*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
    std::wstring latest;
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring(fd.cFileName) > latest) latest = fd.cFileName;
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    const std::wstring candidate = dir + L"\\" + latest + L"\\" + target;
    return !latest.empty() && GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES ? candidate : exe;
}
class StartupPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Startup apps"; }

    void Create() override {
        summary_ = CreateLabel(L"", SS_CENTERIMAGE);
        table_.Create({{L"Name", 220}, {L"Status", 80}, {L"Command", 480}, {L"Source", 200}}, 0, false);
    }

    void Layout(RECT a) override {
        const int h = Scale(24);
        Place(summary_, a.left, a.top, a.right - a.left, h);
        Place(table_.Handle(), a.left, a.top + h + Scale(6), a.right - a.left, a.bottom - a.top - h - Scale(6));
    }

    void Show(bool v) override { ShowAll({summary_, table_.Handle()}, v); }
    void OnActivate() override { Refresh(); }
    bool OnNotify(NMHDR* nm, LRESULT& result) override { return table_.HandleNotify(nm, result); }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* row = table_.SelectedRow();
        if (!row) return true;
        const StartupItem item = items_[static_cast<size_t>(row->tag)];
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, item.enabled ? L"Disable" : L"Enable");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 2, L"Open file location");
        AppendMenuW(menu, MF_STRING, 3, L"Copy command");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) {
            if (const DWORD err = WriteEnabled(item, !item.enabled))
                ShowError(Format(L"Couldn't change \"%s\": %s%s", item.name.c_str(), ErrorText(err).c_str(),
                                 item.approvedRoot == HKEY_LOCAL_MACHINE ? L"\n\nSystem-wide items need administrator rights." : L""));
            Refresh();
        } else if (cmd == 2) {
            const std::wstring path = item.filePath.empty() ? ExecutableOf(item.command) : item.filePath;
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) ShowError(L"Couldn't find the file: " + path);
            else ShowInExplorer(path, true);
        } else if (cmd == 3) {
            CopyToClipboard(item.command);
        }
        return true;
    }

private:
    void Refresh() {
        items_.clear();
        ReadRunKey(HKEY_CURRENT_USER, 0, L"Registry: current user", L"Run", items_);
        ReadRunKey(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, L"Registry: all users", L"Run", items_);
        ReadRunKey(HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY, L"Registry: all users (32-bit)", L"Run32", items_);
        ReadStartupFolder(FOLDERID_Startup, HKEY_CURRENT_USER, L"Startup folder: current user", items_);
        ReadStartupFolder(FOLDERID_CommonStartup, HKEY_LOCAL_MACHINE, L"Startup folder: all users", items_);
        std::vector<Row> rows;
        size_t enabled = 0;
        for (size_t i = 0; i < items_.size(); ++i) {
            const auto& it = items_[i];
            if (it.enabled) ++enabled;
            Row r;
            r.tag = i;
            r.accent = it.enabled ? CLR_NONE : g_pal.textFaint;
            r.icon = FileIcon(IconPathFor(it));
            r.Add(it.name).Add(it.enabled ? L"Enabled" : L"Disabled").Add(it.command).Add(it.source);
            rows.push_back(std::move(r));
        }
        table_.SetRows(std::move(rows));
        SetText(summary_, Format(L"%zu startup items · %zu enabled · right-click to enable or disable", items_.size(), enabled));
    }

    HWND summary_ = nullptr;
    Table table_;
    std::vector<StartupItem> items_;
};

}  // namespace

std::unique_ptr<Page> MakeStartupPage() { return std::make_unique<StartupPage>(); }

}  // namespace mtm
