#include "optimize_panel.h"

#include "theme.h"

namespace mtm {
namespace {

constexpr int kRemindDays[] = {0, 14, 30, 90};
constexpr uint64_t kDay = 864000000000ull;

uint64_t Now() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

int DaysSince(uint64_t ft) { return ft ? static_cast<int>((Now() - ft) / kDay) : -1; }

}  // namespace

void OptimizePanel::Create() {
    info_.Create(L"Optimize drives");
    info_.SetSections({
        {L"What it does",
         L"HDDs get defragmented: file pieces are moved together so the disk head travels less. SSDs are never "
         L"defragmented here; they get a quick re-TRIM that tells the drive which blocks are free, keeping writes fast."},
        {L"Recommended: turn off the automatic schedule",
         L"Windows runs it weekly in the background, sometimes mid-game or mid-work, and on HDDs the reshuffling adds "
         L"wear. Switch to manual and optimize when reminded."},
        {L"How often",
         L"HDD: monthly, or when Analyze shows over 10% fragmented. SSD: a monthly re-TRIM is plenty."},
    });
    volumes_.Create({{L"Drive", 140}, {L"Type", 56}, {L"Optimization", 104}, {L"Last optimized", 140, true}, {L"Status", 170}},
                    -1, false);
    optimize_ = CreateButton(L"\xE768  Optimize now", true);
    analyze_ = CreateButton(L"\xE721  Analyze");
    stop_ = CreateButton(L"\xE71A  Stop");
    progress_.Create();
    status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
    schedule_ = CreateButton(L"Automatic schedule: checking…");
    remindLabel_ = CreateLabel(L"Remind me", SS_CENTERIMAGE | SS_RIGHT);
    remind_ = CreateCombo({L"Never", L"Every 2 weeks", L"Every month", L"Every 3 months"});
    later_ = CreateButton(L"\xE823  Remind me later");
    const int days = OptimizeReminderDays();
    int sel = 2;
    for (int i = 0; i < 4; ++i)
        if (kRemindDays[i] == days) sel = i;
    SendMessageW(remind_, CB_SETCURSEL, sel, 0);
    Refresh();  // also lights the sidebar dot at startup when an optimization is due
}

void OptimizePanel::Layout(RECT a) {
    area_ = a;
    const int gap = Scale(12), h = Scale(34);
    const int infoW = std::max<int>(Scale(300), (a.right - a.left) * 9 / 25);
    Place(info_.Handle(), a.left, a.top, infoW, a.bottom - a.top);
    const int x = a.left + infoW + gap, w = a.right - x;
    int y = a.top;
    // Controls flow left to right and wrap onto a new row when the panel is narrow.
    int cx = x;
    auto place = [&](HWND hw, int width, int dy = 0, int height = 0) {
        if (cx > x && cx + width > a.right) {
            cx = x;
            y += h + Scale(8);
        }
        Place(hw, cx, y + dy, width, height ? height : h);
        cx += width + Scale(8);
    };
    place(optimize_, Scale(140));
    place(analyze_, Scale(104));
    place(stop_, Scale(84));
    // The progress bar takes what is left of the row; only wraps when under 80 px.
    if (a.right - cx < Scale(80)) {
        cx = x;
        y += h + Scale(8);
    }
    Place(progress_.Handle(), cx + Scale(4), y + (h - Scale(8)) / 2, std::max(0, int(a.right) - cx - Scale(4)), Scale(8));
    cx = x;
    y += h + Scale(8);
    place(schedule_, Scale(290));
    if (cx + Scale(80 + 150) > a.right) {  // keep "Remind me" together with its dropdown
        cx = x;
        y += h + Scale(8);
    }
    place(remindLabel_, Scale(80));
    cx -= Scale(4);
    place(remind_, Scale(150), 0, Scale(200));
    y += h + Scale(6);
    // Status line, with "Remind me later" at its right end (shown only when a drive is due).
    const int laterW = Scale(160);
    Place(later_, a.right - laterW, y - Scale(4), laterW, h);
    Place(status_, x + Scale(2), y, w - Scale(2) - (due_ ? laterW + Scale(8) : 0), Scale(26));
    y += Scale(32);
    Place(volumes_.Handle(), x, y, w, a.bottom - y);
}

void OptimizePanel::Show(bool v) {
    visible_ = v;
    for (HWND hw : {info_.Handle(), volumes_.Handle(), optimize_, analyze_, stop_, progress_.Handle(), status_, schedule_,
                    remindLabel_, remind_})
        ShowWindow(hw, v ? SW_SHOW : SW_HIDE);
    ShowWindow(later_, v && due_ ? SW_SHOW : SW_HIDE);
}

void OptimizePanel::OnActivate() {
    if (!busy_) Refresh();
}

void OptimizePanel::OnTick(bool) {
    // Re-check now and then so a reminder shows up even if the app stays open for days.
    if (++ticks_ % 1800 == 0 && !busy_) Refresh();
}

void OptimizePanel::Refresh() {
    if (refreshing_) return;
    refreshing_ = true;
    refreshWorker_ = std::jthread([this](std::stop_token) {
        auto list = ListOptimizableVolumes();
        const int automatic = AutomaticOptimizationEnabled();
        PostToUi([this, list = std::move(list), automatic]() mutable {
            refreshing_ = false;
            list_ = std::move(list);
            automatic_ = automatic;
            Populate();
        });
    });
}

void OptimizePanel::Populate() {
    const bool wasDue = due_;
    due_ = OptimizationDue(list_);
    const int remind = OptimizeReminderDays();
    std::vector<Row> rows;
    std::wstring dueText;
    for (const OptVolume& v : list_) {
        Row r;
        r.tag = v.letter;
        const int days = DaysSince(v.lastOptimized);
        const bool overdue = remind > 0 && (days < 0 || days > remind);
        std::wstring status;
        if (busy_ && runningLetter_ == v.letter) status = L"Running…";
        else if (auto it = lastResult_.find(v.letter); it != lastResult_.end()) status = it->second;
        else if (overdue) status = days < 0 ? L"Never optimized" : Format(L"Due — %d days ago", days);
        else status = days < 0 ? L"—" : L"OK";
        if (overdue) {
            r.accent = g_pal.warn;
            dueText += (dueText.empty() ? L"" : L", ") + std::wstring(1, v.letter) + L":";
        }
        const std::wstring name = std::wstring(1, v.letter) + L":  " + (v.label.empty() ? L"Local Disk" : v.label);
        const wchar_t* type = v.knownMedia ? (v.ssd ? L"SSD" : L"HDD") : L"?";
        const wchar_t* op = v.knownMedia ? (v.ssd ? L"Re-TRIM" : L"Defragment") : L"Optimize";
        r.Add(name).Add(type).Add(op)
            .Add(v.lastOptimized ? FormatFileTime(v.lastOptimized, false) + (days >= 0 ? Format(L"  (%dd)", days) : L"") : L"Never",
                 double(v.lastOptimized))
            .Add(status);
        rows.push_back(std::move(r));
    }
    volumes_.SetRows(std::move(rows));
    if (volumes_.SelectedIndex() < 0 && volumes_.Size())
        ListView_SetItemState(volumes_.Handle(), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);

    SetText(schedule_, automatic_ == 1 ? L"\xE7BA  Automatic: ON — turn off (recommended)"
                       : automatic_ == 0 ? L"\xE73E  Automatic: OFF — manual only"
                                         : L"Automatic schedule: unknown");
    SetButtonPrimary(schedule_, false);
    if (!busy_) {
        if (due_) SetText(status_, L"Reminder: " + dueText + L" hasn't been optimized recently. Select it and press Optimize now.");
        else if (automatic_ == 1) SetText(status_, L"Windows optimizes these drives weekly. You can switch to manual with reminders.");
        else SetText(status_, remind ? Format(L"Manual mode. You'll be reminded every %d days.", remind)
                                     : L"Manual mode with reminders off.");
    }
    ShowWindow(later_, visible_ && due_ ? SW_SHOW : SW_HIDE);
    UpdateButtons();
    if (wasDue != due_) {
        InvalidateRect(g_main, nullptr, FALSE);  // sidebar reminder dot
        if (area_.right > area_.left) Layout(area_);  // status line makes room for "Remind me later"
    }
}

wchar_t OptimizePanel::SelectedLetter() const {
    const Row* r = volumes_.SelectedRow();
    return r ? static_cast<wchar_t>(r->tag) : 0;
}

void OptimizePanel::UpdateButtons() {
    const wchar_t letter = SelectedLetter();
    bool ssd = false;
    for (const OptVolume& v : list_)
        if (v.letter == letter) ssd = v.ssd;
    EnableWindow(optimize_, letter && !busy_);
    EnableWindow(analyze_, letter && !busy_ && !ssd);  // fragmentation doesn't matter on an SSD
    EnableWindow(stop_, busy_);
    EnableWindow(schedule_, automatic_ >= 0 && !busy_);
}

void OptimizePanel::Start(bool analyze) {
    const wchar_t letter = SelectedLetter();
    if (!letter || busy_) return;
    busy_ = true;
    runningLetter_ = letter;
    lastResult_.erase(letter);
    progress_.SetFraction(0);
    SetText(status_, Format(L"%s %c: …%s", analyze ? L"Analyzing" : L"Optimizing", letter,
                            IsElevated() ? L"" : L" (approve the administrator prompt)"));
    Populate();
    worker_ = std::jthread([this, letter, analyze](std::stop_token st) {
        OptimizeResult r = RunOptimization(letter, analyze, st, [this, letter](double pct, const std::wstring& stage) {
            PostToUi([this, letter, pct, stage] {
                progress_.SetFraction(pct / 100.0);
                SetText(status_, Format(L"%c:  %s  %.0f%%", letter, stage.c_str(), pct));
            });
        });
        PostToUi([this, letter, analyze, r = std::move(r)] {
            busy_ = false;
            runningLetter_ = 0;
            std::wstring result;
            if (r.code == ERROR_SUCCESS) {
                progress_.SetFraction(1);
                if (analyze && r.fragmentedPercent >= 0)
                    result = Format(L"Analyzed: %d%% fragmented%s", r.fragmentedPercent,
                                    r.fragmentedPercent > 10 ? L" — optimize recommended" : L" — fine");
                else
                    result = analyze ? L"Analyzed" : L"Optimized just now";
                SetText(status_, Format(L"%c:  done. ", letter) + r.summary);
            } else {
                progress_.SetFraction(0);
                result = r.code == ERROR_CANCELLED ? L"Cancelled" : L"Failed";
                SetText(status_, r.code == ERROR_CANCELLED ? std::wstring(L"Cancelled.")
                                 : Format(L"%c:  failed (code %lu). ", letter, r.code) + r.summary);
            }
            lastResult_[letter] = result;
            Refresh();  // re-reads the event log, so "Last optimized" updates
        });
    });
}

bool OptimizePanel::OnCommand(HWND control, WORD code) {
    if (control == remind_ && code == CBN_SELCHANGE) {
        const int sel = static_cast<int>(SendMessageW(remind_, CB_GETCURSEL, 0, 0));
        if (sel >= 0 && sel < 4) SetOptimizeReminderDays(kRemindDays[sel]);
        Populate();
        return true;
    }
    if (code != BN_CLICKED) return false;
    if (control == optimize_) Start(false);
    else if (control == analyze_) Start(true);
    else if (control == stop_) worker_.request_stop();
    else if (control == later_) {
        SnoozeOptimizeReminder(7);
        Populate();
    } else if (control == schedule_) {
        if (automatic_ < 0 || busy_) return true;
        const bool enable = automatic_ == 0;
        if (enable && !Confirm(L"Turn Windows' weekly automatic optimization back on?")) return true;
        EnableWindow(schedule_, FALSE);
        SetText(status_, enable ? L"Turning automatic optimization on…" : L"Turning automatic optimization off…");
        scheduleWorker_ = std::jthread([this, enable] {
            const DWORD r = SetAutomaticOptimization(enable);
            PostToUi([this, r] {
                if (r == ERROR_CANCELLED) SetText(status_, L"Not changed — the administrator prompt was cancelled.");
                else if (r != ERROR_SUCCESS) SetText(status_, L"Couldn't change the schedule: " + ErrorText(r));
                Refresh();
            });
        });
    } else {
        return false;
    }
    return true;
}

bool OptimizePanel::OnNotify(NMHDR* nm, LRESULT& result) {
    if (nm->hwndFrom != volumes_.Handle()) return false;
    if (nm->code == LVN_ITEMCHANGED) UpdateButtons();
    return volumes_.HandleNotify(nm, result);
}

}  // namespace mtm
