#include "page.h"
#include "table.h"
#include "theme.h"

#include <shellapi.h>

#include <thread>
#include <unordered_map>

namespace mtm {
namespace {

const wchar_t* StateName(DWORD s) {
    switch (s) {
    case SERVICE_STOPPED: return L"Stopped";
    case SERVICE_START_PENDING: return L"Starting";
    case SERVICE_STOP_PENDING: return L"Stopping";
    case SERVICE_RUNNING: return L"Running";
    case SERVICE_CONTINUE_PENDING: return L"Continuing";
    case SERVICE_PAUSE_PENDING: return L"Pausing";
    case SERVICE_PAUSED: return L"Paused";
    default: return L"?";
    }
}

const wchar_t* StartTypeName(DWORD t) {
    switch (t) {
    case SERVICE_BOOT_START: return L"Boot";
    case SERVICE_SYSTEM_START: return L"System";
    case SERVICE_AUTO_START: return L"Automatic";
    case SERVICE_DEMAND_START: return L"Manual";
    case SERVICE_DISABLED: return L"Disabled";
    default: return L"";
    }
}

struct ServiceEntry {
    std::wstring name, display;
    DWORD state = 0, pid = 0;
};

std::vector<ServiceEntry> EnumerateServices(SC_HANDLE scm) {
    std::vector<ServiceEntry> out;
    DWORD resume = 0;
    std::vector<BYTE> buf(64 * 1024);
    for (;;) {
        DWORD needed = 0, count = 0;
        const BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(),
                                              static_cast<DWORD>(buf.size()), &needed, &count, &resume, nullptr);
        const DWORD err = ok ? 0 : GetLastError();
        if (!ok && err != ERROR_MORE_DATA) break;
        const auto* items = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            out.push_back({items[i].lpServiceName, items[i].lpDisplayName,
                           items[i].ServiceStatusProcess.dwCurrentState, items[i].ServiceStatusProcess.dwProcessId});
        }
        if (ok) break;
        if (needed > buf.size()) buf.resize(needed);
    }
    return out;
}

// Runs a start/stop/restart on a worker thread, since stopping can take seconds.
void ControlServiceAsync(std::wstring name, int action, std::function<void()> done) {
    std::thread([name = std::move(name), action, done = std::move(done)] {
        std::wstring error;
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE svc = scm ? OpenServiceW(scm, name.c_str(), SERVICE_START | SERVICE_STOP | SERVICE_QUERY_STATUS) : nullptr;
        if (!svc) {
            error = ErrorText(GetLastError());
        } else {
            SERVICE_STATUS st{};
            if (action != 0) {  // stop (1) or restart (2)
                if (!ControlService(svc, SERVICE_CONTROL_STOP, &st) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
                    error = ErrorText(GetLastError());
                } else {
                    for (int i = 0; i < 60 && QueryServiceStatus(svc, &st) && st.dwCurrentState != SERVICE_STOPPED; ++i)
                        Sleep(250);
                }
            }
            if (error.empty() && action != 1 && !StartServiceW(svc, 0, nullptr) &&
                GetLastError() != ERROR_SERVICE_ALREADY_RUNNING)
                error = ErrorText(GetLastError());
            CloseServiceHandle(svc);
        }
        if (scm) CloseServiceHandle(scm);
        PostToUi([name, error, done] {
            if (!error.empty())
                ShowError(Format(L"Couldn't control service \"%s\": %s\n\nMost services need administrator rights (File > Restart as administrator).",
                                 name.c_str(), error.c_str()));
            done();
        });
    }).detach();
}

class ServicesPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Services"; }

    void Create() override {
        summary_ = CreateLabel(L"", SS_CENTERIMAGE);
        table_.Create({{L"Name", 200}, {L"Display name", 320}, {L"Status", 90}, {L"PID", 70, true}, {L"Start type", 100}},
                      0, false);
    }

    void Layout(RECT a) override {
        const int h = Scale(24);
        Place(summary_, a.left, a.top, a.right - a.left, h);
        Place(table_.Handle(), a.left, a.top + h + Scale(6), a.right - a.left, a.bottom - a.top - h - Scale(6));
    }

    void Show(bool v) override { ShowAll({summary_, table_.Handle()}, v); }
    void OnActivate() override { Refresh(); }
    void OnTick(bool visible) override {
        if (visible && ++ticks_ % 3 == 0) Refresh();
    }
    bool OnNotify(NMHDR* nm, LRESULT& result) override { return table_.HandleNotify(nm, result); }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* row = table_.SelectedRow();
        if (!row) return true;
        const std::wstring name = row->cells[0];
        const bool running = row->cells[2] == L"Running";
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING | (running ? MF_GRAYED : 0), 1, L"Start");
        AppendMenuW(menu, MF_STRING | (running ? 0 : MF_GRAYED), 2, L"Stop");
        AppendMenuW(menu, MF_STRING | (running ? 0 : MF_GRAYED), 3, L"Restart");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 4, L"Open Services console");
        AppendMenuW(menu, MF_STRING, 5, L"Copy name");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        if (cmd >= 1 && cmd <= 3) {
            if (cmd != 1 && !Confirm(Format(L"%s the service \"%s\"?", cmd == 2 ? L"Stop" : L"Restart", name.c_str()))) return true;
            ControlServiceAsync(name, cmd == 1 ? 0 : (cmd == 2 ? 1 : 2), [this] { Refresh(); });
        } else if (cmd == 4) {
            ShellExecuteW(g_main, L"open", L"services.msc", nullptr, nullptr, SW_SHOWNORMAL);
        } else if (cmd == 5) {
            CopyToClipboard(name);
        }
        return true;
    }

private:
    std::wstring StartType(SC_HANDLE scm, const std::wstring& name) {
        auto it = startTypes_.find(name);
        if (it != startTypes_.end()) return it->second;
        std::wstring result;
        if (SC_HANDLE svc = OpenServiceW(scm, name.c_str(), SERVICE_QUERY_CONFIG)) {
            DWORD needed = 0;
            QueryServiceConfigW(svc, nullptr, 0, &needed);
            std::vector<BYTE> buf(needed ? needed : 1);
            auto* cfg = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buf.data());
            if (needed && QueryServiceConfigW(svc, cfg, needed, &needed)) result = StartTypeName(cfg->dwStartType);
            CloseServiceHandle(svc);
        }
        startTypes_[name] = result;
        return result;
    }

    void Refresh() {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
        if (!scm) {
            SetText(summary_, L"Couldn't open the Service Control Manager: " + ErrorText(GetLastError()));
            return;
        }
        const auto services = EnumerateServices(scm);
        std::vector<Row> rows;
        size_t running = 0;
        for (const auto& s : services) {
            if (s.state == SERVICE_RUNNING) ++running;
            Row r;
            r.tag = std::hash<std::wstring>{}(s.name);
            if (s.state == SERVICE_RUNNING) r.accent = g_pal.good;
            r.Add(s.name)
                .Add(s.display)
                .Add(StateName(s.state))
                .Add(s.pid ? std::to_wstring(s.pid) : L"", s.pid)
                .Add(StartType(scm, s.name));
            rows.push_back(std::move(r));
        }
        CloseServiceHandle(scm);
        table_.SetRows(std::move(rows));
        SetText(summary_, Format(L"%zu services · %zu running · right-click to start, stop or restart", services.size(), running));
    }

    HWND summary_ = nullptr;
    Table table_;
    std::unordered_map<std::wstring, std::wstring> startTypes_;
    unsigned ticks_ = 0;
};

}  // namespace

std::unique_ptr<Page> MakeServicesPage() { return std::make_unique<ServicesPage>(); }

}  // namespace mtm
