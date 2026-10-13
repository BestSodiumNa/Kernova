#include "benchmarks.h"
#include "page.h"
#include "graph.h"
#include "sysinfo.h"
#include "table.h"
#include "theme.h"

#include <shlobj.h>

#include <deque>
#include <fstream>
#include <algorithm>
#include <thread>

namespace mtm {
namespace {

enum Kind { kCpu, kGpu, kDisk, kKinds };

Kind KindOf(const BenchResult& r) {
    if (r.test == L"CPU") return kCpu;
    if (r.test == L"GPU") return kGpu;
    return kDisk;
}

Graph::Tint TintOf(Kind k) { return k == kCpu ? Graph::Tint::Cpu : k == kGpu ? Graph::Tint::Gpu : Graph::Tint::Disk; }

// ---------------------------------------------------------------- history file
// Results are kept in %LOCALAPPDATA%\Kernova\benchmarks.dat so they survive restarts
// (copied once from the folder of an earlier app name).

constexpr uint32_t kHistoryMagic = 0x4842544D;  // "MTBH"
constexpr uint32_t kHistoryVersion = 1;
constexpr size_t kHistoryMax = 100;

std::wstring HistoryPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Kernova";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\benchmarks.dat";
        for (const wchar_t* old : {L"\\BetterTaskManager\\benchmarks.dat", L"\\MyTaskManager\\benchmarks.dat"})
            CopyFileW((std::wstring(base) + old).c_str(), path.c_str(), TRUE);  // no-op once the new file exists
    }
    CoTaskMemFree(base);
    return path;
}

class Writer {
public:
    explicit Writer(std::ofstream& f) : f_(f) {}
    template <class T> void Pod(const T& v) { f_.write(reinterpret_cast<const char*>(&v), sizeof v); }
    void Str(const std::wstring& s) {
        Pod(static_cast<uint32_t>(s.size()));
        f_.write(reinterpret_cast<const char*>(s.data()), static_cast<std::streamsize>(s.size() * sizeof(wchar_t)));
    }

private:
    std::ofstream& f_;
};

class Reader {
public:
    explicit Reader(std::ifstream& f) : f_(f) {}
    template <class T> bool Pod(T& v) { return static_cast<bool>(f_.read(reinterpret_cast<char*>(&v), sizeof v)); }
    bool Str(std::wstring& s) {
        uint32_t n = 0;
        if (!Pod(n) || n > 1u << 16) return false;
        s.resize(n);
        return static_cast<bool>(f_.read(reinterpret_cast<char*>(s.data()), static_cast<std::streamsize>(n * sizeof(wchar_t))));
    }

private:
    std::ifstream& f_;
};

void SaveHistory(const std::deque<BenchResult>& results) {
    const std::wstring path = HistoryPath();
    if (path.empty()) return;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return;
    Writer w(f);
    w.Pod(kHistoryMagic);
    w.Pod(kHistoryVersion);
    w.Pod(static_cast<uint32_t>(results.size()));
    for (const BenchResult& r : results) {
        w.Str(r.test); w.Str(r.device); w.Str(r.score); w.Str(r.details); w.Str(r.headline);
        w.Pod(r.value); w.Pod(r.when); w.Pod(static_cast<uint8_t>(r.ok));
        w.Pod(static_cast<uint32_t>(r.metrics.size()));
        for (const auto& [k, v] : r.metrics) { w.Str(k); w.Str(v); }
        w.Pod(static_cast<uint32_t>(r.timeline.size()));
        for (double v : r.timeline) w.Pod(v);
        w.Str(r.timelineUnit); w.Str(r.timelineNote);
    }
}

std::deque<BenchResult> LoadHistory() {
    std::deque<BenchResult> out;
    std::ifstream f(HistoryPath(), std::ios::binary);
    if (!f) return out;
    Reader rd(f);
    uint32_t magic = 0, version = 0, count = 0;
    if (!rd.Pod(magic) || magic != kHistoryMagic || !rd.Pod(version) || version != kHistoryVersion || !rd.Pod(count))
        return out;
    for (uint32_t i = 0; i < count && i < kHistoryMax; ++i) {
        BenchResult r;
        uint8_t ok = 0;
        uint32_t n = 0;
        if (!rd.Str(r.test) || !rd.Str(r.device) || !rd.Str(r.score) || !rd.Str(r.details) || !rd.Str(r.headline) ||
            !rd.Pod(r.value) || !rd.Pod(r.when) || !rd.Pod(ok) || !rd.Pod(n) || n > 256)
            break;
        r.ok = ok != 0;
        bool good = true;
        for (uint32_t m = 0; m < n && good; ++m) {
            std::wstring k, v;
            good = rd.Str(k) && rd.Str(v);
            r.metrics.emplace_back(std::move(k), std::move(v));
        }
        if (!good || !rd.Pod(n) || n > 100000) break;
        r.timeline.resize(n);
        for (double& v : r.timeline) good = good && rd.Pod(v);
        if (!good || !rd.Str(r.timelineUnit) || !rd.Str(r.timelineNote)) break;
        out.push_back(std::move(r));
    }
    return out;
}

// ---------------------------------------------------------------- page

class BenchmarksPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Benchmarks"; }

    void Create() override {
        static const wchar_t kGlyphs[kKinds] = {0xE950, 0xF211, 0xEDA2};  // processor, GPU chip, hard drive
        static const wchar_t* kNames[kKinds] = {L"Processor", L"Graphics", L"Storage"};
        for (int k = 0; k < kKinds; ++k)
            cards_[k].Create(kGlyphs[k], kNames[k], TintOf(Kind(k)), [this, k] { ShowLatest(Kind(k)); });
        run_[kCpu] = CreateButton(L"\xE768  Run CPU test", true);
        run_[kGpu] = CreateButton(L"\xE768  Run", true);
        run_[kDisk] = CreateButton(L"\xE768  Run", true);
        threads_ = CreateLabel(L"", SS_CENTERIMAGE);
        gpuPick_ = CreateCombo({});
        drive_ = CreateCombo({});
        runAll_ = CreateButton(L"\xE8B1  Run all");
        stop_ = CreateButton(L"\xE71A  Stop");
        EnableWindow(stop_, FALSE);
        progress_.Create();
        status_ = CreateLabel(L"Each test takes 5–15 seconds. Close heavy apps for steadier results. Click a result for details.",
                              SS_CENTERIMAGE | SS_ENDELLIPSIS);
        table_.Create({{L"Test", 90}, {L"Score", 130, true}, {L"Device", 230}, {L"When", 150}}, -1, false);
        metrics_.Create(L"Result details");
        chart_.Create(Graph::Tint::Cpu);

        SetText(threads_, Format(L"%u threads · %s", std::max(1u, std::thread::hardware_concurrency()),
                                 CpuBrandName().c_str()));
        FillGpus();
        FillDrives();
        results_ = LoadHistory();
        RefreshHistory();
        if (!results_.empty()) ShowResult(0);
        else ShowEmptyDetails();
    }

    // Cards + run controls + a history list and detail panel worth reading.
    int MinHeight(int) const override { return Scale(760); }

    void Layout(RECT a) override {
        const int gap = Scale(12), h = Scale(34), cardH = Scale(146);
        const int colW = (a.right - a.left - gap * 2) / 3;
        for (int k = 0; k < kKinds; ++k) {
            const int x = a.left + k * (colW + gap);
            Place(cards_[k].Handle(), x, a.top, colW, cardH);
            const int y = a.top + cardH + Scale(8);
            const int btn = k == kCpu ? Scale(150) : Scale(90);
            Place(run_[k], x + colW - btn, y, btn, h);
            const int comboW = colW - btn - Scale(8);
            if (k == kCpu) Place(threads_, x + Scale(4), y, comboW - Scale(4), h);
            else Place(k == kGpu ? gpuPick_ : drive_, x, y, comboW, Scale(240));
        }
        const int y2 = a.top + cardH + Scale(8) + h + gap;
        const int btn = Scale(110);
        Place(stop_, a.right - btn, y2, btn, h);
        Place(runAll_, a.right - btn * 2 - Scale(8), y2, btn, h);
        Place(progress_.Handle(), a.left, y2 + (h - Scale(8)) / 2, Scale(200), Scale(8));
        Place(status_, a.left + Scale(214), y2, a.right - btn * 2 - Scale(24) - a.left - Scale(214), h);

        const int top = y2 + h + gap;
        const int tableW = (a.right - a.left) * 9 / 20;
        Place(table_.Handle(), a.left, top, tableW, a.bottom - top);
        const int dx = a.left + tableW + gap, dw = a.right - dx;
        const int metricsH = (a.bottom - top) * 11 / 20;
        Place(metrics_.Handle(), dx, top, dw, metricsH);
        Place(chart_.Handle(), dx, top + metricsH + gap, dw, a.bottom - top - metricsH - gap);
    }

    void Show(bool v) override {
        std::vector<HWND> all{threads_, gpuPick_, drive_, runAll_, stop_, progress_.Handle(), status_, table_.Handle(),
                              metrics_.Handle(), chart_.Handle()};
        for (int k = 0; k < kKinds; ++k) {
            all.push_back(cards_[k].Handle());
            all.push_back(run_[k]);
        }
        for (HWND h : all) ShowWindow(h, v ? SW_SHOW : SW_HIDE);
    }

    void OnActivate() override {
        if (!running_) FillDrives();
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override {
        if (table_.HandleNotify(nm, result)) return true;
        if (nm->hwndFrom == table_.Handle() && nm->code == LVN_ITEMCHANGED) {
            const auto* lv = reinterpret_cast<NMLISTVIEW*>(nm);
            if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
                if (const Row* row = table_.SelectedRow()) ShowById(row->tag);
            return true;
        }
        return false;
    }

    bool OnCommand(HWND control, WORD code) override {
        if (code == CBN_SELCHANGE && control == gpuPick_) {
            const int sel = static_cast<int>(SendMessageW(gpuPick_, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel < static_cast<int>(gpus_.size())) {
                WriteSetting(L"BenchGpuLuidLow", gpus_[size_t(sel)].luid.LowPart);
                UpdateCards();
            }
            return true;
        }
        if (code != BN_CLICKED) return false;
        if (control == run_[kCpu]) Start({kCpu});
        else if (control == run_[kGpu]) Start({kGpu});
        else if (control == run_[kDisk]) Start({kDisk});
        else if (control == runAll_) Start({kCpu, kGpu, kDisk});
        else if (control == stop_) worker_.request_stop();
        else return false;
        return true;
    }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* row = table_.SelectedRow();
        if (!row) return true;
        // Copy what we need now: a benchmark finishing while the menu is open replaces the rows.
        const uint64_t id = row->tag;
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Copy result");
        AppendMenuW(menu, MF_STRING, 2, L"Copy all details");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (running_ ? MF_GRAYED : 0), 3, L"Delete result");
        AppendMenuW(menu, MF_STRING | (running_ ? MF_GRAYED : 0), 4, L"Clear history");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        const BenchResult* r = Find(id);
        if (!r) return true;
        if (cmd == 1) {
            CopyToClipboard(r->test + L": " + r->score + L" (" + r->device + L")");
        } else if (cmd == 2) {
            std::wstring text = r->test + L" benchmark — " + FormatFileTime(r->when) + L"\r\n" + r->score + L"\r\n";
            for (const auto& [k, v] : r->metrics) text += k + L": " + v + L"\r\n";
            CopyToClipboard(text);
        } else if (cmd == 3 && !running_) {
            results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(results_.size() - id));
            Changed();
        } else if (cmd == 4 && !running_ && Confirm(L"Delete all saved benchmark results?")) {
            results_.clear();
            Changed();
        }
        return true;
    }

private:
    // ids are positions counted from the oldest result, so they stay stable as new ones arrive.
    uint64_t IdOf(size_t index) const { return results_.size() - index; }
    const BenchResult* Find(uint64_t id) const {
        if (id == 0 || id > results_.size()) return nullptr;
        return &results_[results_.size() - id];
    }

    void Changed() {
        SaveHistory(results_);
        RefreshHistory();
        if (!results_.empty()) ShowResult(0);
        else ShowEmptyDetails();
    }

    void FillGpus() {
        gpus_ = EnumerateGpus();
        SendMessageW(gpuPick_, CB_RESETCONTENT, 0, 0);
        const DWORD saved = ReadSetting(L"BenchGpuLuidLow", 0);
        int pick = 0;
        for (size_t i = 0; i < gpus_.size(); ++i) {
            const GpuAdapter& g = gpus_[i];
            const std::wstring text = g.name + (g.integrated ? L"  (integrated)" : L"  (dedicated, " + FormatBytes(g.dedicatedVram) + L")");
            SendMessageW(gpuPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (saved && g.luid.LowPart == saved) pick = static_cast<int>(i);
        }
        if (gpus_.empty()) {
            SendMessageW(gpuPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No Direct3D 11 GPU found"));
            EnableWindow(run_[kGpu], FALSE);
        }
        SendMessageW(gpuPick_, CB_SETCURSEL, pick, 0);
    }

    void FillDrives() {
        const int prev = static_cast<int>(SendMessageW(drive_, CB_GETCURSEL, 0, 0));
        SendMessageW(drive_, CB_RESETCONTENT, 0, 0);
        drives_.clear();
        wchar_t buf[512] = {};
        GetLogicalDriveStringsW(ARRAYSIZE(buf), buf);
        for (const wchar_t* d = buf; *d; d += wcslen(d) + 1) {
            const UINT type = GetDriveTypeW(d);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
            wchar_t label[MAX_PATH] = {};
            GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
            ULARGE_INTEGER freeBytes{};
            GetDiskFreeSpaceExW(d, &freeBytes, nullptr, nullptr);
            const std::wstring text = Format(L"%s %s(%s free)", d, label[0] ? (std::wstring(label) + L" ").c_str() : L"",
                                             FormatBytes(freeBytes.QuadPart).c_str());
            SendMessageW(drive_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            drives_.push_back(d);
        }
        SendMessageW(drive_, CB_SETCURSEL, prev >= 0 && prev < static_cast<int>(drives_.size()) ? prev : 0, 0);
    }

    void SetRunning(bool running) {
        running_ = running;
        for (HWND h : {run_[kCpu], run_[kGpu], run_[kDisk], runAll_, drive_, gpuPick_}) EnableWindow(h, !running);
        if (!running && gpus_.empty()) EnableWindow(run_[kGpu], FALSE);
        EnableWindow(stop_, running);
    }

    void Start(std::vector<Kind> kinds) {
        if (running_) return;
        const int dsel = static_cast<int>(SendMessageW(drive_, CB_GETCURSEL, 0, 0));
        const int gsel = static_cast<int>(SendMessageW(gpuPick_, CB_GETCURSEL, 0, 0));
        const bool wantDisk = std::find(kinds.begin(), kinds.end(), kDisk) != kinds.end();
        const bool wantGpu = std::find(kinds.begin(), kinds.end(), kGpu) != kinds.end();
        if (wantDisk && (dsel < 0 || dsel >= static_cast<int>(drives_.size()))) {
            ShowError(L"Pick a drive to test first.");
            return;
        }
        if (wantGpu && (gsel < 0 || gsel >= static_cast<int>(gpus_.size()))) {
            kinds.erase(std::remove(kinds.begin(), kinds.end(), kGpu), kinds.end());  // no GPU: skip it in "Run all"
            if (kinds.empty()) return;
        }
        const std::wstring drive = wantDisk ? drives_[size_t(dsel)] : L"";
        const GpuAdapter gpu = gsel >= 0 && gsel < static_cast<int>(gpus_.size()) ? gpus_[size_t(gsel)] : GpuAdapter{};
        SetRunning(true);
        progress_.SetFraction(0);
        SetText(status_, L"Starting…");
        worker_ = std::jthread([this, kinds, drive, gpu](std::stop_token st) {
            const double parts = double(kinds.size());
            for (size_t i = 0; i < kinds.size() && !st.stop_requested(); ++i) {
                const BenchProgress progress = [this, i, parts](double f, const std::wstring& text) {
                    PostToUi([this, f = (double(i) + f) / parts, text] {
                        progress_.SetFraction(f);
                        SetText(status_, text);
                    });
                };
                BenchResult r = kinds[i] == kCpu ? RunCpuBenchmark(st, progress)
                              : kinds[i] == kGpu ? RunGpuBenchmark(gpu, st, progress)
                                                 : RunDiskBenchmark(drive, st, progress);
                const bool last = i + 1 == kinds.size() || st.stop_requested() || !r.ok;
                PostToUi([this, r = std::move(r), last]() mutable { Finished(std::move(r), last); });
                if (last) break;
            }
        });
    }

    void Finished(BenchResult r, bool last) {
        if (last) SetRunning(false);
        if (last) progress_.SetFraction(r.ok ? 1.0 : 0.0);
        SetText(status_, r.ok ? L"Done: " + r.test + L" — " + r.score
                              : r.test + L": " + r.score + (r.details.empty() ? L"" : L" — " + r.details));
        if (!r.ok) return;  // failures and cancellations aren't kept
        results_.push_front(std::move(r));
        while (results_.size() > kHistoryMax) results_.pop_back();
        SaveHistory(results_);
        RefreshHistory();
        ShowResult(0);
    }

    // Best score of the same test on the same device.
    double BestFor(const BenchResult& r) const {
        double best = 0;
        for (const BenchResult& o : results_)
            if (o.test == r.test && o.device == r.device) best = std::max(best, o.value);
        return best;
    }

    void RefreshHistory() {
        std::vector<Row> rows;
        for (size_t i = 0; i < results_.size(); ++i) {
            const BenchResult& r = results_[i];
            Row row;
            row.tag = IdOf(i);
            row.accent = TintColorOf(KindOf(r));
            row.Add(r.test).Add(r.headline.empty() ? r.score : r.headline, r.value);
            row.Add(r.device).Add(FormatFileTime(r.when), double(r.when));
            rows.push_back(std::move(row));
        }
        table_.SetRows(std::move(rows));
        UpdateCards();
    }

    static COLORREF TintColorOf(Kind k) { return k == kCpu ? g_pal.cpu : k == kGpu ? g_pal.gpu : g_pal.disk; }

    void UpdateCards() {
        for (int k = 0; k < kKinds; ++k) {
            const BenchResult* latest = nullptr;
            for (const BenchResult& r : results_)
                if (KindOf(r) == k) { latest = &r; break; }
            std::wstring device;
            if (k == kCpu) device = CpuBrandName();
            else if (k == kGpu) {
                const int sel = static_cast<int>(SendMessageW(gpuPick_, CB_GETCURSEL, 0, 0));
                device = sel >= 0 && sel < static_cast<int>(gpus_.size()) ? gpus_[size_t(sel)].name : L"No GPU";
            } else {
                device = L"Sequential read · write · random 4K";
            }
            if (!latest) {
                cards_[k].SetText(L"", L"Not run yet — press Run", device);
                continue;
            }
            const double best = BestFor(*latest);
            std::wstring line = L"Last run " + FormatFileTime(latest->when);
            if (best > latest->value * 1.005) line += Format(L" · best %.0f", best);
            cards_[k].SetText(latest->headline, line, k == kDisk ? latest->device : device);
        }
    }

    void ShowLatest(Kind k) {
        for (size_t i = 0; i < results_.size(); ++i)
            if (KindOf(results_[i]) == k) {
                table_.SelectTag(IdOf(i));
                ShowResult(i);
                return;
            }
        for (int c = 0; c < kKinds; ++c) cards_[c].SetSelected(c == k);
        SetText(status_, L"No result for this test yet — press Run below the card.");
    }

    void ShowById(uint64_t id) {
        if (id == 0 || id > results_.size()) return;
        ShowResult(results_.size() - id);
    }

    void ShowEmptyDetails() {
        for (auto& c : cards_) c.SetSelected(false);
        metrics_.SetLines({{L"Run a test above, then click any result to see everything it measured.", L""}});
        chart_.SetSeries({}, 1, L"Throughput over time", L"", L"");
    }

    void ShowResult(size_t index) {
        const BenchResult& r = results_[index];
        const Kind kind = KindOf(r);
        for (int c = 0; c < kKinds; ++c) cards_[c].SetSelected(c == kind);
        if (table_.SelectedRow() == nullptr || table_.SelectedRow()->tag != IdOf(index)) table_.SelectTag(IdOf(index));

        std::vector<ListCard::Line> lines;
        const COLORREF color = TintColorOf(kind);
        // Score vs. the best run on this device, as a meter.
        const double best = BestFor(r);
        ListCard::Line score{L"Score", r.headline, best > 0 ? r.value / best : 1.0, color};
        if (best > r.value * 1.005) score.label = Format(L"Score  ·  %.0f%% of best (%.0f)", 100.0 * r.value / best, best);
        else score.label = L"Score  ·  best on this device";
        lines.push_back(std::move(score));
        for (const auto& [k, v] : r.metrics) lines.push_back({k, v});
        metrics_.SetLines(std::move(lines), FormatFileTime(r.when));

        chart_.SetTint(TintOf(kind));
        chart_.SetSeries(r.timeline, 0.25, r.test + L" throughput · " + r.timelineUnit, r.timelineUnit, r.timelineNote);
    }

    ActionCard cards_[kKinds];
    HWND run_[kKinds] = {};
    HWND threads_ = nullptr, gpuPick_ = nullptr, drive_ = nullptr, runAll_ = nullptr, stop_ = nullptr;
    Bar progress_;
    HWND status_ = nullptr;
    Table table_;
    ListCard metrics_;
    Chart chart_;
    std::vector<std::wstring> drives_;
    std::vector<GpuAdapter> gpus_;
    std::deque<BenchResult> results_;  // newest first
    bool running_ = false;
    std::jthread worker_;  // last member: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeBenchmarksPage() { return std::make_unique<BenchmarksPage>(); }

}  // namespace mtm
