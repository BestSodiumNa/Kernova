// page_recorder.cpp
#include "page.h"
#include "recorder.h"
#include "table.h"
#include "theme.h"

#include <thread>

namespace mtm {
namespace {

// Thin trackbar wrappers (the trackbar reports moves via WM_HSCROLL -> Page::OnScroll).
HWND CreateTrackbar(int minPos, int maxPos) {
    HWND h = CreateChild(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_HORZ | TBS_NOTICKS);
    SendMessageW(h, TBM_SETRANGEMIN, FALSE, minPos);
    SendMessageW(h, TBM_SETRANGEMAX, TRUE, maxPos);
    ThemeSlider(h);  // rounded track + round knob (drawn in CustomDrawSlider)
    return h;
}
void SetTrackbarRange(HWND h, int minPos, int maxPos) {
    SendMessageW(h, TBM_SETRANGEMIN, FALSE, minPos);
    SendMessageW(h, TBM_SETRANGEMAX, TRUE, maxPos);
}
void SetTrackbarPos(HWND h, int pos) { SendMessageW(h, TBM_SETPOS, TRUE, pos); }
int GetTrackbarPos(HWND h) { return static_cast<int>(SendMessageW(h, TBM_GETPOS, 0, 0)); }

class RecorderPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Flight Recorder"; }

    void Create() override {
        status_   = CreateLabel(L"", SS_CENTERIMAGE);
        startBtn_ = CreateButton(L"  Start recording", true);
        stopBtn_  = CreateButton(L"  Stop");
        saveBtn_  = CreateButton(L"  Save buffer…");
        openBtn_  = CreateButton(L"  Open trace…");

        scrub_     = CreateTrackbar(0, 1000);
        scrubInfo_ = CreateLabel(L"", SS_CENTERIMAGE);

        table_.Create({{L"Time", 140, true},
                       {L"CPU", 80, true, -10},
                       {L"Memory", 110, true, -(1024.0 * 1024 * 1024)},
                       {L"Disk", 80, true, -10},
                       {L"Net", 100, true, -10},
                       {L"Top process", 240}},
                      0, true);

        UpdateButtonState();
    }

    void Layout(RECT a) override {
        const int h = Scale(34), btn = Scale(150), gap = Scale(8);
        int x = a.left;
        Place(startBtn_, x, a.top, btn + Scale(20), h); x += btn + Scale(20) + gap;
        Place(stopBtn_,  x, a.top, btn, h); x += btn + gap;
        Place(saveBtn_,  x, a.top, btn, h); x += btn + gap;
        Place(openBtn_,  x, a.top, btn, h); x += btn + gap;
        Place(status_,   x, a.top, a.right - x, h);

        const int scrubTop = a.top + h + Scale(10);
        Place(scrub_,     a.left, scrubTop, a.right - a.left, Scale(30));
        Place(scrubInfo_, a.left, scrubTop + Scale(30), a.right - a.left, Scale(20));

        const int top = scrubTop + Scale(54);
        Place(table_.Handle(), a.left, top, a.right - a.left, a.bottom - top);
    }

    void Show(bool v) override {
        ShowAll({status_, startBtn_, stopBtn_, saveBtn_, openBtn_, scrub_, scrubInfo_, table_.Handle()}, v);
    }

    void OnActivate() override {
        UpdateButtonState();
        Refresh();
    }

    void OnTick(bool visible) override {
        if (!visible) return;
        UpdateButtonState();
        // Cheap path: only rebuild the table while recording or viewing a loaded file.
        if (g_recorder.Recording() || showLoaded_) Refresh();
    }

    bool OnScroll(HWND control, WORD) override {
        if (control != scrub_) return false;
        OnScrub();
        return true;
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override { return table_.HandleNotify(nm, result); }

    bool OnCommand(HWND control, WORD code) override {
        if (code != BN_CLICKED) return false;
        if (control == startBtn_) { OnStart(); return true; }
        if (control == stopBtn_)  { OnStop();  return true; }
        if (control == saveBtn_)  { OnSaveBuffer(); return true; }
        if (control == openBtn_)  { OnOpen(); return true; }
        return false;
    }

private:
    void UpdateButtonState() {
        const bool rec = g_recorder.Recording();
        const size_t buffered = g_recorder.Size();  // no copy of the buffer
        EnableWindow(startBtn_, !rec);
        EnableWindow(stopBtn_,   rec);
        EnableWindow(saveBtn_,  !saving_ && buffered > 0);
        std::wstring text = Format(L"%s  ·  %zu samples in memory%s",
            rec ? Format(L"Recording to %s", g_recorder.Path().c_str()).c_str() : L"Not recording",
            buffered,
            rec ? Format(L"  ·  %llu written", static_cast<unsigned long long>(g_recorder.Written())).c_str() : L"");
        // Surface why a recording stopped on its own (disk full, drive removed...).
        const std::wstring lastError = g_recorder.LastError();
        if (!rec && !lastError.empty()) text += L"  ·  " + lastError;
        SetText(status_, text);
    }

    void OnStart() {
        const std::wstring path = PickFile(true, L"Trace files", L"*.mtmtrace", L"mtmtrace", DefaultTraceName().c_str());
        if (path.empty()) return;  // user cancelled
        std::wstring error;
        if (!g_recorder.Start(path, error)) ShowError(L"Couldn't start recording: " + error);
        UpdateButtonState();
    }

    void OnStop() {
        g_recorder.Stop();
        UpdateButtonState();
    }

    // Saving can take a second or two on a full buffer, so it runs on a worker.
    // The result comes back through PostToUi (WM_APP + 1 is already WM_APP_RUN here).
    void OnSaveBuffer() {
        if (saving_) return;
        auto samples = g_recorder.Snapshot();
        if (samples.empty()) {
            ShowError(L"The in-memory buffer is empty.");
            return;
        }
        const std::wstring path = PickFile(true, L"Trace files", L"*.mtmtrace", L"mtmtrace", DefaultTraceName().c_str());
        if (path.empty()) return;
        saving_ = true;
        UpdateButtonState();
        saver_ = std::jthread([this, path, samples = std::move(samples)](std::stop_token) {
            std::wstring error;
            const bool ok = FlightRecorder::Save(path, samples, error);
            PostToUi([this, ok, error] {
                saving_ = false;
                UpdateButtonState();
                if (!ok) ShowError(L"Couldn't save: " + error);
            });
        });
    }

    void OnOpen() {
        const std::wstring path = PickFile(false, L"Trace files", L"*.mtmtrace", L"mtmtrace");
        if (path.empty()) return;

        std::vector<TraceSample> loaded;
        std::wstring error;
        if (!FlightRecorder::Load(path, loaded, error)) {
            ShowError(L"Couldn't open the trace: " + error);
            return;
        }
        loaded_     = std::move(loaded);
        showLoaded_ = true;
        scrubPos_   = 0;
        SetTrackbarRange(scrub_, 0, static_cast<int>(loaded_.size()) - 1);
        SetTrackbarPos(scrub_, 0);
        Refresh();
    }

    void OnScrub() {
        if (!showLoaded_ || loaded_.empty()) return;
        scrubPos_ = static_cast<size_t>(std::max(0, GetTrackbarPos(scrub_)));
        if (scrubPos_ >= loaded_.size()) scrubPos_ = loaded_.size() - 1;
        Refresh();
    }

    // Which sample list are we showing?
    const std::vector<TraceSample>& Source() {
        if (showLoaded_) return loaded_;
        liveCache_ = g_recorder.Snapshot();
        return liveCache_;
    }

    void Refresh() {
        const auto& samples = Source();
        if (samples.empty()) {
            table_.SetRows({});
            SetText(scrubInfo_, L"No samples.");
            return;
        }

        // Live: the most recent N samples. Loaded: a window ending at the scrub position.
        constexpr size_t kWindow = 200;
        const size_t end   = showLoaded_ ? std::min(scrubPos_ + 1, samples.size()) : samples.size();
        const size_t begin = end > kWindow ? end - kWindow : 0;

        std::vector<Row> rows;
        rows.reserve(end - begin);
        for (size_t i = begin; i < end; ++i) {
            const TraceSample& s = samples[i];
            Row r;
            r.tag = i;
            const std::wstring top = s.top.empty() ? L"—" : s.top[0].name;
            r.Add(FormatTime(s.time), double(s.time))
             .Add(Format(L"%.1f%%", s.cpu),        s.cpu)
             .Add(FormatBytes(s.memUsed),          double(s.memUsed))
             .Add(Format(L"%.1f%%", s.diskActive), s.diskActive)
             .Add(Format(L"%.1f MB/s", (s.netIn + s.netOut) / (1024.0 * 1024.0)), double(s.netIn + s.netOut))
             .Add(top);
            rows.push_back(std::move(r));
        }
        table_.SetRows(std::move(rows));

        if (showLoaded_) {
            const TraceSample& s = samples[scrubPos_];
            SetText(scrubInfo_, Format(L"Sample %zu of %zu  ·  %s", scrubPos_ + 1, samples.size(), FormatTime(s.time).c_str()));
        } else {
            SetText(scrubInfo_, Format(L"%zu samples buffered", samples.size()));
        }
    }

    static std::wstring DefaultTraceName() {
        SYSTEMTIME st;
        GetLocalTime(&st);
        return Format(L"MTM_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    }

    // FILETIME (UTC) -> local "HH:MM:SS.mmm".
    static std::wstring FormatTime(uint64_t filetime) {
        FILETIME ft;
        ft.dwLowDateTime  = static_cast<DWORD>(filetime);
        ft.dwHighDateTime = static_cast<DWORD>(filetime >> 32);
        SYSTEMTIME utc, local;
        if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
            return Format(L"%llu", static_cast<unsigned long long>(filetime));  // raw value rather than a dash
        return Format(L"%02u:%02u:%02u.%03u", local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    }

    HWND  status_    = nullptr;
    HWND  startBtn_  = nullptr;
    HWND  stopBtn_   = nullptr;
    HWND  saveBtn_   = nullptr;
    HWND  openBtn_   = nullptr;
    HWND  scrub_     = nullptr;
    HWND  scrubInfo_ = nullptr;
    Table table_;

    std::vector<TraceSample> loaded_;
    std::vector<TraceSample> liveCache_;
    bool   showLoaded_ = false;
    bool   saving_     = false;
    size_t scrubPos_   = 0;
    std::jthread saver_;  // last member: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeRecorderPage() { return std::make_unique<RecorderPage>(); }

}  // namespace mtm
