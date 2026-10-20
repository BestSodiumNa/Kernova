#pragma once

#include "common.h"

namespace mtm {

struct CpuTemp {
    double c = -1;           // degrees C; negative = unavailable
    std::wstring source;     // where the reading came from
};

// GPU die temperature in degrees C from the WDDM driver (same source as Task Manager), or -1.
double GpuTempC(const LUID& adapter);

// Latest CPU temperature. Real package/die temperature when LibreHardwareMonitor or OpenHardwareMonitor
// is running (read over WMI); otherwise the ACPI thermal zone, which is only approximate.
// Polled on its own worker thread because WMI can block; this returns a copy of the last result.
CpuTemp CpuTempNow();

// Drive temperature in degrees C for \\.\PhysicalDriveN, or -1. Refreshed every ~10 s on the sensor
// worker so a spun-down disk is never queried from the UI thread.
double DiskTempC(int number);

// "52 °C"; empty if unavailable. CpuTempText flags the approximate ACPI-zone fallback: in full
// ("52 °C (ACPI zone, approx.)") or, when brief, as "~52 °C" for tight spaces.
std::wstring TempText(double c);
std::wstring CpuTempText(const CpuTemp& t, bool brief = false);

}  // namespace mtm
