#pragma once

#include "common.h"

namespace mtm {

// Razer mice over the 90-byte feature-report protocol Synapse uses (as reverse-engineered
// publicly, e.g. by the OpenRazer project; implemented here from the protocol description).
struct RazerMouse {
    std::wstring name, path;  // path: the HID mouse collection that takes the 91-byte feature reports
    USHORT pid = 0;
};
std::vector<RazerMouse> FindRazerMice();

struct RazerState {
    bool ok = false;           // the mouse answered at all (it can be asleep or out of range)
    bool wired = false;        // no battery at all (wired-only mouse)
    int battery = -1;          // %, -1 when wired
    bool charging = false;     // a wireless mouse charging over its cable
    int pollHz = 0;
    bool pollExtended = false;  // understands the newer command with 2000-8000 Hz
    int dpiX = 0, dpiY = 0;
    std::vector<int> stages;    // DPI of each stage
    int activeStage = -1;       // index into stages
    int sleepSeconds = 0;       // idle time before a wireless mouse sleeps
    std::wstring firmware;
};
// Talks to the mouse; takes a few hundred ms over wireless, so call it off the UI thread.
RazerState ReadRazer(const RazerMouse& mouse);

// Changes are stored in the mouse (they survive unplugging). Each returns a Win32 error,
// ERROR_NOT_SUPPORTED when the mouse refuses the command.
DWORD RazerSetDpi(const RazerMouse& mouse, int dpi);
DWORD RazerSetActiveStage(const RazerMouse& mouse, const RazerState& state, int stage);
DWORD RazerSetPolling(const RazerMouse& mouse, bool extended, int hz);
std::vector<int> RazerPollingChoices(bool extended);

}  // namespace mtm
