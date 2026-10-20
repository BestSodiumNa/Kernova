#pragma once

#include "common.h"

namespace mtm {

struct PowerPlan {
    GUID guid{};
    std::wstring name;
    bool active = false;
    bool builtIn = false;  // one of Windows' own schemes
    int presetIndex = -1;  // created from one of PowerPresets()
};

// Every power scheme on the machine, the active one marked.
std::vector<PowerPlan> ListPowerPlans();
DWORD ActivatePowerPlan(const GUID& plan);
// Deletes a plan (not the active one). UAC prompt when needed. Blocks.
DWORD DeletePowerPlan(const GUID& plan);

// Plans the app can add.
struct PowerPreset {
    const wchar_t* name;
    const wchar_t* summary;  // one line for the picker
    const wchar_t* what;     // what it changes
    const wchar_t* why;      // when to use it
};
const std::vector<PowerPreset>& PowerPresets();
// Creates the preset as a new plan and makes it active. UAC prompt when needed. Blocks.
DWORD AddPowerPreset(size_t index);

// Core parking for the active plan: Windows may "park" (idle) cores to save power.
struct ParkingState {
    bool known = false;
    DWORD minCoresAc = 0, minCoresDc = 0;  // % of cores that must stay unparked
    bool DisabledAc() const { return minCoresAc >= 100; }
    bool DisabledDc() const { return minCoresDc >= 100; }
};
ParkingState ReadCoreParking();
// disable = keep 100% of cores unparked (plugged in and on battery); otherwise restore
// the plan's default. UAC prompt when needed. Blocks.
DWORD SetCoreParkingDisabled(bool disable);

// Starts `file params` elevated (one UAC prompt) and waits for it, calling `whileWaiting` about
// every 150 ms. Returns its exit code or a Win32 error (ERROR_CANCELLED when the prompt is
// declined, WAIT_TIMEOUT when it is still running after `timeoutMs`). Blocks.
DWORD RunElevated(const std::wstring& file, const std::wstring& params, const std::function<void()>& whileWaiting = {},
                  DWORD timeoutMs = INFINITE);
// The same for this exe, which is how every "--xxx" helper is started.
DWORD RunSelfElevated(const std::wstring& params, DWORD timeoutMs = 10 * 60000);  // a restore point can take minutes

// "{xxxxxxxx-xxxx-...}" and the GUID of the active power plan (false if there is none).
std::wstring GuidText(const GUID& g);
bool ActiveScheme(GUID& out);
// Tries `fn` directly; if Windows says we need admin rights, re-runs this exe elevated
// with `helperParams` (one UAC prompt) and returns its exit code. Blocks.
DWORD WithElevation(const std::function<DWORD()>& fn, const std::wstring& helperParams);

// Handles "--power ..." (the elevated helper).
bool HandlePowerCommandLine(int& exitCode);

}  // namespace mtm
