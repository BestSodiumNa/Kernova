#pragma once

#include "common.h"

#include <stop_token>

namespace mtm {

// Drive optimization (what Windows' "Defragment and Optimize Drives" does):
//  - HDD: defragment, i.e. move file pieces together so the disk head seeks less.
//  - SSD: re-TRIM, i.e. tell the SSD which blocks are free so it can erase them
//    ahead of time. SSDs are never defragmented by this code (defrag.exe /O picks
//    the right operation per media type).

struct OptVolume {
    wchar_t letter = L'C';
    std::wstring label, fileSystem;
    uint64_t size = 0, free = 0;
    bool ssd = false;            // no seek penalty
    bool knownMedia = false;     // seek-penalty query answered
    uint64_t lastOptimized = 0;  // FILETIME UTC from the event log, 0 = never / unknown
    std::wstring lastOperation;  // "re-trim", "defragmentation"...
};

// Fixed NTFS / ReFS volumes Windows can optimize.
std::vector<OptVolume> ListOptimizableVolumes();

// Whether Windows' weekly "ScheduledDefrag" task is enabled. -1 = couldn't tell.
int AutomaticOptimizationEnabled();
// Turns the scheduled task on/off (UAC prompt when not elevated). Blocks.
DWORD SetAutomaticOptimization(bool enabled);

struct OptimizeResult {
    DWORD code = ERROR_SUCCESS;  // ERROR_CANCELLED when stopped or the UAC prompt was declined
    std::wstring summary;        // last meaningful line, e.g. "Total fragmented space = 3%"
    int fragmentedPercent = -1;  // from an analysis
    std::wstring log;            // full defrag.exe output
};

// Runs defrag.exe on one volume: analysis only, or a full optimization (/O: defrag on
// HDDs, re-trim on SSDs). Elevates through a UAC prompt when needed and streams
// progress (0..100, stage text) back from the elevated helper. Blocks; call from a
// worker thread.
OptimizeResult RunOptimization(wchar_t letter, bool analyzeOnly, std::stop_token st,
                               const std::function<void(double percent, const std::wstring& stage)>& progress);

// Handles "--optimize <letter> <analyze|optimize> <progress file>" (the elevated helper).
bool HandleOptimizeCommandLine(int& exitCode);

// Reminder interval in days (0 = off), stored in settings. Default 30.
int OptimizeReminderDays();
void SetOptimizeReminderDays(int days);
// "Remind me later": quiet until this FILETIME.
void SnoozeOptimizeReminder(int days);
// True when some volume hasn't been optimized within the reminder interval.
bool OptimizationDue(const std::vector<OptVolume>& volumes);

}  // namespace mtm
