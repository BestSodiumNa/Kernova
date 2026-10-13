// page_health.cpp
#include "health.h"
#include "optimize_panel.h"
#include "page.h"
#include "table.h"
#include "theme.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <future>

namespace mtm {
namespace {

COLORREF HealthAccent(Health h) {
    return h == Health::Bad ? g_pal.bad : h == Health::Warning ? g_pal.warn : CLR_NONE;
}

class HealthPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Drive Health"; }

    void Create() override {
        status_     = CreateLabel(L"", SS_CENTERIMAGE);
        refreshBtn_ = CreateButton(L"  Refresh");

        drivesTable_.Create({{L"Drive", 80, true},
                             {L"Model", 280},
                             {L"Bus", 70},
                             {L"Size", 90, true, -(1024.0 * 1024 * 1024)},
                             {L"Temp", 70, true, -1},
                             {L"Health", 100}},
                            0, true);

        attrs_.Create({{L"Attribute", 280}, {L"Value", 160, true}, {L"Detail", 360}}, -1, false);
        optimize_.Create();
    }

    // Drives, attributes and the optimize panel each need room to be readable.
    int MinHeight(int) const override { return Scale(920); }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(120), gap = Scale(8);
        Place(refreshBtn_, a.right - btn, a.top, btn, h);
        Place(status_, a.left, a.top, a.right - a.left - btn - gap, h);

        const int top = a.top + h + Scale(12);
        // Top half: drives + attributes; bottom half: the optimize (defrag / TRIM) panel.
        const int panelTop = a.top + (a.bottom - a.top) / 2;
        const int mid = top + (panelTop - top) * 2 / 5;
        Place(drivesTable_.Handle(), a.left, top, a.right - a.left, mid - top);
        Place(attrs_.Handle(), a.left, mid + Scale(8), a.right - a.left, panelTop - mid - Scale(8));
        optimize_.Layout(RECT{a.left, panelTop + Scale(14), a.right, a.bottom});
    }

    void Show(bool v) override {
        ShowAll({status_, refreshBtn_, drivesTable_.Handle(), attrs_.Handle()}, v);
        optimize_.Show(v);
    }

    bool NeedsAttention() const override { return optimize_.NeedsAttention(); }

    void OnActivate() override {
        KickQuery();
        optimize_.OnActivate();
    }

    void OnTick(bool visible) override {
        optimize_.OnTick(visible);
        if (!visible) return;
        PollQuery();
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (nm->hwndFrom == drivesTable_.Handle() && nm->code == LVN_ITEMCHANGED) {
            ShowAttributes();  // selection changed
        }
        if (optimize_.OnNotify(nm, result)) return true;
        if (drivesTable_.HandleNotify(nm, result)) return true;
        return attrs_.HandleNotify(nm, result);
    }

    bool OnCommand(HWND control, WORD code) override {
        if (optimize_.OnCommand(control, code)) return true;
        if (control == refreshBtn_ && code == BN_CLICKED) {
            KickQuery();
            return true;
        }
        return false;
    }

private:
    // Not on a timer: one query can be tens of seconds of I/O on SATA drives.
    void KickQuery() {
        if (queryInFlight_.exchange(true)) return;  // already running
        SetText(status_, L"Reading drive health…");
        EnableWindow(refreshBtn_, FALSE);
        future_ = std::async(std::launch::async, [] { return QueryDriveHealth(); });
    }

    void PollQuery() {
        if (!future_.valid()) return;
        if (future_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;

        queryInFlight_.store(false);
        EnableWindow(refreshBtn_, TRUE);
        try {
            drives_ = future_.get();
        } catch (const std::exception& e) {
            // A failure here should mean "no drive data", not a crashed task manager.
            drives_.clear();
            const std::string what = e.what();
            SetText(status_, L"Couldn't read drive health: " + std::wstring(what.begin(), what.end()));
            drivesTable_.SetRows({});
            attrs_.SetRows({});
            return;
        }
        Populate();
    }

    void Populate() {
        std::vector<Row> rows;
        rows.reserve(drives_.size());
        size_t attention = 0;
        for (size_t i = 0; i < drives_.size(); ++i) {
            const DriveHealth& d = drives_[i];
            if (d.health == Health::Bad || d.health == Health::Warning) ++attention;
            Row r;
            r.tag = i;
            r.accent = HealthAccent(d.health);
            const std::wstring temp = d.temperatureC >= 0 ? Format(L"%d °C", d.temperatureC) : L"—";
            r.Add(d.kind.empty() ? Format(L"Disk %d", d.number) : Format(L"%s %d", d.kind.c_str(), d.number), d.number)
             .Add(d.model.empty() ? L"(unknown model)" : d.model)
             .Add(d.bus)
             .Add(FormatBytes(d.size), double(d.size))
             .Add(temp, double(d.temperatureC))
             .Add(HealthName(d.health));
            rows.push_back(std::move(r));
        }
        drivesTable_.SetRows(std::move(rows));

        const size_t n = drives_.size();
        SetText(status_, Format(L"%zu drive%s  ·  %s", n, n == 1 ? L"" : L"s",
                                attention ? Format(L"%zu need attention", attention).c_str() : L"no problems reported"));

        // Select the first row so the attribute table isn't empty.
        if (n > 0 && drivesTable_.SelectedIndex() < 0)
            ListView_SetItemState(drivesTable_.Handle(), 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ShowAttributes();
    }

    void ShowAttributes() {
        const Row* sel = drivesTable_.SelectedRow();
        if (!sel || sel->tag >= drives_.size()) {
            attrs_.SetRows({});
            return;
        }
        const DriveHealth& d = drives_[static_cast<size_t>(sel->tag)];
        std::vector<Row> rows;
        rows.reserve(d.attributes.size() + 1);
        if (!d.summary.empty()) {  // one-line verdict first
            Row r;
            r.tag = 0;
            r.accent = HealthAccent(d.health);
            r.Add(L"Summary").Add(HealthName(d.health)).Add(d.summary);
            rows.push_back(std::move(r));
        }
        for (size_t i = 0; i < d.attributes.size(); ++i) {
            const DriveAttribute& a = d.attributes[i];
            Row r;
            r.tag = i + 1;  // distinct tags keep the selection stable
            r.accent = HealthAccent(a.health);
            r.Add(a.name).Add(a.value).Add(a.detail);
            rows.push_back(std::move(r));
        }
        attrs_.SetRows(std::move(rows));
    }

    HWND  status_     = nullptr;
    HWND  refreshBtn_ = nullptr;
    Table drivesTable_;
    Table attrs_;
    OptimizePanel optimize_;

    std::vector<DriveHealth>              drives_;
    std::atomic<bool>                     queryInFlight_{false};
    std::future<std::vector<DriveHealth>> future_;  // last: its destructor waits for the query
};

}  // namespace

std::unique_ptr<Page> MakeHealthPage() { return std::make_unique<HealthPage>(); }

}  // namespace mtm
