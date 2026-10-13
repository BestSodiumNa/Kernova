#include "apps.h"
#include "icons.h"
#include "page.h"
#include "table.h"
#include "theme.h"

#include <shellapi.h>

#include <objbase.h>

#include <thread>

namespace mtm {
namespace {

void RunUninstaller(const InstalledApp& app) {
    if (app.isStoreApp) {
        ShellExecuteW(g_main, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (app.uninstall.empty()) {
        ShowError(L"This app doesn't register an uninstaller.");
        return;
    }
    if (!Confirm(Format(L"Run the uninstaller for \"%s\"?\n\n%s", app.name.c_str(), app.uninstall.c_str()))) return;
    std::vector<wchar_t> cmd(app.uninstall.begin(), app.uninstall.end());
    cmd.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_ELEVATION_REQUIRED) {
        // Let Windows show the UAC prompt by launching through cmd with "runas".
        const std::wstring params = L"/c \"" + app.uninstall + L"\"";
        SHELLEXECUTEINFOW sei{};
        sei.cbSize = sizeof sei;
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = params.c_str();
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei)) return;
    }
    ShowError(L"Couldn't start the uninstaller: " + ErrorText(err == ERROR_ELEVATION_REQUIRED ? GetLastError() : err));
}

class InstalledAppsPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Installed apps"; }

    void Create() override {
        search_ = CreateEdit(L"", L"Search name or publisher");
        refresh_ = CreateButton(L"  Refresh");
        summary_ = CreateLabel(L"", SS_CENTERIMAGE);
        table_.Create({{L"Name", 300},
                       {L"Publisher", 200},
                       {L"Version", 120},
                       {L"Installed", 95},
                       {L"Size", 90, true, -(1024.0 * 1024 * 1024)},
                       {L"Source", 150},
                       {L"Location", 300}},
                      4, true);
    }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(120);
        Place(search_, a.left, a.top, Scale(280), h);
        Place(refresh_, a.right - btn, a.top, btn, h);
        Place(summary_, a.left + Scale(296), a.top, a.right - a.left - Scale(296) - btn - Scale(8), h);
        const int top = a.top + h + Scale(14);
        Place(table_.Handle(), a.left, top, a.right - a.left, a.bottom - top);
    }

    void Show(bool v) override { ShowAll({search_, refresh_, summary_, table_.Handle()}, v); }

    void OnActivate() override {
        if (!loaded_) Load();
    }

    bool OnCommand(HWND control, WORD code) override {
        if (control == refresh_ && code == BN_CLICKED) {
            Load();
            return true;
        }
        if (control == search_ && code == EN_CHANGE) {
            Rebuild();
            return true;
        }
        return false;
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (table_.HandleNotify(nm, result)) return true;
        return false;
    }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* row = table_.SelectedRow();
        if (!row) return true;
        const InstalledApp app = apps_[static_cast<size_t>(row->tag)];
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, app.isStoreApp ? L"Uninstall (opens Settings)…" : L"Uninstall…");
        AppendMenuW(menu, MF_STRING | (app.location.empty() ? MF_GRAYED : 0), 2, L"Open install location");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Copy name");
        AppendMenuW(menu, MF_STRING | (app.uninstall.empty() ? MF_GRAYED : 0), 4, L"Copy uninstall command");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) RunUninstaller(app);
        else if (cmd == 2) ShowInExplorer(app.location, false);
        else if (cmd == 3) CopyToClipboard(app.name);
        else if (cmd == 4) CopyToClipboard(app.uninstall);
        return true;
    }

private:
    void Load() {
        if (loading_) return;
        loading_ = true;
        loaded_ = true;
        EnableWindow(refresh_, FALSE);
        SetText(summary_, L"Reading installed apps…");
        worker_ = std::jthread([this](std::stop_token) {
            std::vector<InstalledApp> apps;
            std::set<std::wstring> keyNames;
            CollectRegistryApps(apps, keyNames);
            CollectMsiApps(apps, keyNames);
            CollectStoreApps(apps);
            // Icons are looked up here, off the UI thread (a few hundred shell lookups).
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            for (InstalledApp& a : apps) a.icon = IconFromLocation(a.iconLocation);
            if (SUCCEEDED(com)) CoUninitialize();
            PostToUi([this, apps = std::move(apps)]() mutable {
                apps_ = std::move(apps);
                loading_ = false;
                EnableWindow(refresh_, TRUE);
                Rebuild();
            });
        });
    }

    void Rebuild() {
        const std::wstring needle = ToLower(GetText(search_));
        std::vector<Row> rows;
        uint64_t total = 0;
        for (size_t i = 0; i < apps_.size(); ++i) {
            const auto& a = apps_[i];
            if (!needle.empty() && ToLower(a.name).find(needle) == std::wstring::npos &&
                ToLower(a.publisher).find(needle) == std::wstring::npos)
                continue;
            total += a.sizeBytes;
            Row r;
            r.tag = i;
            r.icon = a.icon ? a.icon : GenericAppIcon();
            r.Add(a.name)
                .Add(a.publisher)
                .Add(a.version)
                .Add(a.installDate)
                .Add(a.sizeBytes ? FormatBytes(a.sizeBytes) : L"", double(a.sizeBytes))
                .Add(a.source)
                .Add(a.location);
            rows.push_back(std::move(r));
        }
        const size_t shown = rows.size();
        table_.SetRows(std::move(rows));
        SetText(summary_, Format(L"%zu of %zu apps · %s reported size · right-click to uninstall", shown, apps_.size(),
                                 FormatBytes(total).c_str()));
    }

    HWND search_ = nullptr, refresh_ = nullptr, summary_ = nullptr;
    Table table_;
    std::vector<InstalledApp> apps_;
    bool loaded_ = false, loading_ = false;
    std::jthread worker_;  // last member: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeInstalledAppsPage() { return std::make_unique<InstalledAppsPage>(); }

}  // namespace mtm
