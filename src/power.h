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

// Handles "--power ..." (the elevated helper).
bool HandlePowerCommandLine(int& exitCode);

}  // namespace mtm
