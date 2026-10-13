#pragma once

#include "defrag.h"
#include "graph.h"
#include "table.h"

#include <map>
#include <thread>

namespace mtm {

// "Optimize drives" section of the Drive Health page: explains defrag vs. TRIM,
// shows each volume's media type and last optimization, toggles Windows' automatic
// schedule, keeps a manual reminder, and runs optimizations with live progress.
class OptimizePanel {
public:
    void Create();
    void Layout(RECT area);
    void Show(bool visible);
    void OnActivate();
    void OnTick(bool visible);
    bool OnCommand(HWND control, WORD code);
    bool OnNotify(NMHDR* nm, LRESULT& result);
    // Some drive is past the reminder interval (drives the sidebar dot).
    bool NeedsAttention() const { return due_; }

private:
    void Refresh();               // volumes + schedule state, off the UI thread
    void Populate();
    void UpdateButtons();
    void Start(bool analyze);
    wchar_t SelectedLetter() const;

    TextCard info_;
    Table volumes_;
    HWND optimize_ = nullptr, analyze_ = nullptr, stop_ = nullptr, schedule_ = nullptr, remind_ = nullptr,
         remindLabel_ = nullptr, status_ = nullptr, later_ = nullptr;
    Bar progress_;

    std::vector<OptVolume> list_;
    int automatic_ = -1;          // scheduled task: 1 on, 0 off, -1 unknown
    bool due_ = false;
    bool busy_ = false;           // optimization running
    bool refreshing_ = false;
    bool visible_ = false;
    wchar_t runningLetter_ = 0;
    std::map<wchar_t, std::wstring> lastResult_;  // per drive, this session
    unsigned ticks_ = 0;
    RECT area_{};
    std::jthread refreshWorker_, worker_, scheduleWorker_;  // last: joined first
};

}  // namespace mtm
