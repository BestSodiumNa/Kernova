#include "power.h"

#include <objbase.h>
#include <powrprof.h>
#include <shellapi.h>

#include <algorithm>

namespace mtm {
namespace {

// Spelled out here rather than relying on initguid / the SDK's DEFINE_GUID linkage.
constexpr GUID kSubProcessor = {0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID kMinCores = {0x0cc5b647, 0xc1df, 0x4637, {0x89, 0x1a, 0xde, 0xc3, 0x5c, 0x31, 0x85, 0x83}};
constexpr GUID kMaxState = {0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
constexpr GUID kMinState = {0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};
constexpr GUID kBoostMode = {0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};

constexpr GUID kBalanced = {0x381b4222, 0xf694, 0x41f0, {0x96, 0x85, 0xff, 0x5b, 0xb2, 0x60, 0xdf, 0x2e}};
constexpr GUID kHighPerf = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xcf, 0x2e, 0x1f, 0x3d, 0xc7, 0xc4}};
constexpr GUID kPowerSaver = {0xa1841308, 0x3541, 0x4fab, {0xbc, 0x81, 0xf7, 0x15, 0x56, 0xf2, 0x0b, 0x4a}};
constexpr GUID kUltimate = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x00, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61}};

struct PresetRecipe {
    GUID base;
    bool custom;          // renamed + tuned copy (vs. a plain copy of a Windows plan)
    int maxState = -1;    // % (AC and DC), -1 = leave
    int minState = -1;
    int boost = -1;       // 0 off, 1 on, 2 aggressive
    int minCores = -1;    // core parking: % of cores kept unparked
};

// Order matches PowerPresets().
const PresetRecipe kRecipes[] = {
    {kBalanced, false},
    {kPowerSaver, false},
    {kHighPerf, false},
    {kUltimate, false},
    {kHighPerf, true, 100, 5, 2, 100},   // Gaming: all cores awake, aggressive boost
    {kBalanced, true, 99, 5, 0, -1},     // Cool & quiet: 99% max disables turbo boost
    {kPowerSaver, true, 80, 5, 0, -1},   // Max battery
};

bool SameGuid(const GUID& a, const GUID& b) { return memcmp(&a, &b, sizeof a) == 0; }

std::wstring FriendlyName(const GUID& scheme) {
    DWORD size = 0;
    if (PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return L"";
    std::vector<BYTE> buf(size);
    if (PowerReadFriendlyName(nullptr, &scheme, nullptr, nullptr, buf.data(), &size) != ERROR_SUCCESS) return L"";
    return reinterpret_cast<const wchar_t*>(buf.data());
}

std::wstring PresetPlanName(size_t i) { return std::wstring(L"Kernova · ") + PowerPresets()[i].name; }
// Plans made under the app's earlier names are still recognised as ours.
bool IsLegacyPresetPlanName(const std::wstring& name, size_t i) {
    for (const wchar_t* prefix : {L"BetterTaskManager · ", L"MyTaskManager · "})
        if (name == std::wstring(prefix) + PowerPresets()[i].name) return true;
    return false;
}

}  // namespace

std::wstring GuidText(const GUID& g) {
    wchar_t buf[64] = {};
    StringFromGUID2(g, buf, ARRAYSIZE(buf));
    return buf;
}

bool ActiveScheme(GUID& out) {
    GUID* active = nullptr;
    if (PowerGetActiveScheme(nullptr, &active) != ERROR_SUCCESS || !active) return false;
    out = *active;
    LocalFree(active);
    return true;
}

DWORD RunElevated(const std::wstring& file, const std::wstring& params, const std::function<void()>& whileWaiting,
                  DWORD timeoutMs) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW sei{sizeof sei};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = g_main;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    DWORD result;
    if (!ShellExecuteExW(&sei)) {
        result = GetLastError();  // ERROR_CANCELLED when the UAC prompt is declined
    } else {
        if (whileWaiting) {
            const ULONGLONG start = GetTickCount64();
            while (WaitForSingleObject(sei.hProcess, 150) == WAIT_TIMEOUT) {
                whileWaiting();
                if (timeoutMs != INFINITE && GetTickCount64() - start >= timeoutMs) break;
            }
        } else {
            WaitForSingleObject(sei.hProcess, timeoutMs);
        }
        if (!GetExitCodeProcess(sei.hProcess, &result)) result = GetLastError();
        else if (result == STILL_ACTIVE) result = WAIT_TIMEOUT;  // timed out: the helper is still running
        CloseHandle(sei.hProcess);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}

DWORD RunSelfElevated(const std::wstring& params, DWORD timeoutMs) {
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    return RunElevated(exe, params, {}, timeoutMs);
}

DWORD WithElevation(const std::function<DWORD()>& fn, const std::wstring& helperParams) {
    const DWORD r = fn();
    if (r != ERROR_ACCESS_DENIED || IsElevated()) return r;
    return RunSelfElevated(helperParams);
}

namespace {

DWORD WriteBoth(const GUID& scheme, const GUID& setting, DWORD value) {
    DWORD r = PowerWriteACValueIndex(nullptr, &scheme, &kSubProcessor, &setting, value);
    if (r == ERROR_SUCCESS) r = PowerWriteDCValueIndex(nullptr, &scheme, &kSubProcessor, &setting, value);
    return r;
}

// Processor settings that make a Balanced copy behave like a Windows plan that this PC
// doesn't have. Modern Standby laptops ship with only Balanced: the others aren't
// hidden, they're absent, so duplicating them fails with ERROR_FILE_NOT_FOUND.
void ApplyBaseLook(const GUID& g, const GUID& base) {
    if (SameGuid(base, kHighPerf) || SameGuid(base, kUltimate)) {
        WriteBoth(g, kMinState, 100);  // no ramp-up delay: the CPU idles at full clock
        WriteBoth(g, kMaxState, 100);
        WriteBoth(g, kBoostMode, 2);   // aggressive
        WriteBoth(g, kMinCores, 100);  // no core parking
    } else if (SameGuid(base, kPowerSaver)) {
        WriteBoth(g, kMinState, 5);
        WriteBoth(g, kMaxState, 80);
        WriteBoth(g, kBoostMode, 0);   // no boost
    }
}

// The name a preset's plan gets: Windows' own name for its plans, ours for the custom ones.
std::wstring TargetName(size_t index) {
    if (kRecipes[index].custom) return PresetPlanName(index);
    std::wstring n = PowerPresets()[index].name;  // "High performance (Windows)"
    const size_t p = n.find(L" (Windows)");
    return p == std::wstring::npos ? n : n.substr(0, p);
}

DWORD CreatePreset(size_t index) {
    if (index >= std::size(kRecipes)) return ERROR_INVALID_PARAMETER;
    const PresetRecipe& r = kRecipes[index];
    const std::wstring target = TargetName(index);
    // Already on this PC (the Windows original, or one we made earlier): just switch to it.
    for (const PowerPlan& p : ListPowerPlans())
        if ((!r.custom && SameGuid(p.guid, r.base)) || p.name == target || (r.custom && IsLegacyPresetPlanName(p.name, index)))
            return PowerSetActiveScheme(nullptr, &p.guid);

    GUID* created = nullptr;
    bool rebuilt = false;
    DWORD res = PowerDuplicateScheme(nullptr, &r.base, &created);
    if (res != ERROR_SUCCESS || !created) {
        // The plan to copy doesn't exist here: start from Balanced and dial in its settings.
        created = nullptr;
        res = PowerDuplicateScheme(nullptr, &kBalanced, &created);
        rebuilt = true;
        if (res != ERROR_SUCCESS || !created) return res ? res : ERROR_GEN_FAILURE;
    }
    const GUID g = *created;
    LocalFree(created);
    if (rebuilt) ApplyBaseLook(g, r.base);
    if (r.custom || rebuilt) {
        std::wstring name = target;
        PowerWriteFriendlyName(nullptr, &g, nullptr, nullptr, reinterpret_cast<UCHAR*>(name.data()),
                               static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    }
    if (r.maxState >= 0) WriteBoth(g, kMaxState, DWORD(r.maxState));
    if (r.minState >= 0) WriteBoth(g, kMinState, DWORD(r.minState));
    if (r.boost >= 0) WriteBoth(g, kBoostMode, DWORD(r.boost));
    if (r.minCores >= 0) WriteBoth(g, kMinCores, DWORD(r.minCores));
    return PowerSetActiveScheme(nullptr, &g);
}

DWORD WriteParking(bool disable) {
    GUID scheme{};
    if (!ActiveScheme(scheme)) return ERROR_NOT_FOUND;
    DWORD ac = 100, dc = 100;
    if (!disable) {  // back to the plan's own default
        PowerReadACDefaultIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, &ac);
        PowerReadDCDefaultIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, &dc);
        if (ac >= 100 && dc >= 100) ac = dc = 5;  // plan's default is "never park"; Balanced uses ~5%
    }
    DWORD r = PowerWriteACValueIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, ac);
    if (r == ERROR_SUCCESS) r = PowerWriteDCValueIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, dc);
    if (r == ERROR_SUCCESS) r = PowerSetActiveScheme(nullptr, &scheme);  // re-apply so it takes effect now
    return r;
}

}  // namespace

const std::vector<PowerPreset>& PowerPresets() {
    static const std::vector<PowerPreset> list = {
        {L"Balanced (Windows)", L"Windows' default: fast when busy, saves power when idle",
         L"Raises clocks as soon as there's work and lowers them (and parks cores) when idle.",
         L"The best everyday choice for most people; laptops stay cool and quiet."},
        {L"Power saver (Windows)", L"Lower clocks and dimmer screen to stretch battery life",
         L"Caps processor performance, turns off boost sooner and saves power more aggressively.",
         L"Long flights, low battery, or when you only need web and documents."},
        {L"High performance (Windows)", L"Keeps the CPU ready at high clocks; uses more power",
         L"Raises the minimum processor state and sleeps devices less, so there's less ramp-up delay.",
         L"Desktops and plugged-in laptops doing heavy work. Laptops often hide this plan; adding it restores it."},
        {L"Ultimate Performance (Windows)", L"Removes nearly every power-saving delay (hidden plan)",
         L"Windows' hidden workstation plan: no idle throttling, devices never power down.",
         L"Benchmarks and pro workloads where every millisecond of latency matters. Uses the most power."},
        {L"Gaming", L"High performance with core parking off and aggressive boost",
         L"Copy of High performance with all cores kept awake (no core parking) and boost set to aggressive.",
         L"Smoother frame times in games that suffer from cores waking up late. Plug in; it runs warmer."},
        {L"Cool & quiet", L"Balanced with turbo boost disabled (max processor state 99%)",
         L"Copy of Balanced with the maximum processor state at 99%, which stops turbo boost.",
         L"Laptops that run hot or loud: often 10–20 °C cooler and much quieter for a small speed loss."},
        {L"Max battery", L"Power saver with clocks capped at 80% and no boost",
         L"Copy of Power saver with the maximum processor state at 80% and boost off.",
         L"Squeezing the most time out of a battery; fine for browsing, video and writing."},
    };
    return list;
}

std::vector<PowerPlan> ListPowerPlans() {
    std::vector<PowerPlan> out;
    GUID active{};
    const bool haveActive = ActiveScheme(active);
    for (ULONG i = 0;; ++i) {
        GUID g{};
        DWORD size = sizeof g;
        if (PowerEnumerate(nullptr, nullptr, nullptr, ACCESS_SCHEME, i, reinterpret_cast<UCHAR*>(&g), &size) != ERROR_SUCCESS)
            break;
        PowerPlan p;
        p.guid = g;
        p.name = FriendlyName(g);
        p.active = haveActive && SameGuid(g, active);
        for (const GUID& b : {kBalanced, kHighPerf, kPowerSaver, kUltimate})
            if (SameGuid(g, b)) p.builtIn = true;
        for (size_t k = 0; k < PowerPresets().size(); ++k)
            if (!p.builtIn && (p.name == TargetName(k) || (kRecipes[k].custom && IsLegacyPresetPlanName(p.name, k))))
                p.presetIndex = static_cast<int>(k);  // ours, incl. rebuilt Windows plans and pre-rename ones
        out.push_back(std::move(p));
    }
    return out;
}

DWORD ActivatePowerPlan(const GUID& plan) { return PowerSetActiveScheme(nullptr, &plan); }

DWORD DeletePowerPlan(const GUID& plan) {
    return WithElevation([&] { return PowerDeleteScheme(nullptr, &plan); }, L"--power delete " + GuidText(plan));
}

DWORD AddPowerPreset(size_t index) {
    return WithElevation([&] { return CreatePreset(index); }, Format(L"--power preset %zu", index));
}

ParkingState ReadCoreParking() {
    ParkingState s;
    GUID scheme{};
    if (!ActiveScheme(scheme)) return s;
    s.known = PowerReadACValueIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, &s.minCoresAc) == ERROR_SUCCESS &&
              PowerReadDCValueIndex(nullptr, &scheme, &kSubProcessor, &kMinCores, &s.minCoresDc) == ERROR_SUCCESS;
    return s;
}

DWORD SetCoreParkingDisabled(bool disable) {
    return WithElevation([&] { return WriteParking(disable); }, disable ? L"--power parking off" : L"--power parking on");
}

bool HandlePowerCommandLine(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    const bool match = argc >= 2 && wcscmp(argv[1], L"--power") == 0;
    if (match) {
        // Only these three exact forms are accepted.
        exitCode = ERROR_INVALID_PARAMETER;
        if (argc == 4 && wcscmp(argv[2], L"parking") == 0 && (!wcscmp(argv[3], L"on") || !wcscmp(argv[3], L"off"))) {
            exitCode = static_cast<int>(WriteParking(wcscmp(argv[3], L"off") == 0));
        } else if (argc == 4 && wcscmp(argv[2], L"preset") == 0 && wcslen(argv[3]) == 1 && iswdigit(argv[3][0])) {
            exitCode = static_cast<int>(CreatePreset(static_cast<size_t>(argv[3][0] - L'0')));
        } else if (argc == 4 && wcscmp(argv[2], L"delete") == 0) {
            GUID g{};
            if (SUCCEEDED(IIDFromString(argv[3], &g))) exitCode = static_cast<int>(PowerDeleteScheme(nullptr, &g));
        }
    }
    LocalFree(argv);
    return match;
}

}  // namespace mtm
