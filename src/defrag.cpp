#include "defrag.h"

#include "power.h"

#include <objbase.h>
#include <shellapi.h>
#include <winevt.h>
#include <winioctl.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <thread>

namespace mtm {
namespace {

constexpr wchar_t kTaskName[] = L"\\Microsoft\\Windows\\Defrag\\ScheduledDefrag";

std::wstring SystemExe(const wchar_t* name) {
    wchar_t dir[MAX_PATH] = {};
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\" + name;
}

std::wstring FromCodePage(const std::string& s, UINT cp) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Runs a console program with no window, handing each output line (split on \r or \n,
// since defrag redraws its progress with \r) to onLine. `stop` is polled; returning
// true terminates the child. Returns the exit code, or a Win32 error.
DWORD RunCaptured(std::wstring cmdline, const std::function<void(const std::wstring&)>& onLine,
                  const std::function<bool()>& stop = {}) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return GetLastError();
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        const DWORD e = GetLastError();
        CloseHandle(readEnd);
        CloseHandle(writeEnd);
        return e;
    }
    CloseHandle(writeEnd);
    CloseHandle(pi.hThread);
    std::string pending;
    auto flush = [&](bool all) {
        size_t start = 0;
        for (size_t i = 0; i < pending.size(); ++i) {
            if (pending[i] != '\r' && pending[i] != '\n') continue;
            if (i > start) onLine(FromCodePage(pending.substr(start, i - start), CP_OEMCP));
            start = i + 1;
        }
        pending.erase(0, start);
        if (all && !pending.empty()) {
            onLine(FromCodePage(pending, CP_OEMCP));
            pending.clear();
        }
    };
    bool killed = false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr)) break;  // child closed the pipe
        if (avail) {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(readEnd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) || !got) break;
            pending.append(buf, got);
            flush(false);
            continue;
        }
        if (stop && !killed && stop()) {
            // Safe: defrag moves data with FSCTL_MOVE_FILE, which is atomic per extent.
            TerminateProcess(pi.hProcess, ERROR_CANCELLED);
            killed = true;
        }
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            // drain whatever is left
            DWORD left = 0;
            while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &left, nullptr) && left) {
                char buf[4096];
                DWORD got = 0;
                if (!ReadFile(readEnd, buf, std::min<DWORD>(left, sizeof buf), &got, nullptr) || !got) break;
                pending.append(buf, got);
            }
            break;
        }
    }
    flush(true);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(readEnd);
    return killed ? ERROR_CANCELLED : code;
}

std::wstring DefragCommand(wchar_t letter, bool analyze) {
    // /U prints progress, /V the full report. /O = the right optimization for the media type.
    return L"\"" + SystemExe(L"defrag.exe") + L"\" " + std::wstring(1, letter) + (analyze ? L": /A /U /V" : L": /O /U /V");
}

// Interprets a defrag.exe output line: progress, stage name, fragmentation.
void ParseLine(const std::wstring& line, double& percent, std::wstring& stage, OptimizeResult& r) {
    std::wstring t = line;
    while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
    while (!t.empty() && t.back() == L' ') t.pop_back();
    if (t.empty()) return;
    r.log += t + L"\r\n";
    const size_t pct = t.find(L'%');
    if (pct != std::wstring::npos && pct > 0) {
        size_t s = pct;
        while (s > 0 && iswdigit(t[s - 1])) --s;
        if (s < pct) {
            const int v = _wtoi(t.substr(s, pct - s).c_str());
            const std::wstring lower = ToLower(t);
            if (lower.find(L"fragmented space") != std::wstring::npos) r.fragmentedPercent = v;
            if (lower.find(L"complete") != std::wstring::npos) {
                percent = v;
                const size_t colon = t.find(L':');
                if (colon != std::wstring::npos) stage = t.substr(0, colon);
                return;  // progress lines are noise in the summary
            }
        }
    }
    r.summary = t;
}

uint64_t FileTimeFromIso(const std::wstring& iso) {  // "2026-10-01T13:48:52.7532014Z"
    SYSTEMTIME st{};
    if (swscanf_s(iso.c_str(), L"%hu-%hu-%huT%hu:%hu:%hu", &st.wYear, &st.wMonth, &st.wDay, &st.wHour, &st.wMinute, &st.wSecond) != 6)
        return 0;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

constexpr uint64_t kDay = 864000000000ull;  // FILETIME ticks per day

// Latest "optimization completed" (event 258) per drive letter from the Application log.
std::map<wchar_t, std::pair<uint64_t, std::wstring>> LastOptimizations() {
    std::map<wchar_t, std::pair<uint64_t, std::wstring>> out;
    EVT_HANDLE q = EvtQuery(nullptr, L"Application",
                            L"*[System[Provider[@Name='Microsoft-Windows-Defrag'] and (EventID=258)]]",
                            EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!q) return out;
    EVT_HANDLE events[32];
    DWORD got = 0;
    int seen = 0;
    while (seen < 300 && EvtNext(q, 32, events, 2000, 0, &got)) {
        for (DWORD i = 0; i < got; ++i, ++seen) {
            DWORD used = 0, props = 0;
            EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &props);
            std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
            if (EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(xml.size() * sizeof(wchar_t)), xml.data(), &used, &props)) {
                const size_t t = xml.find(L"SystemTime='");
                std::vector<std::wstring> data;
                for (size_t p = 0; (p = xml.find(L"<Data>", p)) != std::wstring::npos;) {
                    p += 6;
                    const size_t e = xml.find(L"</Data>", p);
                    if (e == std::wstring::npos) break;
                    data.push_back(xml.substr(p, e - p));
                }
                if (t != std::wstring::npos && data.size() >= 2) {
                    const uint64_t when = FileTimeFromIso(xml.substr(t + 12, 30));
                    const size_t colon = data[1].rfind(L":)");  // "New Volume (D:)"
                    if (colon != std::wstring::npos && colon > 0) {
                        const wchar_t letter = static_cast<wchar_t>(towupper(data[1][colon - 1]));
                        auto& slot = out[letter];
                        if (when > slot.first) slot = {when, data[0]};
                    }
                }
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(q);
    return out;
}

}  // namespace

std::vector<OptVolume> ListOptimizableVolumes() {
    std::vector<OptVolume> out;
    const auto last = LastOptimizations();
    wchar_t buf[512] = {};
    GetLogicalDriveStringsW(ARRAYSIZE(buf), buf);
    for (const wchar_t* d = buf; *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        wchar_t label[MAX_PATH] = {}, fs[MAX_PATH] = {};
        if (!GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH)) continue;
        const std::wstring fsName = fs;
        if (fsName != L"NTFS" && fsName != L"ReFS") continue;  // FAT/exFAT can't be optimized this way
        OptVolume v;
        v.letter = static_cast<wchar_t>(towupper(d[0]));
        v.label = label;
        v.fileSystem = fsName;
        ULARGE_INTEGER total{}, freeBytes{};
        if (GetDiskFreeSpaceExW(d, nullptr, &total, &freeBytes)) {
            v.size = total.QuadPart;
            v.free = freeBytes.QuadPart;
        }
        // No seek penalty = solid-state. Opening the volume with no access rights needs no admin.
        const std::wstring dev = std::wstring(L"\\\\.\\") + v.letter + L":";
        HANDLE h = CreateFileW(dev.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY q{};
            q.PropertyId = StorageDeviceSeekPenaltyProperty;
            q.QueryType = PropertyStandardQuery;
            DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
            DWORD got = 0;
            if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &got, nullptr) && got >= sizeof sp) {
                v.knownMedia = true;
                v.ssd = !sp.IncursSeekPenalty;
            }
            CloseHandle(h);
        }
        if (auto it = last.find(v.letter); it != last.end()) {
            v.lastOptimized = it->second.first;
            v.lastOperation = it->second.second;
        }
        out.push_back(std::move(v));
    }
    return out;
}

int AutomaticOptimizationEnabled() {
    std::wstring xml;
    const DWORD code = RunCaptured(L"\"" + SystemExe(L"schtasks.exe") + L"\" /Query /TN \"" + kTaskName + L"\" /XML",
                                   [&](const std::wstring& l) { xml += l; });
    if (code != 0) return -1;
    // The task-level switch is <Settings><Enabled>; triggers have their own <Enabled> too.
    const size_t s = xml.find(L"<Settings>"), e = xml.find(L"</Settings>");
    if (s == std::wstring::npos || e == std::wstring::npos) return -1;
    const std::wstring settings = xml.substr(s, e - s);
    // Settings/Enabled defaults to true when absent.
    return settings.find(L"<Enabled>false</Enabled>") != std::wstring::npos ? 0 : 1;
}

DWORD SetAutomaticOptimization(bool enabled) {
    const std::wstring params = std::wstring(L"/Change /TN \"") + kTaskName + L"\" " + (enabled ? L"/Enable" : L"/Disable");
    if (IsElevated()) return RunCaptured(L"\"" + SystemExe(L"schtasks.exe") + L"\" " + params, [](const std::wstring&) {});
    return RunElevated(SystemExe(L"schtasks.exe"), params);
}

OptimizeResult RunOptimization(wchar_t letter, bool analyzeOnly, std::stop_token st,
                               const std::function<void(double, const std::wstring&)>& progress) {
    OptimizeResult r;
    double percent = 0;
    std::wstring stage = analyzeOnly ? L"Analysis" : L"Optimization";
    auto onLine = [&](const std::wstring& line) {
        ParseLine(line, percent, stage, r);
        progress(percent, stage);
    };
    if (IsElevated()) {
        r.code = RunCaptured(DefragCommand(letter, analyzeOnly), onLine, [&] { return st.stop_requested(); });
        return r;
    }

    // Not elevated: run this exe elevated as a helper that runs defrag.exe and appends its
    // output to a temp file, which we tail here. Creating "<file>.stop" asks it to cancel.
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring file = Format(L"%smtm-optimize-%lu-%llu.mtmopt", tmp, GetCurrentProcessId(),
                                     static_cast<unsigned long long>(GetTickCount64()));
    { std::ofstream(file, std::ios::binary | std::ios::trunc); }
    wchar_t exe[MAX_PATH * 2];
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    const std::wstring params = Format(L"--optimize %c %s \"%s\"", letter, analyzeOnly ? L"analyze" : L"optimize", file.c_str());
    std::streamoff offset = 0;
    std::string partial;
    auto tail = [&] {
        std::ifstream in(file, std::ios::binary);
        if (!in) return;
        in.seekg(offset);
        std::string chunk((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        offset += static_cast<std::streamoff>(chunk.size());
        partial += chunk;
        size_t nl;
        while ((nl = partial.find('\n')) != std::string::npos) {
            onLine(FromCodePage(partial.substr(0, nl), CP_UTF8));
            partial.erase(0, nl + 1);
        }
    };
    bool stopSent = false;
    r.code = RunElevated(exe, params, [&] {
        tail();
        if (st.stop_requested() && !stopSent) {
            std::ofstream(file + L".stop").put('1');
            stopSent = true;
        }
    });
    tail();
    DeleteFileW(file.c_str());
    DeleteFileW((file + L".stop").c_str());
    return r;
}

// The caller creates the progress file before starting the helper, so the elevated process only
// ever appends to an existing plain file named like ours: it never creates a file elsewhere and
// never follows a link to one.
HANDLE OpenProgressFile(const std::wstring& file) {
    const size_t slash = file.find_last_of(L"\\/");
    const std::wstring name = ToLower(slash == std::wstring::npos ? file : file.substr(slash + 1));
    if (!name.starts_with(L"mtm-optimize-") || !name.ends_with(L".mtmopt")) return INVALID_HANDLE_VALUE;
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return h;
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

bool HandleOptimizeCommandLine(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    const bool match = argc >= 2 && wcscmp(argv[1], L"--optimize") == 0;
    if (match) {
        exitCode = ERROR_INVALID_PARAMETER;
        // Strictly validated: one drive letter, a known mode, and our own progress file.
        const std::wstring file = argc == 5 ? argv[4] : L"";
        const bool okLetter = argc == 5 && wcslen(argv[2]) == 1 && iswalpha(argv[2][0]);
        const bool okMode = argc == 5 && (wcscmp(argv[3], L"analyze") == 0 || wcscmp(argv[3], L"optimize") == 0);
        HANDLE out = okLetter && okMode ? OpenProgressFile(file) : INVALID_HANDLE_VALUE;
        if (out != INVALID_HANDLE_VALUE) {
            const std::wstring stopFile = file + L".stop";
            exitCode = static_cast<int>(RunCaptured(
                DefragCommand(static_cast<wchar_t>(towupper(argv[2][0])), wcscmp(argv[3], L"analyze") == 0),
                [&](const std::wstring& line) {
                    const std::string text = ToUtf8(line) + "\n";
                    DWORD written = 0;
                    WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
                },
                [&] { return GetFileAttributesW(stopFile.c_str()) != INVALID_FILE_ATTRIBUTES; }));
            CloseHandle(out);
        }
    }
    LocalFree(argv);
    return match;
}

int OptimizeReminderDays() { return static_cast<int>(ReadSetting(L"OptimizeReminderDays", 30)); }
void SetOptimizeReminderDays(int days) { WriteSetting(L"OptimizeReminderDays", static_cast<DWORD>(std::max(0, days))); }

void SnoozeOptimizeReminder(int days) {
    const uint64_t until = NowFileTime() + uint64_t(days) * kDay;
    WriteSettingBinary(L"OptimizeSnoozeUntil", &until, sizeof until);
}

bool OptimizationDue(const std::vector<OptVolume>& volumes) {
    const int days = OptimizeReminderDays();
    if (days <= 0 || volumes.empty()) return false;
    const uint64_t now = NowFileTime();
    uint64_t snooze = 0;
    if (ReadSettingBinary(L"OptimizeSnoozeUntil", &snooze, sizeof snooze) && now < snooze) return false;
    for (const OptVolume& v : volumes)
        if (!v.lastOptimized || now - v.lastOptimized > uint64_t(days) * kDay) return true;
    return false;
}

}  // namespace mtm
