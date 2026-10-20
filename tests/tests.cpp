// Checks the code that reads untrusted input or decides what the elevated helpers may write.
// Run with: ctest --test-dir build --output-on-failure   (or build\kernova_tests.exe)
#include "recorder.h"
#include "tweaks.h"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using mtm::FlightRecorder;
using mtm::TraceSample;

// ---- what the elevated --tweak helper is willing to write ------------------------------

void TestTweakEntries() {
    using mtm::ValidTweakEntry;
    CHECK(!ValidTweakEntry({}));

    CHECK(ValidTweakEntry({L"quantum", L"40"}));
    CHECK(ValidTweakEntry({L"quantum", L"63"}));
    CHECK(ValidTweakEntry({L"quantum", L"-"}));   // "-" = remove the value
    CHECK(!ValidTweakEntry({L"quantum", L"64"}));  // above 0x3F
    CHECK(!ValidTweakEntry({L"quantum", L"1000"}));
    CHECK(!ValidTweakEntry({L"quantum", L"4a"}));
    CHECK(!ValidTweakEntry({L"quantum", L"+5"}));
    CHECK(!ValidTweakEntry({L"quantum", L""}));
    CHECK(!ValidTweakEntry({L"quantum"}));
    CHECK(!ValidTweakEntry({L"quantum", L"1", L"2"}));

    CHECK(ValidTweakEntry({L"usb", L"{381B4222-F694-41F0-9685-FF5BB260DF2E}", L"0", L"1"}));
    CHECK(!ValidTweakEntry({L"usb", L"{x}", L"-", L"1"}));  // both values are required
    CHECK(!ValidTweakEntry({L"usb", L"{x}", L"2", L"1"}));
    CHECK(!ValidTweakEntry({L"usb", L"{x}", L"0"}));

    CHECK(ValidTweakEntry({L"PCI\\VEN_10DE&DEV_2520&SUBSYS_00000000&REV_A1\\4&1&0&0008", L"1", L"3"}));
    CHECK(ValidTweakEntry({L"pci\\x", L"-", L"-"}));
    CHECK(!ValidTweakEntry({L"ROOT\\x", L"1", L"3"}));  // only PCI display adapters
    CHECK(!ValidTweakEntry({L"C:\\Windows", L"1", L"3"}));
    CHECK(!ValidTweakEntry({L"PCI\\x", L"2", L"3"}));
    CHECK(!ValidTweakEntry({L"PCI\\x", L"1", L"4"}));
    CHECK(!ValidTweakEntry({L"PCI\\x", L"1"}));

    // Every scheduler preset must be something the helper accepts, or choosing it would fail after the
    // UAC prompt; and none may be listed twice.
    std::vector<DWORD> seen;
    for (const mtm::QuantumPreset& p : mtm::QuantumPresets()) {
        CHECK(ValidTweakEntry({L"quantum", std::to_wstring(p.value)}));
        CHECK(std::find(seen.begin(), seen.end(), p.value) == seen.end());
        seen.push_back(p.value);
    }
    CHECK(seen.size() == 13);  // the three named ones plus the ten other length/fixed/boost combinations

    // Values that don't fit what the helper may write (these are what the backup refuses to record).
    CHECK(!ValidTweakEntry({L"quantum", L"2147483648"}));
    CHECK(!ValidTweakEntry({L"usb", L"{x}", L"4294967295", L"0"}));
    CHECK(!ValidTweakEntry({L"PCI\\x", L"4294967295", L"0"}));
}

// ---- registry DWORDs and the parked restore-point frequency -------------------------------
// Runs against a throwaway key under HKCU, so no admin rights are needed.

void TestRegistryDwords() {
    using mtm::ParkDword;
    using mtm::ReadDword;
    using mtm::UnparkDword;
    using mtm::WriteDword;
    HKEY root = HKEY_CURRENT_USER;
    const wchar_t* sub = L"Software\\KernovaTests";
    RegDeleteTreeW(root, sub);

    // A DWORD keeps its whole range; "not set" is only ever nullopt.
    CHECK(WriteDword(root, sub, L"v", 0x80000000u) == ERROR_SUCCESS);
    CHECK(ReadDword(root, sub, L"v") == 0x80000000u);
    CHECK(WriteDword(root, sub, L"v", 0xFFFFFFFFu) == ERROR_SUCCESS);
    CHECK(ReadDword(root, sub, L"v") == 0xFFFFFFFFu);
    CHECK(WriteDword(root, sub, L"v", 0u) == ERROR_SUCCESS);
    CHECK(ReadDword(root, sub, L"v") == 0u);
    CHECK(WriteDword(root, sub, L"v", std::nullopt) == ERROR_SUCCESS);
    CHECK(!ReadDword(root, sub, L"v"));
    CHECK(WriteDword(root, sub, L"v", std::nullopt) == ERROR_SUCCESS);  // removing what isn't there is fine
    RegSetKeyValueW(root, sub, L"text", REG_SZ, L"5", 2 * sizeof(wchar_t));
    CHECK(!ReadDword(root, sub, L"text"));  // not a DWORD

    auto freq = [&] { return ReadDword(root, sub, L"freq"); };
    auto parked = [&] {
        DWORD size = 0;
        return RegGetValueW(root, sub, L"parked", RRF_RT_ANY, nullptr, nullptr, &size) == ERROR_SUCCESS;
    };
    auto park = [&] { return ParkDword(root, sub, L"freq", L"parked"); };
    auto unpark = [&] { return UnparkDword(root, sub, L"freq", L"parked"); };

    // Lifted to 0 and put back.
    WriteDword(root, sub, L"freq", 1440u);
    CHECK(park() == ERROR_SUCCESS && parked());
    WriteDword(root, sub, L"freq", 0u);
    CHECK(unpark() == ERROR_SUCCESS);
    CHECK(freq() == 1440u && !parked());

    // Not set before: it is removed again, not left at 0.
    WriteDword(root, sub, L"freq", std::nullopt);
    CHECK(park() == ERROR_SUCCESS);
    WriteDword(root, sub, L"freq", 0u);
    CHECK(unpark() == ERROR_SUCCESS);
    CHECK(!freq() && !parked());

    // A value above INT_MAX survives the round trip.
    WriteDword(root, sub, L"freq", 0xFFFFFFFFu);
    CHECK(park() == ERROR_SUCCESS);
    WriteDword(root, sub, L"freq", 0u);
    CHECK(unpark() == ERROR_SUCCESS);
    CHECK(freq() == 0xFFFFFFFFu);

    // Nothing parked: nothing changes (this is what every normal helper run does first).
    WriteDword(root, sub, L"freq", 7u);
    CHECK(unpark() == ERROR_SUCCESS);
    CHECK(freq() == 7u);

    // A run that was killed leaves the original parked; parking again must not overwrite it with 0.
    WriteDword(root, sub, L"freq", 5u);
    CHECK(park() == ERROR_SUCCESS);
    WriteDword(root, sub, L"freq", 0u);
    CHECK(park() == ERROR_ALREADY_EXISTS);
    CHECK(unpark() == ERROR_SUCCESS);
    CHECK(freq() == 5u);

    // A parked value that isn't ours is dropped and the setting is left alone.
    for (const wchar_t* junk : {L"abc", L"", L"12345678901", L"4294967296", L"-5"}) {
        WriteDword(root, sub, L"freq", 9u);
        RegSetKeyValueW(root, sub, L"parked", REG_SZ, junk, DWORD((wcslen(junk) + 1) * sizeof(wchar_t)));
        CHECK(unpark() == ERROR_INVALID_DATA);
        CHECK(freq() == 9u && !parked());
    }

    RegDeleteTreeW(root, sub);
}

// ---- the .mtmtrace loader ---------------------------------------------------------------

std::wstring TempFile(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, dir);
    return std::wstring(dir) + L"kernova-test-" + name;
}

std::vector<char> ReadAll(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteAll(const std::wstring& path, const std::vector<char>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

TraceSample MakeSample(uint64_t t) {
    TraceSample s{};
    s.time = t;
    s.monotonic = t * 10;
    s.cpu = 12.5f;
    s.gpu = 3.0f;
    s.diskActive = 4.0f;
    s.memUsed = 5;
    s.memTotal = 16;
    s.netIn = 7;
    s.gpuMemory = 9;
    s.coreUsage = {1.0f, 2.0f, 3.0f};
    s.coreMhz = {3000, 3100, 3200};
    s.top = {{42, 8.0f, 1234, L"chrome.exe"}, {7, 1.5f, 99, L"Kernova.exe"}};
    return s;
}

void TestTraceFiles() {
    const std::wstring path = TempFile(L"trace.mtmtrace");
    const std::vector<TraceSample> samples = {MakeSample(1), MakeSample(2), MakeSample(3)};
    std::wstring error;
    std::vector<TraceSample> loaded;

    // Round trip.
    CHECK(FlightRecorder::Save(path, samples, error));
    CHECK(FlightRecorder::Load(path, loaded, error));
    CHECK(loaded.size() == 3);
    if (loaded.size() == 3) {
        CHECK(loaded[2].time == 3 && loaded[2].monotonic == 30);
        CHECK(loaded[0].cpu == 12.5f && loaded[0].memTotal == 16 && loaded[0].gpuMemory == 9);
        CHECK(loaded[1].coreMhz == samples[1].coreMhz && loaded[1].coreUsage == samples[1].coreUsage);
        CHECK(loaded[1].top.size() == 2 && loaded[1].top[0].name == L"chrome.exe" && loaded[1].top[1].pid == 7);
    }

    const std::vector<char> good = ReadAll(path);
    CHECK(good.size() > 16);

    // A truncated final record is accepted: the earlier samples are kept.
    std::vector<char> cut(good.begin(), good.end() - 5);
    WriteAll(path, cut);
    CHECK(FlightRecorder::Load(path, loaded, error));
    CHECK(loaded.size() == 2);

    // A flipped byte inside the first record fails the checksum; nothing is returned.
    std::vector<char> bad = good;
    bad[16 + 8 + 4] ^= 0x55;
    WriteAll(path, bad);
    CHECK(!FlightRecorder::Load(path, loaded, error));
    CHECK(loaded.empty());
    CHECK(error.find(L"corrupt") != std::wstring::npos);

    // Wrong magic, unsupported version, too short to hold a header.
    bad = good;
    bad[0] = 'X';
    WriteAll(path, bad);
    CHECK(!FlightRecorder::Load(path, loaded, error));
    bad = good;
    bad[8] = 2;
    WriteAll(path, bad);
    CHECK(!FlightRecorder::Load(path, loaded, error));
    WriteAll(path, std::vector<char>(good.begin(), good.begin() + 10));
    CHECK(!FlightRecorder::Load(path, loaded, error));
    WriteAll(path, {});
    CHECK(!FlightRecorder::Load(path, loaded, error));

    // Header only: valid file, no samples.
    WriteAll(path, std::vector<char>(good.begin(), good.begin() + 16));
    CHECK(!FlightRecorder::Load(path, loaded, error));

    // Over the size cap: refused before anything is mapped or decoded.
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER big{};
        big.QuadPart = 300ll << 20;
        SetFilePointerEx(h, big, nullptr, FILE_BEGIN);
        SetEndOfFile(h);
        CloseHandle(h);
        CHECK(!FlightRecorder::Load(path, loaded, error));
        CHECK(error.find(L"larger") != std::wstring::npos);
    } else {
        CHECK(false);
    }

    DeleteFileW(path.c_str());
}

}  // namespace

int main() {
    TestTweakEntries();
    TestRegistryDwords();
    TestTraceFiles();
    std::printf(g_failures ? "%d check(s) failed\n" : "all checks passed\n", g_failures);
    return g_failures ? 1 : 0;
}
