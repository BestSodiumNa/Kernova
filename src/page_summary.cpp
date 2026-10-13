#include <winsock2.h>
#include <ws2tcpip.h>

#include "graph.h"
#include "icons.h"
#include "page.h"
#include "processes.h"
#include "recorder.h"
#include "sysinfo.h"
#include "theme.h"

#include <iphlpapi.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstddef>

namespace mtm {
namespace {

// ---------------------------------------------------------------- data helpers

std::wstring RegString(const wchar_t* key, const wchar_t* value) {
    wchar_t buf[256] = {};
    DWORD size = sizeof buf;
    return RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}

// "Windows 11 Pro 24H2 (build 26100.4061)". ProductName still says "Windows 10" on
// Windows 11, so the build number decides.
std::wstring WindowsVersion() {
    constexpr wchar_t kKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::wstring product = RegString(kKey, L"ProductName");
    const std::wstring build = RegString(kKey, L"CurrentBuild");
    const std::wstring display = RegString(kKey, L"DisplayVersion");
    DWORD ubr = 0, size = sizeof ubr;
    RegGetValueW(HKEY_LOCAL_MACHINE, kKey, L"UBR", RRF_RT_REG_DWORD, nullptr, &ubr, &size);
    if (!build.empty() && _wtoi(build.c_str()) >= 22000) {
        const size_t pos = product.find(L"Windows 10");
        if (pos != std::wstring::npos) product.replace(pos, 10, L"Windows 11");
    }
    return Format(L"%s %s (build %s.%lu)", product.c_str(), display.c_str(), build.c_str(), ubr);
}

std::wstring ComputerName() {
    wchar_t buf[256] = {};
    DWORD size = ARRAYSIZE(buf);
    return GetComputerNameExW(ComputerNameDnsHostname, buf, &size) ? buf : L"";
}

std::wstring UserName() {
    wchar_t buf[256] = {};
    DWORD size = ARRAYSIZE(buf);
    return GetUserNameW(buf, &size) ? buf : L"";
}

std::wstring PowerText() {
    SYSTEM_POWER_STATUS s{};
    if (!GetSystemPowerStatus(&s)) return L"Unknown";
    std::wstring text = s.ACLineStatus == 1 ? L"Plugged in" : (s.ACLineStatus == 0 ? L"On battery" : L"Unknown");
    if (s.BatteryFlag != 128 && s.BatteryFlag != 255 && s.BatteryLifePercent <= 100)
        text += Format(L"  ·  %u%%%s", s.BatteryLifePercent, (s.BatteryFlag & 8) ? L" charging" : L"");
    return text;
}

struct NetCounts {
    size_t established = 0, listening = 0, exposed = 0;
};

// TCP only: listening sockets bound to 0.0.0.0 / :: are reachable on every interface.
NetCounts CountTcp() {
    NetCounts c;
    for (ULONG af : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
        DWORD size = 0;
        if (GetExtendedTcpTable(nullptr, &size, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0) != ERROR_INSUFFICIENT_BUFFER) continue;
        std::vector<BYTE> buf(size + 16 * 1024);  // headroom: the table can grow between calls
        size = static_cast<DWORD>(buf.size());
        if (GetExtendedTcpTable(buf.data(), &size, FALSE, af, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) continue;
        if (af == AF_INET) {
            const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
            const size_t max = (size - offsetof(MIB_TCPTABLE_OWNER_PID, table)) / sizeof(MIB_TCPROW_OWNER_PID);
            for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
                const auto& r = t->table[i];
                if (r.dwState == MIB_TCP_STATE_LISTEN) {
                    ++c.listening;
                    if (r.dwLocalAddr == 0) ++c.exposed;
                } else if (r.dwState == MIB_TCP_STATE_ESTAB) {
                    ++c.established;
                }
            }
        } else {
            const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
            const size_t max = (size - offsetof(MIB_TCP6TABLE_OWNER_PID, table)) / sizeof(MIB_TCP6ROW_OWNER_PID);
            static const UCHAR kAny[16] = {};
            for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
                const auto& r = t->table[i];
                if (r.dwState == MIB_TCP_STATE_LISTEN) {
                    ++c.listening;
                    if (memcmp(r.ucLocalAddr, kAny, 16) == 0) ++c.exposed;
                } else if (r.dwState == MIB_TCP_STATE_ESTAB) {
                    ++c.established;
                }
            }
        }
    }
    return c;
}

struct ServiceCounts {
    size_t total = 0, running = 0;
};

ServiceCounts CountServices() {
    ServiceCounts c;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return c;
    std::vector<BYTE> buf(64 * 1024);
    DWORD resume = 0;
    for (;;) {
        DWORD needed = 0, count = 0;
        const BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO, SERVICE_WIN32, SERVICE_STATE_ALL, buf.data(),
                                              static_cast<DWORD>(buf.size()), &needed, &count, &resume, nullptr);
        if (!ok && GetLastError() != ERROR_MORE_DATA) break;
        const auto* items = reinterpret_cast<const ENUM_SERVICE_STATUS_PROCESSW*>(buf.data());
        for (DWORD i = 0; i < count; ++i) {
            ++c.total;
            if (items[i].ServiceStatusProcess.dwCurrentState == SERVICE_RUNNING) ++c.running;
        }
        if (ok) break;
        if (needed > buf.size()) buf.resize(needed);
    }
    CloseServiceHandle(scm);
    return c;
}

size_t CountSignedInUsers() {
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    size_t users = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) return 0;
    for (DWORD i = 0; i < count; ++i) {
        LPWSTR name = nullptr;
        DWORD bytes = 0;
        if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessions[i].SessionId, WTSUserName, &name, &bytes) &&
            name && name[0])
            ++users;
        if (name) WTSFreeMemory(name);
    }
    WTSFreeMemory(sessions);
    return users;
}

struct Volume {
    std::wstring root, label;
    uint64_t total = 0, free = 0;
};

std::vector<Volume> Volumes() {
    std::vector<Volume> out;
    wchar_t buf[512] = {};
    GetLogicalDriveStringsW(ARRAYSIZE(buf), buf);
    for (const wchar_t* d = buf; *d; d += wcslen(d) + 1) {
        const UINT type = GetDriveTypeW(d);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE) continue;
        Volume v;
        v.root = d;
        wchar_t label[MAX_PATH] = {};
        GetVolumeInformationW(d, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        v.label = label;
        ULARGE_INTEGER freeBytes{}, total{};
        if (!GetDiskFreeSpaceExW(d, &freeBytes, &total, nullptr) || !total.QuadPart) continue;
        v.total = total.QuadPart;
        v.free = freeBytes.QuadPart;
        out.push_back(std::move(v));
    }
    return out;
}

// ---------------------------------------------------------------- page

class SummaryPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Summary"; }

    void Create() override {
        cpu_.Create(Graph::Tint::Cpu, 100);
        mem_.Create(Graph::Tint::Memory, 100);
        gpu_.Create(Graph::Tint::Gpu, 100);
        disk_.Create(Graph::Tint::Disk, 100);
        net_.Create(Graph::Tint::Network, 0);
        topCpu_.Create(L"Top processes · CPU");
        topMem_.Create(L"Top processes · Memory");
        system_.Create(L"System");
        storage_.Create(L"Storage");
        network_.Create(L"Network & exposure");
        activity_.Create(L"Activity");
    }

    // Wide windows: 5 tiles in a row, cards in 3 columns. Narrow ones: 3 tiles per row and
    // 2 card columns, so text keeps its room; the page scrolls if that gets too tall.
    static bool Narrow(int width) { return width < Scale(1180); }
    static int TileCols(int width) { return Narrow(width) ? 3 : 5; }
    static int CardCols(int width) { return Narrow(width) ? 2 : 3; }
    static int TileH() { return Scale(150); }
    static int CardMinH() { return Scale(330); }  // title + 7 process rows with meters

    int MinHeight(int width) const override {
        const int gap = Scale(14);
        const int tileRows = (5 + TileCols(width) - 1) / TileCols(width);
        const int cardRows = (6 + CardCols(width) - 1) / CardCols(width);
        return tileRows * TileH() + (tileRows - 1) * gap + gap + cardRows * CardMinH() + (cardRows - 1) * gap;
    }

    void Layout(RECT a) override {
        const int gap = Scale(14), width = a.right - a.left;
        const int tileCols = TileCols(width), cardCols = CardCols(width);
        const int tileW = (width - (tileCols - 1) * gap) / tileCols;
        Tile* tiles[] = {&cpu_, &mem_, &gpu_, &disk_, &net_};
        for (int i = 0; i < 5; ++i) {
            const int col = i % tileCols, row = i / tileCols;
            const int x = a.left + col * (tileW + gap);
            Place(tiles[i]->Handle(), x, a.top + row * (TileH() + gap), col == tileCols - 1 ? a.right - x : tileW, TileH());
        }
        const int tileRows = (5 + tileCols - 1) / tileCols;
        const int top = a.top + tileRows * (TileH() + gap);
        const int cardRows = (6 + cardCols - 1) / cardCols;
        const int cardW = (width - (cardCols - 1) * gap) / cardCols;
        const int cardH = std::max(CardMinH(), (int(a.bottom) - top - (cardRows - 1) * gap) / cardRows);
        ListCard* cards[] = {&topCpu_, &topMem_, &system_, &storage_, &network_, &activity_};
        for (int i = 0; i < 6; ++i) {
            const int col = i % cardCols, row = i / cardCols;
            const int x = a.left + col * (cardW + gap);
            const int y = top + row * (cardH + gap);
            Place(cards[i]->Handle(), x, y, col == cardCols - 1 ? a.right - x : cardW, cardH);
        }
    }

    void Show(bool v) override {
        ShowAll({cpu_.Handle(), mem_.Handle(), gpu_.Handle(), disk_.Handle(), net_.Handle(), topCpu_.Handle(),
                 topMem_.Handle(), system_.Handle(), storage_.Handle(), network_.Handle(), activity_.Handle()},
                v);
    }

    void OnActivate() override {
        ticks_ = 0;  // refresh the slow cards right away
        RefreshCards();
    }

    // Tiles keep their history even while hidden; the cards only refresh when visible.
    void OnTick(bool visible) override {
        PushTiles();
        if (visible) RefreshCards();
        ++ticks_;
    }

private:
    void PushTiles() {
        const SystemSnapshot& s = Sys();
        cpu_.Push(s.cpu);
        cpu_.SetText(L"CPU", Format(L"%.0f%%", s.cpu), Format(L"%.2f GHz\t%zu threads", s.avgMhz / 1000.0, s.cores.size()));
        const uint64_t used = s.memTotal - s.memAvail;
        const double memPct = s.memTotal ? 100.0 * double(used) / double(s.memTotal) : 0;
        mem_.Push(memPct);
        mem_.SetText(L"Memory", Format(L"%.0f%%", memPct), FormatBytes(used) + L" of " + FormatBytes(s.memTotal));
        gpu_.Push(s.gpu);
        gpu_.SetText(L"GPU", s.gpuAvailable ? Format(L"%.0f%%", s.gpu) : L"—",
                     Format(L"%s dedicated in use", FormatBytes(s.gpuDedicated).c_str()));
        disk_.Push(s.diskActive);
        disk_.SetText(L"Disk", Format(L"%.0f%%", s.diskActive),
                      L"R " + FormatBytes(uint64_t(s.diskRead)) + L"/s\tW " + FormatBytes(uint64_t(s.diskWrite)) + L"/s");
        const double net = s.netIn + s.netOut;
        net_.Push(net);
        net_.SetText(L"Network", FormatBytes(uint64_t(net)) + L"/s",
                     L"↓ " + FormatBytes(uint64_t(s.netIn)) + L"/s\t↑ " + FormatBytes(uint64_t(s.netOut)) + L"/s");
    }

    void RefreshCards() {
        const SystemSnapshot& s = Sys();
        const auto& procs = g_procs.Processes();

        // Top processes: bars are relative to the busiest one so small values stay visible.
        std::vector<const ProcInfo*> sorted;
        for (const ProcInfo& p : procs) sorted.push_back(&p);
        const size_t keep = std::min<size_t>(sorted.size(), 7);
        std::partial_sort(sorted.begin(), sorted.begin() + keep, sorted.end(),
                          [](const ProcInfo* a, const ProcInfo* b) { return a->cpu > b->cpu; });
        std::vector<ListCard::Line> lines;
        const double topCpu = keep ? std::max(sorted[0]->cpu, 1.0) : 1.0;
        for (size_t i = 0; i < keep; ++i)
            lines.push_back({sorted[i]->name, Format(L"%.1f%%", sorted[i]->cpu), sorted[i]->cpu / topCpu, g_pal.cpu,
                             ProcessIcon(sorted[i]->pid)});
        topCpu_.SetLines(std::move(lines), Format(L"%.0f%% total", s.cpu));

        std::partial_sort(sorted.begin(), sorted.begin() + keep, sorted.end(),
                          [](const ProcInfo* a, const ProcInfo* b) { return a->privateWs > b->privateWs; });
        lines.clear();
        const double topMem = keep ? std::max<double>(double(sorted[0]->privateWs), 1.0) : 1.0;
        for (size_t i = 0; i < keep; ++i)
            lines.push_back({sorted[i]->name, FormatBytes(sorted[i]->privateWs), double(sorted[i]->privateWs) / topMem,
                             g_pal.mem, ProcessIcon(sorted[i]->pid)});
        topMem_.SetLines(std::move(lines), FormatBytes(s.memTotal - s.memAvail) + L" in use");

        // Slow-changing cards: every 10 ticks (and on activation).
        if (ticks_ % 10 == 0) {
            if (windows_.empty()) {
                windows_ = WindowsVersion();
                computer_ = ComputerName();
                user_ = UserName();
            }
            plan_ = ActivePowerPlanName();
            volumes_ = Volumes();
            services_ = CountServices();
            users_ = CountSignedInUsers();
        }
        if (ticks_ % 3 == 0) tcp_ = CountTcp();

        std::vector<ListCard::Line> sys = {
            {L"Device", computer_},
            {L"Windows", windows_},
            {L"Processor", CpuBrandName()},
        };
        // Every GPU (dedicated first), e.g. an RTX laptop GPU plus the CPU's integrated graphics.
        for (const auto& g : s.gpus)
            sys.push_back({g.integrated ? L"Integrated GPU" : L"Dedicated GPU",
                           g.integrated ? g.name : g.name + L" · " + FormatBytes(g.dedicatedVram)});
        if (s.gpus.empty()) sys.push_back({L"Graphics", L"—"});
        sys.push_back({L"Memory", FormatBytes(s.memTotal) + L" installed"});
        sys.push_back({L"Up time", FormatDuration(GetTickCount64() / 1000)});
        sys.push_back({L"Power", PowerText() + (plan_.empty() ? L"" : L"  ·  " + plan_)});
        sys.push_back({L"Signed in as", user_ + (IsElevated() ? L"  ·  administrator" : L"")});
        system_.SetLines(std::move(sys));

        lines.clear();
        uint64_t totalAll = 0, freeAll = 0;
        for (const Volume& v : volumes_) {
            totalAll += v.total;
            freeAll += v.free;
            const double usedFrac = 1.0 - double(v.free) / double(v.total);
            const bool low = double(v.free) / double(v.total) < 0.1;
            lines.push_back({v.root + (v.label.empty() ? L"" : L"  " + v.label),
                             FormatBytes(v.free) + L" free of " + FormatBytes(v.total), usedFrac,
                             low ? g_pal.warn : g_pal.disk});
        }
        storage_.SetLines(std::move(lines), FormatBytes(freeAll) + L" free in total");

        network_.SetLines({
            {L"Throughput", FormatBytes(uint64_t(s.netIn + s.netOut)) + L"/s"},
            {L"Active adapters", std::to_wstring(s.netAdapters)},
            {L"Established TCP connections", FormatCount(tcp_.established)},
            {L"Listening TCP ports", FormatCount(tcp_.listening)},
            {L"Listening on all interfaces", FormatCount(tcp_.exposed), -1, tcp_.exposed ? g_pal.warn : CLR_NONE},
        });

        const std::wstring rec = g_recorder.Recording()
                                     ? L"Recording  ·  " + FormatCount(g_recorder.Written()) + L" written"
                                     : FormatCount(g_recorder.Size()) + L" samples buffered";
        activity_.SetLines({
            {L"Processes", FormatCount(procs.size())},
            {L"Threads", FormatCount(g_procs.ThreadCount())},
            {L"Handles", FormatCount(g_procs.HandleCount())},
            {L"Services running", Format(L"%s of %s", FormatCount(services_.running).c_str(), FormatCount(services_.total).c_str())},
            {L"Signed-in users", FormatCount(users_)},
            {L"Flight recorder", rec, -1, g_recorder.Recording() ? g_pal.bad : CLR_NONE},
        });
    }

    Tile cpu_, mem_, gpu_, disk_, net_;
    ListCard topCpu_, topMem_, system_, storage_, network_, activity_;
    std::wstring windows_, computer_, user_, plan_;
    std::vector<Volume> volumes_;
    NetCounts tcp_;
    ServiceCounts services_;
    size_t users_ = 0;
    unsigned ticks_ = 0;
};

}  // namespace

std::unique_ptr<Page> MakeSummaryPage() { return std::make_unique<SummaryPage>(); }

}  // namespace mtm
