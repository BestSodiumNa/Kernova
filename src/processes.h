#pragma once

#include "common.h"

#include <unordered_map>

namespace mtm {

struct ProcInfo {
    DWORD pid = 0;
    DWORD parentPid = 0;
    std::wstring name;
    double cpu = 0;           // percent of the whole machine
    uint64_t privateWs = 0;   // private working set ("Memory" in Task Manager)
    uint64_t workingSet = 0;
    uint64_t commit = 0;      // private bytes
    ULONG threads = 0;
    ULONG handles = 0;
    ULONG session = 0;
    LONG basePriority = 0;
    double ioRate = 0;        // read + write bytes/s (file, network and device I/O)
};

// One NtQuerySystemInformation(SystemProcessInformation) snapshot per tick,
// shared by every page that needs process data.
class ProcessSampler {
public:
    void Sample();
    const std::vector<ProcInfo>& Processes() const { return procs_; }
    std::wstring NameOf(DWORD pid) const;
    double CpuPercent() const { return cpuTotal_; }
    ULONG ThreadCount() const { return threads_; }
    ULONG HandleCount() const { return handles_; }

private:
    std::vector<ProcInfo> procs_;
    std::unordered_map<DWORD, size_t> index_;
    std::unordered_map<uint64_t, uint64_t> lastCpuTime_;  // (pid, create time) -> 100ns CPU time
    std::unordered_map<uint64_t, uint64_t> lastIo_;       // (pid, create time) -> bytes transferred
    Stopwatch ioClock_;
    std::vector<BYTE> buffer_;
    uint64_t lastSysTotal_ = 0;
    uint64_t lastSysIdle_ = 0;
    double cpuTotal_ = 0;
    ULONG threads_ = 0;
    ULONG handles_ = 0;
};

extern ProcessSampler g_procs;

}  // namespace mtm
