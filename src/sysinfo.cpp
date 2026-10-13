#include <winsock2.h>
#include <ws2ipdef.h>

#include "sysinfo.h"

#include "benchmarks.h"

#include "processes.h"

#include <dxgi.h>
#include <iphlpapi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <powerbase.h>
#include <powrprof.h>
#include <psapi.h>

#include <algorithm>
#include <map>
#include <unordered_map>

namespace mtm {

namespace {

// PROCESSOR_POWER_INFORMATION is documented but not declared in the SDK headers.
struct ProcessorPowerInformation {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
};

// SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION (NtQuerySystemInformation class 8).
struct ProcessorTimes {
    LARGE_INTEGER IdleTime;
    LARGE_INTEGER KernelTime;  // includes idle
    LARGE_INTEGER UserTime;
    LARGE_INTEGER Reserved1[2];
    ULONG Reserved2;
};

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

SystemSnapshot g_sys;
std::unordered_map<DWORD, double> g_gpuByPid;

class Counters {
public:
    Counters() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) return;
        perf_ = Add(L"\\Processor Information(*)\\% Processor Performance");
        parking_ = Add(L"\\Processor Information(*)\\Parking Status");  // 1 while the core is parked
        diskIdle_ = Add(L"\\PhysicalDisk(_Total)\\% Idle Time");
        diskRead_ = Add(L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec");
        diskWrite_ = Add(L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec");
        diskIdleEach_ = Add(L"\\PhysicalDisk(*)\\% Idle Time");
        diskReadEach_ = Add(L"\\PhysicalDisk(*)\\Disk Read Bytes/sec");
        diskWriteEach_ = Add(L"\\PhysicalDisk(*)\\Disk Write Bytes/sec");
        gpuEngine_ = Add(L"\\GPU Engine(*)\\Utilization Percentage");
        gpuDedicated_ = Add(L"\\GPU Adapter Memory(*)\\Dedicated Usage");
        gpuShared_ = Add(L"\\GPU Adapter Memory(*)\\Shared Usage");
        PdhCollectQueryData(query_);  // rate counters need a first sample
    }
    ~Counters() {
        if (query_) PdhCloseQuery(query_);
    }

    bool Collect() { return query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS; }

    double Value(PDH_HCOUNTER c) const {
        PDH_FMT_COUNTERVALUE v{};
        if (!c || PdhGetFormattedCounterValue(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, nullptr, &v) != ERROR_SUCCESS)
            return 0;
        return v.doubleValue;
    }

    // Calls fn(instanceName, value) for every valid instance of a wildcard counter.
    template <typename Fn>
    void ForEach(PDH_HCOUNTER c, Fn fn) {
        if (!c) return;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) !=
            static_cast<PDH_STATUS>(PDH_MORE_DATA))
            return;
        buffer_.resize(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
        if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
            return;
        for (DWORD i = 0; i < count; ++i) {
            const DWORD st = items[i].FmtValue.CStatus;
            if (st == PDH_CSTATUS_VALID_DATA || st == PDH_CSTATUS_NEW_DATA) fn(items[i].szName, items[i].FmtValue.doubleValue);
        }
    }

    PDH_HCOUNTER perf_ = nullptr, parking_ = nullptr, diskIdle_ = nullptr, diskRead_ = nullptr, diskWrite_ = nullptr;
    PDH_HCOUNTER gpuEngine_ = nullptr, gpuDedicated_ = nullptr, gpuShared_ = nullptr;
    PDH_HCOUNTER diskIdleEach_ = nullptr, diskReadEach_ = nullptr, diskWriteEach_ = nullptr;

private:
    PDH_HCOUNTER Add(const wchar_t* path) {
        PDH_HCOUNTER c = nullptr;
        return PdhAddEnglishCounterW(query_, path, 0, &c) == ERROR_SUCCESS ? c : nullptr;
    }
    PDH_HQUERY query_ = nullptr;
    std::vector<BYTE> buffer_;
};

void SampleCores(Counters& pdh, SystemSnapshot& s) {
    static auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    static std::vector<ProcessorTimes> last;

    const DWORD count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    std::vector<ProcessorPowerInformation> power(count);
    const bool havePower =
        count && CallNtPowerInformation(ProcessorInformation, nullptr, 0, power.data(),
                                        static_cast<ULONG>(power.size() * sizeof(ProcessorPowerInformation))) == 0;

    // Busy time per processor (covers the calling thread's processor group).
    std::vector<ProcessorTimes> times(count);
    ULONG got = 0;
    const bool haveTimes = query && query(8, times.data(), static_cast<ULONG>(times.size() * sizeof(ProcessorTimes)), &got) >= 0;
    times.resize(haveTimes ? got / sizeof(ProcessorTimes) : 0);

    std::unordered_map<uint32_t, double> live, parked;
    pdh.ForEach(pdh.parking_, [&](const wchar_t* name, double v) {
        unsigned group = 0, index = 0;
        if (!wcschr(name, L'_') && swscanf_s(name, L"%u,%u", &group, &index) == 2) parked[(group << 16) | index] = v;
    });
    pdh.ForEach(pdh.perf_, [&](const wchar_t* name, double v) {
        unsigned group = 0, index = 0;
        if (!wcschr(name, L'_') && swscanf_s(name, L"%u,%u", &group, &index) == 2) live[(group << 16) | index] = v;
    });

    s.cores.clear();
    double sum = 0;
    size_t next = 0;
    const WORD groups = GetActiveProcessorGroupCount();
    for (WORD g = 0; g < groups && next < count; ++g) {
        const DWORD inGroup = GetActiveProcessorCount(g);
        for (DWORD i = 0; i < inGroup && next < count; ++i, ++next) {
            CoreInfo c;
            c.group = g;
            c.index = static_cast<WORD>(i);
            if (havePower) {
                c.nominalMhz = power[next].MaxMhz;
                c.limitMhz = power[next].MhzLimit;
                c.currentMhz = power[next].CurrentMhz;
            }
            if (auto pk = parked.find((uint32_t(g) << 16) | i); pk != parked.end()) c.parked = pk->second >= 0.5 ? 1 : 0;
            auto it = live.find((uint32_t(g) << 16) | i);
            if (it != live.end() && c.nominalMhz) {
                c.performance = it->second;
                c.currentMhz = static_cast<ULONG>(c.nominalMhz * it->second / 100.0 + 0.5);
            } else if (c.nominalMhz) {
                c.performance = 100.0 * c.currentMhz / c.nominalMhz;
            }
            if (next < times.size() && next < last.size()) {
                const double total = double(times[next].KernelTime.QuadPart - last[next].KernelTime.QuadPart) +
                                     double(times[next].UserTime.QuadPart - last[next].UserTime.QuadPart);
                const double idle = double(times[next].IdleTime.QuadPart - last[next].IdleTime.QuadPart);
                c.usage = total > 0 ? std::clamp(100.0 * (total - idle) / total, 0.0, 100.0) : 0;
            }
            sum += c.currentMhz;
            s.cores.push_back(c);
        }
    }
    last = std::move(times);
    s.avgMhz = s.cores.empty() ? 0 : sum / double(s.cores.size());
}

void SampleNetwork(SystemSnapshot& s) {
    static Stopwatch clock;
    static bool primed = false;
    static uint64_t lastIn = 0, lastOut = 0;
    uint64_t in = 0, out = 0;
    int adapters = 0;
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) == NO_ERROR) {
        for (ULONG i = 0; i < table->NumEntries; ++i) {
            const MIB_IF_ROW2& row = table->Table[i];
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface || row.OperStatus != IfOperStatusUp) continue;
            in += row.InOctets;
            out += row.OutOctets;
            ++adapters;
        }
        FreeMibTable(table);
    }
    const double secs = clock.Seconds();
    clock.Reset();
    s.netIn = primed && secs > 0 && in >= lastIn ? double(in - lastIn) / secs : 0;
    s.netOut = primed && secs > 0 && out >= lastOut ? double(out - lastOut) / secs : 0;
    s.netAdapters = adapters;
    primed = true;
    lastIn = in;
    lastOut = out;
}

void QueryGpuAdapter(SystemSnapshot& s) {
    // Every hardware adapter, the high-performance (dedicated) one first. Laptops list the
    // integrated GPU first because it drives the display, so taking adapter 0 hides the dGPU.
    static const std::vector<GpuAdapter> gpus = EnumerateGpus();
    s.gpus.clear();
    for (const GpuAdapter& g : gpus) s.gpus.push_back({g.name, g.dedicatedVram, g.integrated, g.luid});
    s.gpuName = gpus.empty() ? L"" : gpus.front().name;
    s.gpuDedicatedTotal = gpus.empty() ? 0 : gpus.front().dedicatedVram;
}

uint64_t LuidKey(const LUID& l) { return (uint64_t(uint32_t(l.HighPart)) << 32) | l.LowPart; }

// Parses the "luid_0x00000000_0x0000D1F1" part of a GPU counter instance name.
uint64_t LuidOf(const wchar_t* name) {
    const wchar_t* p = wcsstr(name, L"luid_");
    unsigned long high = 0, low = 0;
    if (!p || swscanf_s(p, L"luid_0x%lx_0x%lx", &high, &low) != 2) return ~0ull;
    return (uint64_t(high) << 32) | low;
}

// "pid_1234_luid_0x0_0xD1F1_phys_0_eng_0_engtype_3D": per-process utilisation of one engine.
// Like Task Manager: a process's GPU % is its busiest engine; the total is the busiest engine overall.
void SampleGpu(Counters& pdh, SystemSnapshot& s) {
    std::unordered_map<std::wstring, double> engineTotals;
    std::unordered_map<DWORD, std::unordered_map<std::wstring, double>> perPid;
    bool any = false;
    pdh.ForEach(pdh.gpuEngine_, [&](const wchar_t* name, double v) {
        any = true;
        unsigned long pid = 0;
        if (swscanf_s(name, L"pid_%lu", &pid) != 1) return;
        const wchar_t* engine = wcsstr(name, L"luid_");
        if (!engine) return;
        const std::wstring key = engine;
        engineTotals[key] += v;
        perPid[pid][key] += v;
    });
    s.gpuAvailable = any || !s.gpuName.empty();
    s.gpu = 0;
    for (const auto& [key, v] : engineTotals) s.gpu = std::max(s.gpu, v);
    s.gpu = std::min(s.gpu, 100.0);
    g_gpuByPid.clear();
    for (const auto& [pid, engines] : perPid) {
        double best = 0;
        for (const auto& [key, v] : engines) best = std::max(best, v);
        if (best > 0) g_gpuByPid[pid] = std::min(best, 100.0);
    }
    // Per GPU: the busiest engine among the instances carrying that adapter's LUID.
    for (auto& g : s.gpus) {
        g.usage = 0;
        for (const auto& [key, v] : engineTotals)
            if (LuidOf(key.c_str()) == LuidKey(g.luid)) g.usage = std::max(g.usage, std::min(v, 100.0));
    }
    s.gpuDedicated = 0;
    s.gpuShared = 0;
    auto memory = [&](const wchar_t* name, double v, bool dedicated) {
        (dedicated ? s.gpuDedicated : s.gpuShared) += static_cast<uint64_t>(v);
        const uint64_t luid = LuidOf(name);
        for (auto& g : s.gpus)
            if (LuidKey(g.luid) == luid) (dedicated ? g.dedicatedUsed : g.sharedUsed) += static_cast<uint64_t>(v);
    };
    pdh.ForEach(pdh.gpuDedicated_, [&](const wchar_t* n, double v) { memory(n, v, true); });
    pdh.ForEach(pdh.gpuShared_, [&](const wchar_t* n, double v) { memory(n, v, false); });
}

// PhysicalDisk instances are "0 C:", "1 D: E:" (and "_Total").
void SampleDisks(Counters& pdh, SystemSnapshot& s) {
    std::map<int, SystemSnapshot::Disk> disks;
    auto each = [&](PDH_HCOUNTER c, auto apply) {
        pdh.ForEach(c, [&](const wchar_t* name, double v) {
            if (name[0] < L'0' || name[0] > L'9') return;  // skip _Total
            const int number = _wtoi(name);
            auto& d = disks[number];
            d.number = number;
            if (const wchar_t* sp = wcschr(name, L' ')) d.volumes = sp + 1;
            apply(d, v);
        });
    };
    each(pdh.diskIdleEach_, [](SystemSnapshot::Disk& d, double v) { d.active = std::clamp(100.0 - v, 0.0, 100.0); });
    each(pdh.diskReadEach_, [](SystemSnapshot::Disk& d, double v) { d.read = v; });
    each(pdh.diskWriteEach_, [](SystemSnapshot::Disk& d, double v) { d.write = v; });
    for (auto& [n, d] : disks) s.disks.push_back(std::move(d));
}

}  // namespace

void SampleSystem() {
    static Counters pdh;
    SystemSnapshot s;
    const bool collected = pdh.Collect();
    s.cpu = g_procs.CpuPercent();
    SampleCores(pdh, s);

    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    s.memTotal = ms.ullTotalPhys;
    s.memAvail = ms.ullAvailPhys;
    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof pi;
    if (GetPerformanceInfo(&pi, sizeof pi)) {
        s.commitUsed = uint64_t(pi.CommitTotal) * pi.PageSize;
        s.commitLimit = uint64_t(pi.CommitLimit) * pi.PageSize;
        s.memCached = uint64_t(pi.SystemCache) * pi.PageSize;
    }

    if (collected) {
        s.diskActive = std::clamp(100.0 - pdh.Value(pdh.diskIdle_), 0.0, 100.0);
        s.diskRead = pdh.Value(pdh.diskRead_);
        s.diskWrite = pdh.Value(pdh.diskWrite_);
    }
    SampleNetwork(s);
    QueryGpuAdapter(s);
    if (collected) {
        SampleGpu(pdh, s);
        SampleDisks(pdh, s);
    }
    g_sys = std::move(s);
}

const SystemSnapshot& Sys() { return g_sys; }

double GpuPercentForPid(DWORD pid) {
    auto it = g_gpuByPid.find(pid);
    return it != g_gpuByPid.end() ? it->second : 0;
}

std::wstring CpuBrandName() {
    static std::wstring cached;
    if (!cached.empty()) return cached;
    wchar_t buf[256] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", L"ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return L"CPU";
    cached = buf;
    while (!cached.empty() && cached.back() == L' ') cached.pop_back();
    return cached;
}

std::wstring ActivePowerPlanName() {
    GUID* scheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &scheme) != ERROR_SUCCESS || !scheme) return L"";
    DWORD size = 0;
    std::wstring name;
    if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS && size) {
        std::vector<BYTE> buf(size);
        if (PowerReadFriendlyName(nullptr, scheme, nullptr, nullptr, buf.data(), &size) == ERROR_SUCCESS)
            name = reinterpret_cast<const wchar_t*>(buf.data());
    }
    LocalFree(scheme);
    return name;
}

}  // namespace mtm
