#include "graph.h"
#include "page.h"
#include "power.h"
#include "sysinfo.h"
#include "table.h"
#include "theme.h"

#include <algorithm>
#include <thread>

namespace mtm {
namespace {

std::wstring PowerSourceText() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return L"Power status unavailable";
    std::wstring text = s.ACLineStatus == 1 ? L"Plugged in" : (s.ACLineStatus == 0 ? L"On battery" : L"Power source unknown");
    if (s.BatteryFlag != 128 && s.BatteryFlag != 255 && s.BatteryLifePercent <= 100) {
        text += Format(L"  ·  Battery %u%%", s.BatteryLifePercent);
        if (s.BatteryFlag & 8) text += L" (charging)";
        if (s.ACLineStatus == 0 && s.BatteryLifeTime != static_cast<DWORD>(-1))
            text += L"  ·  " + FormatDuration(s.BatteryLifeTime) + L" left";
    } else if (s.BatteryFlag == 128) {
        text += L"  ·  No battery";
    }
    if (s.SystemStatusFlag & 1) text += L"  ·  Battery saver on";
    return text;
}

bool OnBattery() {
    SYSTEM_POWER_STATUS s{};
    return GetSystemPowerStatus(&s) && s.ACLineStatus == 0;
}

class PowerPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Power & frequency"; }

    void Create() override {
        planCard_.Create(0xE945, L"Power plan", Graph::Tint::Memory, nullptr);
        parkCard_.Create(0xE950, L"Core parking", Graph::Tint::Cpu, nullptr);
        clockCard_.Create(0xEC4A, L"Clock speed", Graph::Tint::Network, nullptr);
        status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);

        plans_.Create({{L"Power plan", 200}, {L"Kind", 110}, {L"Status", 70}}, -1, false);
        activate_ = CreateButton(L"\xE73E  Use this plan", true);
        delete_ = CreateButton(L"\xE74D  Delete");
        std::vector<const wchar_t*> names;
        for (const PowerPreset& p : PowerPresets()) names.push_back(p.name);
        presetPick_ = CreateCombo({});
        for (const wchar_t* n : names) SendMessageW(presetPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n));
        SendMessageW(presetPick_, CB_SETCURSEL, 4, 0);  // Gaming
        addPreset_ = CreateButton(L"\xE710  Add preset");
        presetInfo_.Create(L"Preset");

        // Core parking: one compact row; the full explanation lives in the hover badge.
        parkLabel_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        parkInfo_.Create(L"About core parking",
                         {{L"What it is",
                           L"Windows can \"park\" idle cores: put them into a deep sleep and send work to the remaining "
                           L"ones. That saves power, but a parked core needs a moment to wake up, which can show up as "
                           L"micro-stutter in games or crackles in audio work."},
                          {L"What we recommend",
                           L"Plugged in, gaming or doing real-time work: disable parking. On battery: leave it allowed, "
                           L"it's a real battery saver. The setting applies to the active plan only."},
                          {L"Good to know",
                           L"Many AMD Ryzen and some Intel laptops manage idle cores in firmware or the chipset driver, so "
                           L"Windows may show no parked cores whichever way this is set."}});
        parkOff_ = CreateButton(L"\xE7E8  Disable parking", true);
        parkOn_ = CreateButton(L"\xE708  Allow parking");

        // Two lists side by side (first half / second half of the CPUs) on wide windows, so
        // every logical processor is visible at once without scrolling.
        for (Table* t : {&cores_, &cores2_})
            t->Create({{L"Logical processor", 120},
                       {L"State", 70},
                       {L"Usage", 70, true, 100},
                       {L"Current MHz", 100, true},
                       {L"Base MHz", 85, true},
                       {L"Limit MHz", 85, true},
                       {L"% of base", 80, true, -120}},
                      -1, false);
        ShowPresetInfo();
    }

    // Heights of the bands, top to bottom. The CPU list is sized to show every logical
    // processor without its own scrollbar; the page scrolls instead when space runs out.
    int PlansTableH() const { return plans_.HeightForRows(std::clamp<size_t>(list_.size(), 2, 5)); }
    static size_t CpuCount() { return std::max<size_t>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)); }
    static bool Split(int width) { return width >= Scale(1250) && CpuCount() > 4; }
    int CoresTableH(int width) const { return cores_.HeightForRows(Split(width) ? (CpuCount() + 1) / 2 : CpuCount()); }
    static int CardH() { return Scale(140); }
    int BandH() const { return std::max(PlansTableH() + 2 * Scale(34) + Scale(16), Scale(200)); }

    // Right column of the plans band, where the parking buttons go when they fit.
    static bool ParkRowSeparate(int width) {
        const int rw = width - (width - Scale(12)) * 11 / 20 - Scale(12);
        return (rw - Scale(34) - Scale(16)) / 2 < Scale(200);
    }
    int MinHeight(int width) const override {
        return CardH() + Scale(38) + BandH() + Scale(12) + (ParkRowSeparate(width) ? Scale(46) : 0) + CoresTableH(width);
    }

    void Layout(RECT a) override {
        const int gap = Scale(12), h = Scale(34), w = a.right - a.left;
        const int cardW = (w - 2 * gap) / 3;
        Place(planCard_.Handle(), a.left, a.top, cardW, CardH());
        Place(parkCard_.Handle(), a.left + cardW + gap, a.top, cardW, CardH());
        Place(clockCard_.Handle(), a.left + 2 * (cardW + gap), a.top, w - 2 * (cardW + gap), CardH());
        int y = a.top + CardH() + Scale(6);
        Place(status_, a.left + Scale(2), y, w, Scale(26));
        y += Scale(32);

        // Left: plans list and its actions. Right: the selected preset's description, with
        // the core parking switch underneath (its status is on the Core parking card above).
        const int leftW = (w - gap) * 11 / 20, rx = a.left + leftW + gap, rw = w - leftW - gap;
        const int bandH = BandH();
        const int tableH = bandH - 2 * h - Scale(16);
        Place(plans_.Handle(), a.left, y, leftW, tableH);
        // Name column takes whatever the fixed Kind / Status columns leave (no sideways scrolling).
        ListView_SetColumnWidth(plans_.Handle(), 1, Scale(110));
        ListView_SetColumnWidth(plans_.Handle(), 2, Scale(84));
        ListView_SetColumnWidth(plans_.Handle(), 0, std::max(Scale(120), leftW - Scale(110 + 84) - Scale(6)));
        int ly = y + tableH + Scale(8);
        Place(activate_, a.left, ly, Scale(150), h);
        Place(delete_, a.left + Scale(158), ly, Scale(100), h);
        ly += h + Scale(8);
        const int comboW = std::max(Scale(160), leftW - Scale(150));
        Place(presetPick_, a.left, ly, comboW, Scale(300));
        Place(addPreset_, a.left + comboW + Scale(8), ly, leftW - comboW - Scale(8), h);

        // Parking buttons sit under the preset card when there is room for both at a
        // comfortable width; otherwise they get their own row below this band.
        const int bw = (rw - h - Scale(16)) / 2;
        const bool inline_ = !ParkRowSeparate(w);
        Place(presetInfo_.Handle(), rx, y, rw, inline_ ? bandH - h - Scale(8) : bandH);
        y += bandH + gap;
        if (inline_) {
            const int by = y - gap - h;
            Place(parkOff_, rx, by, bw, h);
            Place(parkOn_, rx + bw + Scale(8), by, bw, h);
            Place(parkInfo_.Handle(), rx + 2 * bw + Scale(16), by, h, h);
        } else {
            const int fw = Scale(200);
            Place(parkOff_, a.left, y, fw, h);
            Place(parkOn_, a.left + fw + Scale(8), y, fw, h);
            Place(parkInfo_.Handle(), a.left + 2 * fw + Scale(16), y, h, h);
            y += h + gap;
        }

        split_ = Split(w);
        const int coresH = std::max(CoresTableH(w), int(a.bottom) - y);
        if (split_) {
            const int half = (w - gap) / 2;
            Place(cores_.Handle(), a.left, y, half, coresH);
            Place(cores2_.Handle(), a.left + half + gap, y, w - half - gap, coresH);
        } else {
            Place(cores_.Handle(), a.left, y, w, coresH);
        }
        ShowWindow(cores2_.Handle(), visible_ && split_ ? SW_SHOW : SW_HIDE);
    }

    void Show(bool v) override {
        ShowAll({planCard_.Handle(), parkCard_.Handle(), clockCard_.Handle(), status_, plans_.Handle(), activate_, delete_,
                 presetPick_, addPreset_, presetInfo_.Handle(), parkInfo_.Handle(), parkLabel_, parkOff_, parkOn_, cores_.Handle()},
                v);
        visible_ = v;
        ShowWindow(cores2_.Handle(), v && split_ ? SW_SHOW : SW_HIDE);
    }

    void OnActivate() override {
        RefreshPlans();
        Refresh();
    }

    void OnTick(bool visible) override {
        if (!visible) return;
        Refresh();
        if (++ticks_ % 5 == 0 && !busy_) RefreshPlans();  // notices plan changes made elsewhere
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (nm->hwndFrom == plans_.Handle() && nm->code == LVN_ITEMCHANGED) UpdateButtons();
        if (nm->hwndFrom == plans_.Handle() && nm->code == NM_DBLCLK) {
            Activate();
            return true;
        }
        return plans_.HandleNotify(nm, result) || cores_.HandleNotify(nm, result) || cores2_.HandleNotify(nm, result);
    }

    bool OnCommand(HWND control, WORD code) override {
        if (control == presetPick_ && code == CBN_SELCHANGE) {
            ShowPresetInfo();
            return true;
        }
        if (code != BN_CLICKED) return false;
        if (control == activate_) Activate();
        else if (control == delete_) Delete();
        else if (control == addPreset_) AddPreset();
        else if (control == parkOff_) Parking(true);
        else if (control == parkOn_) Parking(false);
        else return false;
        return true;
    }

private:
    const PowerPlan* SelectedPlan() const {
        const Row* r = plans_.SelectedRow();
        return r && r->tag < list_.size() ? &list_[size_t(r->tag)] : nullptr;
    }

    void RefreshPlans() {
        const size_t before = list_.size();
        list_ = ListPowerPlans();
        if (list_.size() != before && before) SendMessageW(g_main, WM_SIZE, 0, 0);  // plans list height follows the count
        parking_ = ReadCoreParking();
        std::vector<Row> rows;
        for (size_t i = 0; i < list_.size(); ++i) {
            const PowerPlan& p = list_[i];
            Row r;
            r.tag = i;
            if (p.active) r.accent = g_pal.accent;
            r.Add(p.name.empty() ? L"(unnamed plan)" : p.name)
                .Add(p.builtIn ? L"Windows" : p.presetIndex >= 0 ? L"Kernova preset" : L"Custom / vendor")
                .Add(p.active ? L"\x2713  Active" : L"");
            rows.push_back(std::move(r));
        }
        plans_.SetRows(std::move(rows));
        if (plans_.SelectedIndex() < 0)
            for (size_t i = 0; i < list_.size(); ++i)
                if (list_[i].active) plans_.SelectTag(i);
        UpdateButtons();
    }

    void UpdateButtons() {
        const PowerPlan* p = SelectedPlan();
        EnableWindow(activate_, p && !p->active && !busy_);
        EnableWindow(delete_, p && !p->active && !busy_);
        EnableWindow(addPreset_, !busy_);
        EnableWindow(parkOff_, parking_.known && !busy_ && !(parking_.DisabledAc() && parking_.DisabledDc()));
        EnableWindow(parkOn_, parking_.known && !busy_ && (parking_.DisabledAc() || parking_.DisabledDc()));
    }

    void ShowPresetInfo() {
        const int sel = static_cast<int>(SendMessageW(presetPick_, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(PowerPresets().size())) return;
        const PowerPreset& p = PowerPresets()[size_t(sel)];
        presetInfo_.SetSections({{p.summary, L""}, {L"What it changes", p.what}, {L"When to use it", p.why}},
                                Format(L"%d of %zu presets", sel + 1, PowerPresets().size()));
    }

    // Runs a power change off the UI thread (it may show a UAC prompt), then refreshes.
    void RunOp(std::wstring busyText, std::function<DWORD()> op, std::wstring doneText) {
        if (busy_) return;
        busy_ = true;
        UpdateButtons();
        SetText(status_, busyText);
        worker_ = std::jthread([this, op = std::move(op), doneText = std::move(doneText)](std::stop_token) {
            const DWORD r = op();
            PostToUi([this, r, doneText] {
                busy_ = false;
                if (r == ERROR_SUCCESS) SetText(status_, doneText);
                else if (r == ERROR_CANCELLED) SetText(status_, L"Not changed — the administrator prompt was cancelled.");
                else SetText(status_, L"Couldn't change it: " + ErrorText(r));
                RefreshPlans();
                Refresh();
            });
        });
    }

    void Activate() {
        const PowerPlan* p = SelectedPlan();
        if (!p || p->active) return;
        const GUID g = p->guid;
        const std::wstring name = p->name;
        RunOp(L"Switching to " + name + L"…", [g] { return ActivatePowerPlan(g); }, L"Now using " + name + L".");
    }

    void Delete() {
        const PowerPlan* p = SelectedPlan();
        if (!p || p->active) return;
        if (!Confirm(L"Delete the power plan \"" + p->name + L"\"?" +
                     (p->builtIn ? std::wstring(L"\n\nIt's a Windows plan; you can add it back later from the presets.") : L"")))
            return;
        const GUID g = p->guid;
        const std::wstring name = p->name;
        RunOp(L"Deleting " + name + L"…", [g] { return DeletePowerPlan(g); }, L"Deleted " + name + L".");
    }

    void AddPreset() {
        const int sel = static_cast<int>(SendMessageW(presetPick_, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= static_cast<int>(PowerPresets().size())) return;
        const std::wstring name = PowerPresets()[size_t(sel)].name;
        RunOp(L"Adding " + name + L"…", [sel] { return AddPowerPreset(size_t(sel)); }, L"Added and switched to " + name + L".");
    }

    void Parking(bool disable) {
        RunOp(disable ? L"Disabling core parking…" : L"Allowing core parking…",
              [disable] { return SetCoreParkingDisabled(disable); },
              disable ? L"Core parking disabled for this plan: all cores stay awake." : L"Core parking allowed again (plan default).");
    }

    void Refresh() {
        const auto& cores = Sys().cores;
        std::vector<Row> rows;
        ULONG nominal = 0, lowest = ~0ul, highest = 0;
        int parked = 0, knownParking = 0;
        for (size_t i = 0; i < cores.size(); ++i) {
            const CoreInfo& c = cores[i];
            nominal = std::max(nominal, c.nominalMhz);
            lowest = std::min(lowest, c.currentMhz);
            highest = std::max(highest, c.currentMhz);
            if (c.parked >= 0) ++knownParking;
            if (c.parked == 1) ++parked;
            Row r;
            r.tag = i;
            if (c.parked == 1) r.accent = g_pal.textFaint;
            r.Add(Format(L"CPU %zu", i), double(i))
                .Add(c.parked == 1 ? L"Parked" : c.parked == 0 ? L"Active" : L"—", double(c.parked))
                .Add(Format(L"%.0f%%", c.usage), c.usage)
                .Add(FormatCount(c.currentMhz), c.currentMhz)
                .Add(FormatCount(c.nominalMhz), c.nominalMhz)
                .Add(FormatCount(c.limitMhz), c.limitMhz)
                .Add(Format(L"%.0f%%", c.performance), c.performance);
            rows.push_back(std::move(r));
        }
        if (split_) {  // first half on the left, the rest on the right
            const size_t half = (rows.size() + 1) / 2;
            std::vector<Row> second(std::make_move_iterator(rows.begin() + static_cast<std::ptrdiff_t>(half)),
                                    std::make_move_iterator(rows.end()));
            rows.resize(half);
            cores2_.SetRows(std::move(second));
        }
        cores_.SetRows(std::move(rows));

        std::wstring active;
        for (const PowerPlan& p : list_)
            if (p.active) active = p.name;
        planCard_.SetText(active.empty() ? L"—" : active, PowerSourceText(), Format(L"%zu plans on this PC", list_.size()));

        std::wstring setting;
        if (!parking_.known) setting = L"Setting unavailable";
        else if (parking_.DisabledAc() && parking_.DisabledDc()) setting = L"Disabled: all cores stay awake";
        else
            setting = std::wstring(L"Plugged in: ") + (parking_.DisabledAc() ? L"never parks" : L"may park") +
                      L"  ·  Battery: " + (parking_.DisabledDc() ? L"never parks" : L"may park");
        parkCard_.SetText(knownParking ? (parked ? Format(L"%d of %zu parked", parked, cores.size()) : L"None parked")
                                       : L"Not reported",
                          setting, OnBattery() ? L"Live · on battery" : L"Live · plugged in");
        SetText(parkLabel_, L"This plan: " + setting + L".  Hover the info icon for what parking does.");
        clockCard_.SetText(Format(L"%.2f GHz", Sys().avgMhz / 1000.0),
                           Format(L"Range %.2f–%.2f GHz", cores.empty() ? 0.0 : lowest / 1000.0, highest / 1000.0),
                           Format(L"Base %.2f GHz · %zu threads", nominal / 1000.0, cores.size()));
        if (!busy_ && status_ && GetText(status_).empty())
            SetText(status_, L"Pick a plan and press Use this plan, or add one of the presets below.");
        UpdateButtons();
    }

    ActionCard planCard_, parkCard_, clockCard_;
    HWND status_ = nullptr, activate_ = nullptr, delete_ = nullptr, presetPick_ = nullptr, addPreset_ = nullptr,
         parkOff_ = nullptr, parkOn_ = nullptr, parkLabel_ = nullptr;
    Table plans_, cores_, cores2_;
    bool split_ = false, visible_ = false;
    TextCard presetInfo_;
    InfoBadge parkInfo_;
    std::vector<PowerPlan> list_;
    ParkingState parking_;
    bool busy_ = false;
    unsigned ticks_ = 0;
    std::jthread worker_;  // last: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakePowerPage() { return std::make_unique<PowerPage>(); }

}  // namespace mtm
