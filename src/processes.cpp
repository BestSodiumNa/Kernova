#include "processes.h"

#include <algorithm>

namespace mtm {

ProcessSampler g_procs;

namespace {

constexpr ULONG kSystemProcessInformation = 5;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004);

struct NtUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
};

// Layout of SYSTEM_PROCESS_INFORMATION (x64), only the fields we read.
struct NtProcessInfo {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    NtUnicodeString ImageName;
    LONG BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPeakPagedPoolUsage;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaPeakNonPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
};

using NtQuerySystemInformationFn = LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);

uint64_t ToU64(const FILETIME& ft) { return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

}  // namespace

void ProcessSampler::Sample() {
    static auto query = reinterpret_cast<NtQuerySystemInformationFn>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    if (!query) return;

    if (buffer_.empty()) buffer_.resize(1 << 20);
    for (int attempt = 0; attempt < 6; ++attempt) {
        ULONG needed = 0;
        const LONG status = query(kSystemProcessInformation, buffer_.data(), static_cast<ULONG>(buffer_.size()), &needed);
        if (status == kStatusInfoLengthMismatch) {
            // The process list can grow between calls; leave headroom.
            buffer_.resize(std::max<size_t>(size_t(needed) + (256 << 10), buffer_.size() * 2));
            continue;
        }
        if (status < 0) return;
        break;
    }

    FILETIME idleFt, kernelFt, userFt;
    GetSystemTimes(&idleFt, &kernelFt, &userFt);
    const uint64_t sysTotal = ToU64(kernelFt) + ToU64(userFt);  // kernel time includes idle
    const uint64_t sysIdle = ToU64(idleFt);
    const uint64_t sysDelta = lastSysTotal_ ? sysTotal - lastSysTotal_ : 0;
    const uint64_t idleDelta = lastSysTotal_ ? sysIdle - lastSysIdle_ : 0;
    cpuTotal_ = sysDelta ? 100.0 * double(sysDelta - idleDelta) / double(sysDelta) : 0.0;
    lastSysTotal_ = sysTotal;
    lastSysIdle_ = sysIdle;

    std::vector<ProcInfo> procs;
    std::unordered_map<uint64_t, uint64_t> cpuTimes;
    std::unordered_map<uint64_t, uint64_t> ioTotals;
    const double ioSecs = ioClock_.Seconds();
    ioClock_.Reset();
    procs.reserve(procs_.size() + 16);
    threads_ = 0;
    handles_ = 0;

    const BYTE* p = buffer_.data();
    for (;;) {
        const auto* e = reinterpret_cast<const NtProcessInfo*>(p);
        const DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->UniqueProcessId));
        threads_ += e->NumberOfThreads;
        handles_ += e->HandleCount;
        if (pid != 0) {  // skip "System Idle Process"
            ProcInfo info;
            info.pid = pid;
            info.parentPid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(e->InheritedFromUniqueProcessId));
            if (e->ImageName.Buffer && e->ImageName.Length)
                info.name.assign(e->ImageName.Buffer, e->ImageName.Length / sizeof(wchar_t));
            else
                info.name = pid == 4 ? L"System" : L"(unknown)";
            info.privateWs = static_cast<uint64_t>(e->WorkingSetPrivateSize.QuadPart);
            info.workingSet = e->WorkingSetSize;
            info.commit = e->PagefileUsage;
            info.threads = e->NumberOfThreads;
            info.handles = e->HandleCount;
            info.session = e->SessionId;
            info.basePriority = e->BasePriority;

            const uint64_t cpuTime = uint64_t(e->UserTime.QuadPart) + uint64_t(e->KernelTime.QuadPart);
            // Key on create time too so a reused PID never inherits old CPU time.
            const uint64_t key = uint64_t(e->CreateTime.QuadPart) * 1000003ull ^ pid;
            cpuTimes[key] = cpuTime;
            auto it = lastCpuTime_.find(key);
            if (it != lastCpuTime_.end() && sysDelta && cpuTime >= it->second)
                info.cpu = 100.0 * double(cpuTime - it->second) / double(sysDelta);
            const uint64_t io = uint64_t(e->ReadTransferCount.QuadPart) + uint64_t(e->WriteTransferCount.QuadPart);
            ioTotals[key] = io;
            auto io0 = lastIo_.find(key);
            if (io0 != lastIo_.end() && ioSecs > 0 && io >= io0->second) info.ioRate = double(io - io0->second) / ioSecs;
            procs.push_back(std::move(info));
        }
        if (!e->NextEntryOffset) break;
        p += e->NextEntryOffset;
    }

    procs_ = std::move(procs);
    lastCpuTime_ = std::move(cpuTimes);
    lastIo_ = std::move(ioTotals);
    index_.clear();
    for (size_t i = 0; i < procs_.size(); ++i) index_[procs_[i].pid] = i;
}

std::wstring ProcessSampler::NameOf(DWORD pid) const {
    if (pid == 0) return L"System Idle Process";
    auto it = index_.find(pid);
    return it != index_.end() ? procs_[it->second].name : std::wstring();
}

}  // namespace mtm
