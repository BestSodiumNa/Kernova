#pragma once

#include "common.h"
#include "sensors.h"

namespace mtm {

struct CoreInfo {
    WORD group = 0;
    WORD index = 0;          // processor number within its group
    double usage = 0;        // % busy since the previous tick
    ULONG currentMhz = 0;    // live clock (nominal x "% Processor Performance")
    ULONG nominalMhz = 0;    // base clock reported by the power manager
    ULONG limitMhz = 0;
    double performance = 0;  // % of nominal; above 100 while boosting
    int parked = -1;         // 1 = parked by Windows right now, 0 = active, -1 = unknown
};

struct SystemSnapshot {
    double cpu = 0;
    std::vector<CoreInfo> cores;
    double avgMhz = 0;
    CpuTemp cpuTemp;

    uint64_t memTotal = 0, memAvail = 0, commitUsed = 0, commitLimit = 0, memCached = 0;

    double diskActive = 0, diskRead = 0, diskWrite = 0;  // %, bytes/s
    struct Disk {
        int number = 0;          // physical disk number ("Disk 0")
        std::wstring volumes;    // "C: D:"
        double active = 0, read = 0, write = 0;
        double tempC = -1;       // negative = unavailable
    };
    std::vector<Disk> disks;     // each physical disk, by number
    double netIn = 0, netOut = 0;                        // bytes/s
    int netAdapters = 0;

    bool gpuAvailable = false;
    struct Gpu {
        std::wstring name;
        uint64_t dedicatedVram = 0;
        bool integrated = false;
        LUID luid{};
        double usage = 0;            // busiest engine on this GPU, %
        uint64_t dedicatedUsed = 0;  // bytes
        uint64_t sharedUsed = 0;
        double tempC = -1;           // die temperature; negative = unavailable
    };
    std::vector<Gpu> gpus;          // all hardware GPUs, dedicated first
    std::wstring gpuName;           // the first of them
    double gpu = 0;                // busiest engine, %
    uint64_t gpuDedicated = 0;     // bytes in use
    uint64_t gpuShared = 0;
    uint64_t gpuDedicatedTotal = 0;
};

// Samples everything once per tick (the main window calls this right after the process sampler).
// Per-core clocks come from the "% Processor Performance" counter because CallNtPowerInformation
// only reports the base clock on modern Windows; this is what Windows Task Manager does too.
void SampleSystem();
const SystemSnapshot& Sys();
double GpuPercentForPid(DWORD pid);

std::wstring CpuBrandName();
std::wstring ActivePowerPlanName();

}  // namespace mtm
