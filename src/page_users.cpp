#include "page.h"
#include "processes.h"
#include "table.h"
#include "theme.h"

#include <wtsapi32.h>

#include <unordered_map>

namespace mtm {
namespace {

// (IsElevated() comes from common.h.)

// ---------------------------------------------------------------------------
// WTS helpers
// ---------------------------------------------------------------------------
const wchar_t* StateName(WTS_CONNECTSTATE_CLASS s) {
    switch (s) {
    case WTSActive:       return L"Active";
    case WTSConnected:    return L"Connected";
    case WTSConnectQuery: return L"Connecting";
    case WTSShadow:       return L"Shadowing";
    case WTSDisconnected: return L"Disconnected";
    case WTSIdle:         return L"Idle";
    case WTSListen:       return L"Listening";
    case WTSReset:        return L"Resetting";
    case WTSDown:         return L"Down";
    case WTSInit:         return L"Initializing";
    default:              return L"?";
    }
}

// WTSQuerySessionInformationW allocates a NUL-terminated UTF-16 buffer for
// the string classes we use. Guard against the degenerate bytes == 0 case so
// we never construct a wstring from an empty allocation.
std::wstring SessionString(DWORD id, WTS_INFO_CLASS cls) {
    LPWSTR buf = nullptr;
    DWORD bytes = 0;
    std::wstring s;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, id, cls,
                                    &buf, &bytes) &&
        buf && bytes >= sizeof(wchar_t)) {
        s.assign(buf);   // reads up to the terminating NUL
    }
    if (buf) WTSFreeMemory(buf);
    return s;
}

// ---------------------------------------------------------------------------
// UsersPage
// ---------------------------------------------------------------------------
class UsersPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Users"; }

    void Create() override {
        summary_    = CreateLabel(L"", SS_CENTERIMAGE);
        disconnect_ = CreateButton(L"  Disconnect");
        signOut_    = CreateButton(L"  Sign out", true);
        table_.Create({{L"User", 220},
                       {L"Session", 80, true},
                       {L"State", 110},
                       {L"Client", 140},
                       {L"Processes", 95, true},
                       {L"CPU", 80, true, -10},
                       {L"Memory", 110, true, -(1024.0 * 1024 * 1024)}},
                      1, false);
    }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(140), gap = Scale(8);
        Place(signOut_,    a.right - btn,             a.top, btn, h);
        Place(disconnect_, a.right - 2 * btn - gap,   a.top, btn, h);
        Place(summary_,    a.left,                    a.top,
              a.right - a.left - 2 * btn - 2 * gap,   h);
        const int top = a.top + h + Scale(14);
        Place(table_.Handle(), a.left, top,
              a.right - a.left, a.bottom - top);
    }

    void Show(bool v) override {
        ShowAll({summary_, disconnect_, signOut_, table_.Handle()}, v);
    }

    void OnActivate() override {
        UpdateButtonState();
        Refresh();
    }

    void OnTick(bool visible) override {
        if (visible) Refresh();
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        return table_.HandleNotify(nm, result);
    }

    bool OnCommand(HWND control, WORD code) override {
        if (code != BN_CLICKED) return false;
        if (control != disconnect_ && control != signOut_) return false;
        Act(control == signOut_);
        return true;
    }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;

        const Row* row = table_.SelectedRow();
        if (!row) return true;
        // TrackPopupMenu pumps messages: a timer tick can replace the rows while
        // the menu is open, so copy what we need now instead of keeping `row`.
        const std::wstring userName = row->cells[0];

        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);

        const bool elevated = IsElevated();
        HMENU menu = CreatePopupMenu();

        // Destructive items are disabled rather than hidden, so the user
        // sees *why* they can't act instead of a mysteriously short menu.
        AppendMenuW(menu, MF_STRING | (elevated ? 0 : MF_GRAYED), 1, L"Disconnect");
        AppendMenuW(menu, MF_STRING | (elevated ? 0 : MF_GRAYED), 2, L"Sign out");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, 3, L"Copy user name");

        // Required so the popup dismisses correctly when clicking elsewhere.
        SetForegroundWindow(g_main);
        const int cmd = TrackPopupMenu(menu,
                                       TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                       pt.x, pt.y, 0, g_main, nullptr);
        // MSDN: post a benign message after TrackPopupMenu, or the menu can
        // fail to close on the next click.
        PostMessage(g_main, WM_NULL, 0, 0);
        DestroyMenu(menu);

        if (cmd == 1 || cmd == 2)      Act(cmd == 2);
        else if (cmd == 3)             CopyToClipboard(userName);
        return true;
    }

private:
    // Grey out the buttons when we know the action cannot succeed. This
    // avoids the three-confusing-error-dialogs-per-click problem.
    void UpdateButtonState() {
        const bool elevated = IsElevated();
        EnableWindow(disconnect_, elevated);
        EnableWindow(signOut_,    elevated);
    }

    void Act(bool signOut) {
        const Row* row = table_.SelectedRow();
        if (!row) return;

        const DWORD id = static_cast<DWORD>(row->tag);

        // Logging off your own session terminates this process before the
        // call returns, so refuse up front instead of after confirming.
        DWORD mySession = 0;
        ProcessIdToSessionId(GetCurrentProcessId(), &mySession);
        if (signOut && id == mySession) {
            ShowError(L"You cannot sign out your own session from here. "
                      L"Use the Start menu instead.");
            return;
        }

        const std::wstring warning = signOut
            ? L"\n\nAll of this user's apps will close and unsaved work will be lost."
            : L"";
        if (!Confirm(Format(L"%s %s (session %lu)?%s",
                            signOut ? L"Sign out" : L"Disconnect",
                            row->cells[0].c_str(),
                            static_cast<unsigned long>(id),
                            warning.c_str())))
            return;

        const BOOL ok = signOut
            ? WTSLogoffSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE)
            : WTSDisconnectSession(WTS_CURRENT_SERVER_HANDLE, id, FALSE);

        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_ACCESS_DENIED) {
                ShowError(signOut
                    ? L"Signing out another user's session requires "
                      L"administrator rights."
                    : L"Disconnecting this session requires "
                      L"administrator rights.");
            } else {
                ShowError(L"Couldn't change the session: " + ErrorText(err));
            }
        }
        Refresh();
    }

    void Refresh() {
        struct Totals {
            size_t   count = 0;
            double   cpu   = 0;
            uint64_t mem   = 0;
        };

        // (Table::SetRows keeps the selection by tag, i.e. by session id.)
        std::unordered_map<ULONG, Totals> bySession;
        for (const ProcInfo& p : g_procs.Processes()) {
            Totals& t = bySession[p.session];
            ++t.count;
            t.cpu += p.cpu;
            t.mem += p.privateWs;
        }

        WTS_SESSION_INFOW* sessions = nullptr;
        DWORD count = 0;
        std::vector<Row> rows;
        rows.reserve(8);

        if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1,
                                  &sessions, &count)) {
            for (DWORD i = 0; i < count; ++i) {
                const DWORD id = sessions[i].SessionId;
                const std::wstring user = SessionString(id, WTSUserName);
                if (user.empty()) continue;   // services + listener sessions

                const std::wstring domain = SessionString(id, WTSDomainName);
                const Totals& t = bySession[id];   // default if missing

                Row r;
                r.tag    = id;
                r.accent = sessions[i].State == WTSActive ? g_pal.good : CLR_NONE;
                r.Add(domain.empty() ? user : domain + L"\\" + user)
                 .Add(std::to_wstring(id), id)
                 .Add(StateName(sessions[i].State))
                 .Add(SessionString(id, WTSClientName))
                 .Add(FormatCount(t.count), double(t.count))
                 .Add(Format(L"%.1f%%", t.cpu), t.cpu)
                 .Add(FormatBytes(t.mem), double(t.mem));
                rows.push_back(std::move(r));
            }
            WTSFreeMemory(sessions);
        }

        const size_t n = rows.size();
        table_.SetRows(std::move(rows));

        SetText(summary_,
                Format(L"%zu signed-in user%s  ·  totals include every "
                       L"process in the session",
                       n, n == 1 ? L"" : L"s"));
    }

    HWND summary_    = nullptr;
    HWND disconnect_ = nullptr;
    HWND signOut_    = nullptr;
    Table table_;
};

}  // namespace

std::unique_ptr<Page> MakeUsersPage() {
    return std::make_unique<UsersPage>();
}

}  // namespace mtm