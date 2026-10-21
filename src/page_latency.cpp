#include "graph.h"
#include "latency.h"
#include "page.h"
#include "power.h"
#include "table.h"
#include "theme.h"
#include "tweaks.h"

#include <algorithm>
#include <thread>

namespace mtm {
namespace {

std::wstring Micros(double us) { return us >= 10000 ? Format(L"%.1f ms", us / 1000) : Format(L"%.0f µs", us); }

class LatencyPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Latency test"; }

    void Create() override {
        status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        start_ = CreateButton(L"\xE768  Start test", true);
        cancel_ = CreateButton(L"\xE711  Cancel");
        stall_ = CreateButton(L"\xE9D9  Stall check");
        stalls_.Create({{L"Stalls (every CPU watched for 20 s)", 390},
                        {L"Worst", 100, true},
                        {L"≥ 100 µs", 100, true},
                        {L"≥ 500 µs", 100, true},
                        {L"≥ 1 ms", 100, true},
                        {L"Time lost", 100, true}},
                       2, false);
        table_.Create({{L"Thread-scheduling setting", 390},
                       {L"Typical", 100, true},
                       {L"99th percentile", 130, true},
                       {L"Worst", 100, true},
                       {L"Over 1 ms", 100, true}},
                      2, false);
        notes_.Create(L"How to read this");
        notes_.SetSections(
            {{L"What this measures",
              L"Like a DPC latency checker, it asks Windows to wake a thread after 1 ms and records how late the wake-up "
              L"is, in microseconds. It does this while every CPU core is busy with background work, because on an idle "
              L"PC all settings give the same answer. Lower is better."},
             {L"How the test runs",
              L"Each thread-scheduling setting is applied in turn (one administrator prompt) and measured for about "
              L"4 seconds, twice, in opposite orders. Your own setting is put back afterwards. It takes about 2 minutes "
              L"with the CPU at 100%: close games first and keep Kernova in front."},
             {L"Reading the result",
              L"The 99th percentile means 99 of 100 wake-ups were faster than that. It is steadier than the worst case. "
              L"A difference under about 25% is noise: run it again and see whether the same setting wins. If every row "
              L"looks alike, either this PC doesn't care, or Windows only reads the new value after a restart."},
             {L"What it can't tell you",
              L"It measures how quickly a waiting thread gets the CPU under load. It is not frame time or click-to-screen "
              L"latency, and it isn't a game. Use it to compare settings on your own PC; it can't say which one suits a "
              L"particular game."},
             {L"Stall check",
              L"A separate 20-second test, no administrator prompt. One thread per CPU reads the clock in a tight loop at "
              L"top priority; a gap means that CPU was taken away by an interrupt or DPC routine, by firmware (SMI), by "
              L"power management or by the hypervisor. A gap on most CPUs at the same moment froze the whole PC: that is "
              L"BIOS, firmware or power management, and no driver update fixes it. A gap on one CPU is local: a driver's "
              L"interrupt or DPC routine. Over 500 µs hurts real-time audio; over 1 ms can be heard. The PC stays "
              L"usable (the threads rest 10 ms in every 100) but run it with nothing else going on, and with "
              L"real-time-priority programs closed, since they show up as gaps too. It can't name the driver: use "
              L"LatencyMon's Drivers tab or a Windows Performance Recorder trace for that."}});
        SetText(status_, L"Press Start to compare the thread-scheduling settings on this PC.");
        UpdateButtons();
    }

    int MinHeight(int) const override { return Scale(760); }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(150), gap = Scale(8), notesH = Scale(210), stallsH = Scale(170);
        Place(start_, a.right - btn, a.top, btn, h);
        Place(stall_, a.right - 2 * btn - gap, a.top, btn, h);
        Place(cancel_, a.right - 3 * btn - 2 * gap, a.top, btn, h);
        Place(status_, a.left, a.top, a.right - a.left - 3 * btn - 3 * gap, h);
        const int top = a.top + h + Scale(14);
        const int tableH = std::max<int>(Scale(140), a.bottom - top - notesH - stallsH - Scale(24));
        Place(table_.Handle(), a.left, top, a.right - a.left, tableH);
        Place(stalls_.Handle(), a.left, top + tableH + Scale(12), a.right - a.left, stallsH);
        Place(notes_.Handle(), a.left, top + tableH + stallsH + Scale(24), a.right - a.left, notesH);
    }

    void Show(bool v) override {
        ShowAll({status_, start_, stall_, cancel_, table_.Handle(), stalls_.Handle(), notes_.Handle()}, v);
    }

    void OnActivate() override {
        UpdateButtons();
        CheckInterrupted();
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        return table_.HandleNotify(nm, result) || stalls_.HandleNotify(nm, result);
    }

    bool OnCommand(HWND control, WORD code) override {
        if (code != BN_CLICKED) return false;
        if (control == start_) Start();
        else if (control == stall_) StartStalls();
        else if (control == cancel_) worker_.request_stop();
        else return false;
        return true;
    }

private:
    void UpdateButtons() {
        EnableWindow(start_, !busy_);
        EnableWindow(stall_, !busy_);
        EnableWindow(cancel_, sweeping_);
    }

    void StartStalls() {
        if (busy_) return;
        if (!Confirm(L"The stall check watches every CPU for 20 seconds at top priority. The PC stays usable but may feel "
                     L"slightly sluggish.\n\nClose games and any real-time-priority programs first, and leave the PC alone "
                     L"while it runs.\n\nStart?"))
            return;
        busy_ = sweeping_ = true;  // sweeping_ = "a test can be cancelled"
        UpdateButtons();
        stalls_.SetRows({});
        SetText(status_, L"Watching every CPU…");
        worker_ = std::jthread([this](std::stop_token st) {
            StallReport r = MeasureStalls(20000, st, [this](DWORD left) {
                PostToUi([this, left] { SetText(status_, Format(L"Watching every CPU… %lu s left", left)); });
            });
            PostToUi([this, r = std::move(r)] { FinishStalls(r); });
        });
    }

    void FinishStalls(const StallReport& r) {
        busy_ = sweeping_ = false;
        UpdateButtons();
        if (!r.ok) {
            SetText(status_, L"The stall check couldn't start.");
            return;
        }
        std::vector<Row> rows;
        auto flag = [](double worstUs) { return worstUs >= 1000 ? g_pal.bad : worstUs >= 500 ? g_pal.warn : CLR_NONE; };
        Row all;
        all.accent = flag(r.wideWorstUs);
        all.Add(Format(L"Whole PC at once (%zu events)", r.wideEvents))
            .Add(r.wideEvents ? Micros(r.wideWorstUs) : L"none", r.wideWorstUs)
            .Add(Format(L"%zu", r.wideOver[1]), double(r.wideOver[1]))
            .Add(Format(L"%zu", r.wideOver[3]), double(r.wideOver[3]))
            .Add(Format(L"%zu", r.wideOver[4]), double(r.wideOver[4]))
            .Add(L"—", 0);
        rows.push_back(std::move(all));
        for (const CpuStalls& c : r.cpus) {
            Row row;
            row.accent = flag(c.worstUs);
            row.Add(r.cpus.size() > 1 && c.group ? Format(L"CPU %u (group %u)", c.number, c.group) : Format(L"CPU %u", c.number))
                .Add(c.worstUs > 0 ? Micros(c.worstUs) : L"none", c.worstUs)
                .Add(Format(L"%zu", c.over[1]), double(c.over[1]))
                .Add(Format(L"%zu", c.over[3]), double(c.over[3]))
                .Add(Format(L"%zu", c.over[4]), double(c.over[4]))
                .Add(Format(L"%.3f%%", c.lostPct), c.lostPct);
            rows.push_back(std::move(row));
        }
        stalls_.SetRows(std::move(rows));
        SetText(status_, (r.cancelled ? L"Cancelled early, partial results.  " : L"") + StallVerdict(r));
    }

    static std::wstring StallVerdict(const StallReport& r) {
        const double worst = std::max(r.wideWorstUs, r.localWorstUs);
        if (worst < 250) return Format(L"Clean: no gap over 250 µs in %.0f s on %d CPUs.", r.watchedSec, r.cpuCount);
        std::wstring s;
        if (r.wideWorstUs >= 250)
            s += Format(L"%zu whole-PC stalls, worst %s: firmware (SMI), power management or the hypervisor, not a driver. "
                        L"Try a BIOS update and turning off deep C-states or CPU throttling.  ",
                        r.wideEvents, Micros(r.wideWorstUs).c_str());
        if (r.localWorstUs >= 250)
            s += Format(L"%zu single-CPU gaps, worst %s: an interrupt or DPC routine. Name the driver with LatencyMon's "
                        L"Drivers tab.",
                        r.localEvents, Micros(r.localWorstUs).c_str());
        return s;
    }

    void Start() {
        if (busy_) return;
        if (!Confirm(L"The test keeps every CPU core busy for about 2 minutes and changes the Windows thread-scheduling "
                     L"setting step by step (one administrator prompt). Your setting is put back afterwards.\n\n"
                     L"Close games and other heavy programs, and keep Kernova in front while it runs.\n\nStart?"))
            return;
        std::vector<DWORD> values = LatencyTestValues();
        const QuantumState q = ReadQuantum();
        current_ = q.exists ? q.value : 2;  // not set = Windows' default
        if (std::find(values.begin(), values.end(), current_) == values.end() &&
            ValidTweakEntry({L"quantum", std::to_wstring(current_)}))
            values.insert(values.begin(), current_);  // so your own setting is in the comparison

        busy_ = sweeping_ = true;
        UpdateButtons();
        table_.SetRows({});
        SetText(status_, L"Waiting for the administrator prompt…");
        worker_ = std::jthread([this, values = std::move(values)](std::stop_token st) {
            SweepOutcome o = RunLatencySweep(values, st, [this](const std::wstring& text) {
                PostToUi([this, text] { SetText(status_, text); });
            });
            PostToUi([this, o = std::move(o)] { Finish(o); });
        });
    }

    void Finish(const SweepOutcome& o) {
        busy_ = sweeping_ = false;
        UpdateButtons();
        std::vector<Row> rows;
        const LatencyRow* best = nullptr;
        const LatencyRow* worst = nullptr;
        for (const LatencyRow& r : o.rows) {
            if (r.inFront && (!best || r.stats.p99Us < best->stats.p99Us)) best = &r;
            if (r.inFront && (!worst || r.stats.p99Us > worst->stats.p99Us)) worst = &r;
        }
        for (const LatencyRow& r : o.rows) {
            Row row;
            row.tag = r.value;
            row.accent = &r == best ? g_pal.good : CLR_NONE;
            std::wstring label = DescribeQuantum(r.value);
            if (r.value == current_) label += L"  ·  your setting";
            if (!r.inFront) label += L"  ·  Kernova wasn't in front: not reliable";
            row.Add(label)
                .Add(Micros(r.stats.medianUs), r.stats.medianUs)
                .Add(Micros(r.stats.p99Us), r.stats.p99Us)
                .Add(Micros(r.stats.maxUs), r.stats.maxUs)
                .Add(Format(L"%.1f%%", r.stats.lateFraction * 100), r.stats.lateFraction * 100);
            rows.push_back(std::move(row));
        }
        table_.SetRows(std::move(rows));

        if (o.error != ERROR_SUCCESS && o.rows.empty()) {
            SetText(status_, o.error == ERROR_CANCELLED ? L"Not run: the administrator prompt was cancelled."
                                                        : L"The test couldn't run: " + ErrorText(o.error));
        } else if (o.rows.empty()) {
            SetText(status_, o.cancelled ? L"Cancelled." : L"No measurements were taken.");
        } else {
            SetText(status_, (o.cancelled ? L"Cancelled early, partial results.  " : L"") + Verdict(best, worst));
        }
        if (o.restoreNeeded) {
            asked_ = false;  // the original value is still parked: offer to put it back
            CheckInterrupted();
        }
    }

    static std::wstring Verdict(const LatencyRow* best, const LatencyRow* worst) {
        if (!best || !worst || best == worst) return L"Not enough reliable measurements to compare. Keep Kernova in front and try again.";
        const double spread = (worst->stats.p99Us - best->stats.p99Us) / std::max(best->stats.p99Us, 1.0);
        if (spread < 0.25)
            return Format(L"No clear difference: the 99th percentile varies by only %.0f%% between settings.", spread * 100);
        return Format(L"Fastest here: %s, 99th percentile %s against %s for the slowest. Run it again to confirm.",
                      DescribeQuantum(best->value).c_str(), Micros(best->stats.p99Us).c_str(),
                      Micros(worst->stats.p99Us).c_str());
    }

    // A test that was killed leaves the scheduling setting at one of its test values (the original is
    // parked in the registry). Offer to put it back, once per visit.
    void CheckInterrupted() {
        if (busy_ || asked_ || !ParkedDwordExists(HKEY_LOCAL_MACHINE, kPriorityControlKey, kParkedQuantum)) return;
        asked_ = true;
        if (!Confirm(L"A latency test was interrupted, so the thread-scheduling setting may still be at one of its test "
                     L"values.\n\nPut your original setting back now? (administrator prompt)"))
            return;
        busy_ = true;
        UpdateButtons();
        SetText(status_, L"Putting your original setting back…");
        worker_ = std::jthread([this](std::stop_token) {
            const DWORD r = RunSelfElevated(L"--tweak", 60000);
            PostToUi([this, r] {
                busy_ = false;
                UpdateButtons();
                SetText(status_, r == ERROR_SUCCESS ? L"Your original setting is back." : L"Couldn't put it back: " + ErrorText(r));
            });
        });
    }

    HWND status_ = nullptr, start_ = nullptr, stall_ = nullptr, cancel_ = nullptr;
    Table table_, stalls_;
    TextCard notes_;
    DWORD current_ = 2;
    bool busy_ = false, sweeping_ = false, asked_ = false;
    std::jthread worker_;  // last: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeLatencyPage() { return std::make_unique<LatencyPage>(); }

}  // namespace mtm
