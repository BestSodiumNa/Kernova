#include "latency.h"

#include "defrag.h"
#include "power.h"
#include "tweaks.h"

#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <thread>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace mtm {
namespace {

constexpr DWORD kPhaseMs = 6000;      // each setting is measured for this long, ...
constexpr DWORD kSettleMs = 1000;     // ... of which the first part is thrown away,
constexpr DWORD kMarginMs = 500;      // ... and the last part is left alone so the next step is never late
constexpr int kRounds = 2;            // and the whole list is run twice, the second time in reverse
constexpr DWORD kWarmupMs = 2500;     // the helper waits this long for the background load to ramp up
constexpr double kMinFrontShare = 0.9;  // below this Kernova wasn't the front window: the boost isn't fair

// -------------------------------------------------------------------------------- measuring

bool KernovaIsFront() {
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Asks a high-resolution timer for a 1 ms sleep over and over and records how late each wake-up was.
// `samples` may be null (the warm-up part: runs the same, records nothing). `frontShare` is the share
// of the time Kernova was the front window. False when the timer can't be created.
bool MeasureWake(DWORD ms, std::stop_token st, std::vector<double>* samples, double& frontShare) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) return false;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const ULONGLONG start = GetTickCount64();
    ULONGLONG nextCheck = start;
    unsigned checks = 0, front = 0;
    while (!st.stop_requested() && GetTickCount64() - start < ms) {
        if (const ULONGLONG now = GetTickCount64(); now >= nextCheck) {
            nextCheck = now + 100;
            ++checks;
            if (KernovaIsFront()) ++front;
        }
        LARGE_INTEGER due, t0, t1;
        due.QuadPart = -10000;  // 1 ms, relative (100 ns units)
        QueryPerformanceCounter(&t0);
        if (!SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;
        WaitForSingleObject(timer, 200);
        QueryPerformanceCounter(&t1);
        if (samples) {
            const double us = double(t1.QuadPart - t0.QuadPart) * 1e6 / double(freq.QuadPart);
            samples->push_back(std::max(0.0, us - 1000.0));
        }
    }
    CloseHandle(timer);
    frontShare = checks ? double(front) / checks : 0;
    return true;
}

// -------------------------------------------------------------------------------- the load

// Runs in its own process so it counts as "everything else on the PC" next to the front window. It
// stops when the app that started it is gone, so a crash can't leave CPU burners behind.
void RunLoad(DWORD parentPid, DWORD threads) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent) return;
    std::atomic<bool> stop{false};
    std::vector<std::thread> spinners;
    for (DWORD i = 0; i < threads; ++i)
        spinners.emplace_back([&stop] {
            volatile unsigned long long x = 1;
            while (!stop.load(std::memory_order_relaxed))
                for (int k = 0; k < 10000; ++k) x = x * 6364136223846793005ull + 1442695040888963407ull;
        });
    WaitForSingleObject(parent, 15 * 60 * 1000);  // and never longer than 15 minutes
    stop = true;
    for (std::thread& t : spinners) t.join();
    CloseHandle(parent);
}

HANDLE StartLoad() {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    const DWORD threads = std::min<DWORD>(64, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    std::wstring cmd = Format(L"\"%s\" --latency-load %lu %lu", exe, GetCurrentProcessId(), threads);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return nullptr;
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// -------------------------------------------------------------------------------- the helper

bool ParseUnsigned(const std::wstring& s, size_t maxDigits, DWORD& out) {
    if (s.empty() || s.size() > maxDigits) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    out = static_cast<DWORD>(wcstoul(s.c_str(), nullptr, 10));
    return true;
}

// Elevated. Steps Win32PrioritySeparation through the plan and tells the app, line by line in the
// progress file, which value is now set: "S" started, "P <n> <value>" a step, "D" done, "E <code>"
// failed. The original value is parked first and put back on every way out (a killed helper is
// repaired by the next elevated Kernova helper run instead).
int RunSweepHelper(const std::wstring& file, const SweepPlan& plan) {
    if (!IsElevated()) return ERROR_ACCESS_DENIED;
    HANDLE log = OpenProgressFile(file, L"mtm-latency-", L".mtmlat");
    if (log == INVALID_HANDLE_VALUE) return ERROR_INVALID_PARAMETER;
    auto write = [&](const std::string& text) {
        DWORD written = 0;
        WriteFile(log, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    };
    auto fail = [&](DWORD code) {
        write("E " + std::to_string(code) + "\n");
        CloseHandle(log);
        return static_cast<int>(code);
    };

    UnparkDword(HKEY_LOCAL_MACHINE, kPriorityControlKey, kQuantumValueName, kParkedQuantum);  // an earlier test
    if (const DWORD parked = ParkDword(HKEY_LOCAL_MACHINE, kPriorityControlKey, kQuantumValueName, kParkedQuantum);
        parked != ERROR_SUCCESS)
        return fail(parked);
    struct Restore {
        ~Restore() { UnparkDword(HKEY_LOCAL_MACHINE, kPriorityControlKey, kQuantumValueName, kParkedQuantum); }
    } restore;

    const std::wstring stopFile = file + L".stop";
    auto stopped = [&] { return GetFileAttributesW(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto pause = [&](DWORD ms) {
        for (DWORD t = 0; t < ms && !stopped(); t += 100) Sleep(100);
    };

    write("S\n");
    pause(kWarmupMs);
    int step = 0;
    DWORD error = ERROR_SUCCESS;
    for (int r = 0; r < plan.rounds && error == ERROR_SUCCESS && !stopped(); ++r) {
        for (size_t k = 0; k < plan.values.size() && error == ERROR_SUCCESS && !stopped(); ++k) {
            const DWORD value = r % 2 == 0 ? plan.values[k] : plan.values[plan.values.size() - 1 - k];
            error = WriteDword(HKEY_LOCAL_MACHINE, kPriorityControlKey, kQuantumValueName, value);
            if (error != ERROR_SUCCESS) break;
            write("P " + std::to_string(step++) + " " + std::to_string(value) + "\n");
            pause(plan.phaseMs);
        }
    }
    if (error != ERROR_SUCCESS) return fail(error);
    write("D\n");
    CloseHandle(log);
    return 0;
}

}  // namespace

// ------------------------------------------------------------------------------------ public

bool MeasureWakeLatency(DWORD ms, std::stop_token st, std::vector<double>& latenciesUs) {
    double front = 0;
    return MeasureWake(ms, st, &latenciesUs, front);
}

WakeStats SummarizeWake(std::vector<double> v) {
    WakeStats s;
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    double sum = 0;
    size_t late = 0;
    for (double x : v) {
        sum += x;
        if (x > 1000) ++late;
    }
    // Nearest rank: the smallest value that at least p of the samples are at or below.
    auto at = [&](double p) {
        const size_t rank = static_cast<size_t>(std::ceil(p * double(n)));
        return v[std::min(n - 1, rank ? rank - 1 : 0)];
    };
    s.samples = n;
    s.avgUs = sum / double(n);
    s.medianUs = at(0.5);
    s.p99Us = at(0.99);
    s.p999Us = at(0.999);
    s.maxUs = v.back();
    s.lateFraction = double(late) / double(n);
    return s;
}

bool ParseSweepArgs(const std::vector<std::wstring>& args, std::wstring& file, SweepPlan& plan) {
    if (args.size() < 4 || args.size() > 3 + 16) return false;
    DWORD phase = 0, rounds = 0;
    if (!ParseUnsigned(args[1], 5, phase) || phase < 2000 || phase > 30000) return false;
    if (!ParseUnsigned(args[2], 1, rounds) || rounds < 1 || rounds > 4) return false;
    SweepPlan p;
    p.phaseMs = phase;
    p.rounds = static_cast<int>(rounds);
    for (size_t i = 3; i < args.size(); ++i) {
        DWORD v = 0;
        if (!ParseUnsigned(args[i], 3, v) || !ValidTweakEntry({L"quantum", std::to_wstring(v)})) return false;
        if (std::find(p.values.begin(), p.values.end(), v) != p.values.end()) return false;
        p.values.push_back(v);
    }
    file = args[0];
    plan = std::move(p);
    return true;
}

std::vector<DWORD> LatencyTestValues() {
    std::vector<DWORD> out;
    for (const QuantumPreset& p : QuantumPresets()) {
        const bool fixedSlices = ((p.value >> 2) & 3) == 2, boosted = (p.value & 3) != 0;
        if (fixedSlices && boosted) continue;
        out.push_back(p.value);
    }
    return out;
}

SweepOutcome RunLatencySweep(const std::vector<DWORD>& values, std::stop_token st,
                             const std::function<void(const std::wstring&)>& progress) {
    SweepOutcome out;
    if (values.empty()) return out;

    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring file = Format(L"%smtm-latency-%lu-%llu.mtmlat", tmp, GetCurrentProcessId(),
                                     static_cast<unsigned long long>(GetTickCount64()));
    { std::ofstream(file, std::ios::binary | std::ios::trunc); }  // the helper only appends to a file that exists
    const std::wstring stopFile = file + L".stop";

    std::wstring params = Format(L"--latency-sweep \"%s\" %lu %d", file.c_str(), kPhaseMs, kRounds);
    for (DWORD v : values) params += L" " + std::to_wstring(v);
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));

    std::atomic<bool> helperDone{false};
    std::atomic<DWORD> helperCode{ERROR_SUCCESS};
    const DWORD helperTimeout = kWarmupMs + DWORD(values.size()) * kRounds * kPhaseMs + 90000;
    std::jthread helper([&] {
        bool stopWritten = false;
        helperCode = RunElevated(
            exe, params,
            [&] {
                if (st.stop_requested() && !stopWritten) {
                    std::ofstream(stopFile).put('1');
                    stopWritten = true;
                }
            },
            helperTimeout);
        helperDone = true;
    });

    std::vector<std::vector<double>> samples(values.size());
    std::vector<double> frontShare(values.size(), 1.0);
    HANDLE load = nullptr;
    std::streamoff offset = 0;
    std::string partial;
    bool done = false;

    auto handle = [&](const std::string& line) {
        if (line == "S") {
            progress(L"Starting: every core is now busy with background work…");
            load = StartLoad();
        } else if (line[0] == 'P') {
            int step = 0;
            unsigned long value = 0;
            if (sscanf_s(line.c_str(), "P %d %lu", &step, &value) != 2) return;
            const auto it = std::find(values.begin(), values.end(), static_cast<DWORD>(value));
            if (it == values.end()) return;
            const size_t i = static_cast<size_t>(it - values.begin());
            progress(Format(L"Round %d of %d · %s · setting %zu of %zu…", step / int(values.size()) + 1, kRounds,
                            DescribeQuantum(static_cast<DWORD>(value)).c_str(), i + 1, values.size()));
            double ignored = 0, front = 0;
            MeasureWake(kSettleMs, st, nullptr, ignored);  // let the new setting take hold
            if (MeasureWake(kPhaseMs - kSettleMs - kMarginMs, st, &samples[i], front)) frontShare[i] = std::min(frontShare[i], front);
        } else if (line == "D") {
            done = true;
        } else if (line[0] == 'E') {
            unsigned long code = 0;
            if (sscanf_s(line.c_str(), "E %lu", &code) == 1) out.error = static_cast<DWORD>(code);
            done = true;
        }
    };
    auto readNew = [&] {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        in.seekg(offset);
        const std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        offset += static_cast<std::streamoff>(chunk.size());
        partial += chunk;
        bool any = false;
        for (size_t nl; (nl = partial.find('\n')) != std::string::npos; partial.erase(0, nl + 1)) {
            if (nl > 0) handle(partial.substr(0, nl));
            any = true;
        }
        return any;
    };

    while (!done) {
        const bool finished = helperDone;  // read once more after it ends, so no last line is missed
        const bool any = readNew();
        if (finished && !any) break;
        if (!any) Sleep(40);
    }
    helper.join();
    if (load) {
        TerminateProcess(load, 0);
        CloseHandle(load);
    }
    DeleteFileW(file.c_str());
    DeleteFileW(stopFile.c_str());

    out.cancelled = st.stop_requested();
    for (size_t i = 0; i < values.size(); ++i) {
        if (samples[i].empty()) continue;
        LatencyRow row;
        row.value = values[i];
        row.stats = SummarizeWake(std::move(samples[i]));
        row.inFront = frontShare[i] >= kMinFrontShare;
        out.rows.push_back(std::move(row));
    }
    const DWORD code = helperCode;
    if (out.error == ERROR_SUCCESS && code != ERROR_SUCCESS && !out.cancelled) out.error = code;
    out.restoreNeeded = ParkedDwordExists(HKEY_LOCAL_MACHINE, kPriorityControlKey, kParkedQuantum);
    return out;
}

bool HandleLatencyCommandLine(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool match = false;
    if (argc >= 2 && wcscmp(argv[1], L"--latency-load") == 0) {
        match = true;
        exitCode = ERROR_INVALID_PARAMETER;
        DWORD pid = 0, threads = 0;
        if (argc == 4 && ParseUnsigned(argv[2], 10, pid) && pid > 0 && ParseUnsigned(argv[3], 2, threads) && threads >= 1 &&
            threads <= 64) {
            RunLoad(pid, threads);
            exitCode = 0;
        }
    } else if (argc >= 2 && wcscmp(argv[1], L"--latency-sweep") == 0) {
        match = true;
        exitCode = ERROR_INVALID_PARAMETER;
        std::wstring file;
        SweepPlan plan;
        if (ParseSweepArgs(std::vector<std::wstring>(argv + 2, argv + argc), file, plan)) exitCode = RunSweepHelper(file, plan);
    }
    LocalFree(argv);
    return match;
}

}  // namespace mtm
