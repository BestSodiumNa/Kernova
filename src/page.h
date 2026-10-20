#pragma once

#include "common.h"

#include <memory>

namespace mtm {

// One tab of the main window. Pages create their controls as children of the
// main window and receive forwarded notifications, commands and ticks.
class Page {
public:
    virtual ~Page() = default;
    virtual const wchar_t* Title() const = 0;
    virtual void Create() = 0;
    virtual void Layout(RECT area) = 0;
    virtual void Show(bool visible) = 0;

    virtual void OnActivate() {}               // tab became visible, or F5
    virtual void OnTick(bool /*visible*/) {}  // every update interval, for all pages
    virtual bool OnNotify(NMHDR*, LRESULT&) { return false; }
    virtual bool OnCommand(HWND /*control*/, WORD /*code*/) { return false; }
    virtual bool OnContextMenu(HWND /*control*/, POINT /*screenPt*/) { return false; }
    // WM_HSCROLL from child controls such as trackbars (they don't use WM_NOTIFY).
    virtual bool OnScroll(HWND /*control*/, WORD /*code*/) { return false; }
    // True to show a reminder dot on the page's sidebar entry (e.g. a drive is due for optimization).
    virtual bool NeedsAttention() const { return false; }
    // Height (in pixels) the page needs at this width before it starts to look cramped.
    // When the window is shorter, the page scrolls instead of squeezing. 0 = always fit.
    virtual int MinHeight(int /*width*/) const { return 0; }
};

std::unique_ptr<Page> MakeSummaryPage();
std::unique_ptr<Page> MakeProcessesPage();
std::unique_ptr<Page> MakePerformancePage();
std::unique_ptr<Page> MakeUsersPage();
std::unique_ptr<Page> MakeConnectionsPage();
std::unique_ptr<Page> MakePowerPage();
std::unique_ptr<Page> MakeStartupPage();
std::unique_ptr<Page> MakeServicesPage();
std::unique_ptr<Page> MakeInstalledAppsPage();
std::unique_ptr<Page> MakeDiskSpacePage();
std::unique_ptr<Page> MakeBenchmarksPage();
std::unique_ptr<Page> MakeRecorderPage();
std::unique_ptr<Page> MakeHealthPage();
std::unique_ptr<Page> MakeTweaksPage();
std::unique_ptr<Page> MakeInputPage();
std::unique_ptr<Page> MakeDevicesPage();

}  // namespace mtm
