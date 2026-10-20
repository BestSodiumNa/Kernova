#pragma once

#include "common.h"

#include <stop_token>

namespace mtm {

// A DPC-latency-style test of the thread-scheduling setting (Win32PrioritySeparation).
//
// DPC latency tools (LatencyMon, DPC Latency Checker) report how late work runs after the hardware
// asks for it, as the worst and typical delay in microseconds. This is the same measurement for the
// scheduler: a thread asks to be woken by a high-resolution 1 ms timer and records how late it was.
// It is done with every CPU core busy with background work, because on an idle PC every scheduler
// setting gives the same answer. The probe runs in the front window's process, like a game does, and
// the load runs in a separate process, like everything else on the PC.

// Lateness of the timer wake-ups, in microseconds.
struct WakeStats {
    size_t samples = 0;
    double avgUs = 0, medianUs = 0, p99Us = 0, p999Us = 0, maxUs = 0;
    double lateFraction = 0;  // share of wake-ups that were more than 1 ms late
};
WakeStats SummarizeWake(std::vector<double> latenciesUs);

// One stretch of timer wake-up measurements (the probe the sweep uses), exposed so the tests can
// check it. Returns false if the high-resolution timer can't be created.
bool MeasureWakeLatency(DWORD ms, std::stop_token st, std::vector<double>& latenciesUs);

// What the elevated helper is asked to do: step through `values` (each written to
// Win32PrioritySeparation), `phaseMs` apiece, `rounds` times (odd rounds in reverse order).
struct SweepPlan {
    DWORD phaseMs = 0;
    int rounds = 0;
    std::vector<DWORD> values;
};
// The arguments after "--latency-sweep": <progress file> <phaseMs> <rounds> <value>... Accepts only
// what the helper may do: 2-30 s phases, 1-4 rounds, 1-16 distinct values that the --tweak helper
// would accept too. Anything else is refused.
bool ParseSweepArgs(const std::vector<std::wstring>& args, std::wstring& file, SweepPlan& plan);

// The values worth testing: the scheduler presets, leaving out the fixed ones that differ only in
// foreground boost (the boost does nothing for fixed slices, so they would just repeat 0x28 / 0x18).
std::vector<DWORD> LatencyTestValues();

struct LatencyRow {
    DWORD value = 0;
    WakeStats stats;
    bool inFront = true;  // Kernova stayed the front window for the whole measurement
};
struct SweepOutcome {
    std::vector<LatencyRow> rows;  // in the order the values were given
    DWORD error = ERROR_SUCCESS;   // ERROR_CANCELLED when the admin prompt was declined
    bool cancelled = false;
    bool restoreNeeded = false;    // the original value is still parked: it must be put back
};
// Runs the whole test and blocks (minutes): one admin prompt, then the scheduler value is changed
// step by step while this process measures. Call from a worker thread. `progress` gets status text.
SweepOutcome RunLatencySweep(const std::vector<DWORD>& values, std::stop_token st,
                             const std::function<void(const std::wstring&)>& progress);

// Handles "--latency-sweep ..." (the elevated helper) and "--latency-load ..." (the background load).
bool HandleLatencyCommandLine(int& exitCode);

}  // namespace mtm
