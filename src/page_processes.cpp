#include "icons.h"
#include "page.h"
#include "processes.h"
#include "sysinfo.h"
#include "table.h"
#include "theme.h"

namespace mtm {
namespace {

using NtProcessFn = LONG(NTAPI*)(HANDLE);

const wchar_t* PriorityName(LONG base) {
    if (base >= 24) return L"Realtime";
    if (base >= 13) return L"High";
    if (base >= 10) return L"Above normal";
    if (base >= 8) return L"Normal";
    if (base >= 6) return L"Below normal";
    if (base > 0) return L"Low";
    return L"";
}

std::wstring ImagePath(DWORD pid) {
    std::wstring path;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t buf[32768];
        DWORD size = ARRAYSIZE(buf);
        if (QueryFullProcessImageNameW(h, 0, buf, &size)) path.assign(buf, size);
        CloseHandle(h);
    }
    return path;
}

// Runs fn on an opened process handle and reports failures to the user.
template <typename Fn>
void WithProcess(DWORD pid, DWORD access, const wchar_t* action, Fn fn) {
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (!h) {
        ShowError(Format(L"Couldn't %s (PID %lu): %s", action, pid, ErrorText(GetLastError()).c_str()));
        return;
    }
    const DWORD err = fn(h);
    CloseHandle(h);
    if (err) ShowError(Format(L"Couldn't %s (PID %lu): %s", action, pid, ErrorText(err).c_str()));
}

DWORD CallNt(HANDLE h, const char* name) {
    static HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    auto fn = reinterpret_cast<NtProcessFn>(GetProcAddress(ntdll, name));
    if (!fn) return ERROR_PROC_NOT_FOUND;
    return fn(h) < 0 ? ERROR_ACCESS_DENIED : 0;
}

bool EfficiencyModeOn(DWORD pid) {
    bool on = false;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid)) {
        PROCESS_POWER_THROTTLING_STATE s{};
        s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        if (GetProcessInformation(h, ProcessPowerThrottling, &s, sizeof s))
            on = (s.ControlMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) &&
                 (s.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED);
        CloseHandle(h);
    }
    return on;
}

// The shell's own "Run" dialog (shell32 ordinal 61), the same one Task Manager uses.
void ShowRunDialog() {
    using RunFileDlgFn = void(WINAPI*)(HWND, HICON, LPCWSTR, LPCWSTR, LPCWSTR, UINT);
    static HMODULE shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto fn = shell ? reinterpret_cast<RunFileDlgFn>(GetProcAddress(shell, MAKEINTRESOURCEA(61))) : nullptr;
    if (fn) fn(g_main, nullptr, nullptr, L"Create new task", L"Type the name of a program, folder, document or website.", 0);
}

class ProcessesPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Processes"; }

    void Create() override {
        search_ = CreateEdit(L"", L"Search processes");
        summary_ = CreateLabel(L"", SS_CENTERIMAGE);
        run_ = CreateButton(L"  Run new task");
        end_ = CreateButton(L"  End task", true);
        table_.Create({{L"Name", 230},
                       {L"PID", 70, true},
                       {L"CPU", 75, true, -10},
                       {L"Memory", 100, true, -(512.0 * 1024 * 1024)},
                       {L"GPU", 70, true, -10},
                       {L"I/O", 95, true, -(1024.0 * 1024)},
                       {L"Working set", 105, true},
                       {L"Commit", 100, true},
                       {L"Threads", 75, true},
                       {L"Handles", 80, true},
                       {L"Session", 70, true},
                       {L"Priority", 105}},
                      2, true);
    }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(140), gap = Scale(8);
        Place(search_, a.left, a.top, Scale(280), h);
        Place(end_, a.right - btn, a.top, btn, h);
        Place(run_, a.right - 2 * btn - gap, a.top, btn, h);
        Place(summary_, a.left + Scale(296), a.top, a.right - a.left - Scale(296) - 2 * btn - 3 * gap, h);
        const int top = a.top + h + Scale(14);
        Place(table_.Handle(), a.left, top, a.right - a.left, a.bottom - top);
    }

    void Show(bool visible) override { ShowAll({search_, summary_, run_, end_, table_.Handle()}, visible); }
    void OnActivate() override { Refresh(); }
    void OnTick(bool visible) override {
        if (visible) Refresh();
    }

    bool OnCommand(HWND control, WORD code) override {
        if (control == search_ && code == EN_CHANGE) Refresh();
        else if (control == run_ && code == BN_CLICKED) ShowRunDialog();
        else if (control == end_ && code == BN_CLICKED) EndSelected();
        else return false;
        return true;
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (table_.HandleNotify(nm, result)) return true;
        if (nm->hwndFrom == table_.Handle() && nm->code == LVN_KEYDOWN &&
            reinterpret_cast<NMLVKEYDOWN*>(nm)->wVKey == VK_DELETE) {
            EndSelected();
            return true;
        }
        return false;
    }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* row = table_.SelectedRow();
        if (!row) return true;
        const DWORD pid = static_cast<DWORD>(row->tag);
        const std::wstring name = row->cells[0];
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);

        enum : UINT { kEnd = 1, kSuspend, kResume, kEfficiency, kOpen, kCopyName, kCopyPath, kSearch,
                      kIdle = 20, kBelow, kNormal, kAbove, kHigh };
        HMENU menu = CreatePopupMenu();
        HMENU pri = CreatePopupMenu();
        AppendMenuW(pri, MF_STRING, kHigh, L"High");
        AppendMenuW(pri, MF_STRING, kAbove, L"Above normal");
        AppendMenuW(pri, MF_STRING, kNormal, L"Normal");
        AppendMenuW(pri, MF_STRING, kBelow, L"Below normal");
        AppendMenuW(pri, MF_STRING, kIdle, L"Low");
        AppendMenuW(menu, MF_STRING, kEnd, L"End task\tDel");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kSuspend, L"Suspend");
        AppendMenuW(menu, MF_STRING, kResume, L"Resume");
        AppendMenuW(menu, MF_STRING | (EfficiencyModeOn(pid) ? MF_CHECKED : 0), kEfficiency, L"Efficiency mode");
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(pri), L"Set priority");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kOpen, L"Open file location");
        AppendMenuW(menu, MF_STRING, kSearch, L"Search online");
        AppendMenuW(menu, MF_STRING, kCopyName, L"Copy name");
        AppendMenuW(menu, MF_STRING, kCopyPath, L"Copy full path");
        const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr));
        DestroyMenu(menu);

        switch (cmd) {
        case kEnd: EndSelected(); break;
        case kSuspend:
            WithProcess(pid, PROCESS_SUSPEND_RESUME, L"suspend the process", [](HANDLE h) { return CallNt(h, "NtSuspendProcess"); });
            break;
        case kResume:
            WithProcess(pid, PROCESS_SUSPEND_RESUME, L"resume the process", [](HANDLE h) { return CallNt(h, "NtResumeProcess"); });
            break;
        case kEfficiency: {
            const bool turnOn = !EfficiencyModeOn(pid);
            WithProcess(pid, PROCESS_SET_INFORMATION, L"change efficiency mode", [turnOn](HANDLE h) -> DWORD {
                // Same recipe as Windows Task Manager: EcoQoS throttling plus low priority.
                PROCESS_POWER_THROTTLING_STATE s{};
                s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
                s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
                s.StateMask = turnOn ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
                if (!SetProcessInformation(h, ProcessPowerThrottling, &s, sizeof s)) return GetLastError();
                if (!SetPriorityClass(h, turnOn ? IDLE_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS)) return GetLastError();
                return 0;
            });
            break;
        }
        case kIdle: case kBelow: case kNormal: case kAbove: case kHigh: {
            static const DWORD classes[] = {IDLE_PRIORITY_CLASS, BELOW_NORMAL_PRIORITY_CLASS, NORMAL_PRIORITY_CLASS,
                                            ABOVE_NORMAL_PRIORITY_CLASS, HIGH_PRIORITY_CLASS};
            const DWORD cls = classes[cmd - kIdle];
            WithProcess(pid, PROCESS_SET_INFORMATION, L"change the priority",
                        [cls](HANDLE h) -> DWORD { return SetPriorityClass(h, cls) ? 0 : GetLastError(); });
            break;
        }
        case kOpen: {
            const std::wstring path = ImagePath(pid);
            if (path.empty()) ShowError(L"The file location isn't available for this process (try running as administrator).");
            else ShowInExplorer(path, true);
            break;
        }
        case kSearch: ShowInExplorer(L"https://www.bing.com/search?q=" + name, false); break;
        case kCopyName: CopyToClipboard(name); break;
        case kCopyPath: CopyToClipboard(ImagePath(pid)); break;
        }
        Refresh();
        return true;
    }

private:
    void Refresh() {
        const std::wstring needle = ToLower(GetText(search_));
        std::vector<Row> rows;
        const auto& procs = g_procs.Processes();
        rows.reserve(procs.size());
        double cpuSum = 0;
        uint64_t memSum = 0;
        for (const ProcInfo& p : procs) {
            cpuSum += p.cpu;
            memSum += p.privateWs;
            if (!needle.empty() && ToLower(p.name).find(needle) == std::wstring::npos &&
                std::to_wstring(p.pid).find(needle) == std::wstring::npos)
                continue;
            const double gpu = GpuPercentForPid(p.pid);
            Row r;
            r.tag = p.pid;
            r.icon = ProcessIcon(p.pid);
            r.Add(p.name)
                .Add(std::to_wstring(p.pid), p.pid)
                .Add(Format(L"%.1f%%", p.cpu), p.cpu)
                .Add(FormatBytes(p.privateWs), double(p.privateWs))
                .Add(gpu >= 0.05 ? Format(L"%.1f%%", gpu) : L"0%", gpu)
                .Add(p.ioRate >= 1 ? FormatBytes(static_cast<uint64_t>(p.ioRate)) + L"/s" : L"0 B/s", p.ioRate)
                .Add(FormatBytes(p.workingSet), double(p.workingSet))
                .Add(FormatBytes(p.commit), double(p.commit))
                .Add(FormatCount(p.threads), p.threads)
                .Add(FormatCount(p.handles), p.handles)
                .Add(std::to_wstring(p.session), p.session)
                .Add(PriorityName(p.basePriority));
            rows.push_back(std::move(r));
        }
        const size_t shown = rows.size();
        table_.SetRows(std::move(rows));
        SetText(summary_, Format(L"%zu of %zu processes  ·  %s threads  ·  %s handles", shown, procs.size(),
                                 FormatCount(g_procs.ThreadCount()).c_str(), FormatCount(g_procs.HandleCount()).c_str()));
    }

    void EndSelected() {
        const Row* row = table_.SelectedRow();
        if (!row) return;
        const DWORD pid = static_cast<DWORD>(row->tag);
        if (!Confirm(Format(L"End \"%s\" (PID %lu)?\n\nUnsaved data in this process will be lost.", row->cells[0].c_str(), pid)))
            return;
        WithProcess(pid, PROCESS_TERMINATE, L"end the process",
                    [](HANDLE h) -> DWORD { return TerminateProcess(h, 1) ? 0 : GetLastError(); });
    }

    HWND search_ = nullptr, summary_ = nullptr, run_ = nullptr, end_ = nullptr;
    Table table_;
};

}  // namespace

std::unique_ptr<Page> MakeProcessesPage() { return std::make_unique<ProcessesPage>(); }

}  // namespace mtm
