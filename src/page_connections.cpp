#include <winsock2.h>
#include <ws2tcpip.h>

#include "graph.h"
#include "icons.h"
#include "net.h"
#include "page.h"
#include "processes.h"
#include "table.h"
#include "theme.h"

#include <iphlpapi.h>

#include <cstddef>
#include <cstring>
#include <thread>

namespace mtm {
namespace {

constexpr DWORD kMaxTableBytes = 8u << 20;  // safety cap, same order as Windows' own tools

struct Conn {
    const wchar_t* proto;
    std::wstring local, remote, state, exposure;
    USHORT localPort = 0, remotePort = 0;
    DWORD pid = 0;
    bool listening = false;
    bool established = false;
};

// Size-probe then fetch, retrying while the table grows between calls.
template <typename Call>
bool FetchTable(std::vector<BYTE>& buf, Call call, bool& truncated) {
    DWORD size = 0;
    DWORD r = call(nullptr, &size);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (r != ERROR_INSUFFICIENT_BUFFER && r != NO_ERROR) return false;
        if (size == 0) return false;
        if (size > kMaxTableBytes) {
            truncated = true;
            return false;
        }
        buf.resize(size);
        DWORD got = size;
        r = call(buf.data(), &got);
        if (r == NO_ERROR) return true;
        size = got;
    }
    return false;
}

std::wstring Ip4(DWORD addr) {
    IN_ADDR in{};
    in.S_un.S_addr = addr;
    wchar_t buf[INET_ADDRSTRLEN] = {};
    return InetNtopW(AF_INET, &in, buf, ARRAYSIZE(buf)) ? buf : L"?";
}

std::wstring Ip6(const UCHAR* addr) {
    IN6_ADDR in{};
    memcpy(in.u.Byte, addr, 16);
    wchar_t buf[INET6_ADDRSTRLEN] = {};
    return InetNtopW(AF_INET6, &in, buf, ARRAYSIZE(buf)) ? buf : L"?";
}

const wchar_t* TcpState(DWORD s) {
    switch (s) {
    case MIB_TCP_STATE_CLOSED: return L"Closed";
    case MIB_TCP_STATE_LISTEN: return L"Listening";
    case MIB_TCP_STATE_SYN_SENT: return L"SYN sent";
    case MIB_TCP_STATE_SYN_RCVD: return L"SYN received";
    case MIB_TCP_STATE_ESTAB: return L"Established";
    case MIB_TCP_STATE_FIN_WAIT1: return L"FIN wait 1";
    case MIB_TCP_STATE_FIN_WAIT2: return L"FIN wait 2";
    case MIB_TCP_STATE_CLOSE_WAIT: return L"Close wait";
    case MIB_TCP_STATE_CLOSING: return L"Closing";
    case MIB_TCP_STATE_LAST_ACK: return L"Last ACK";
    case MIB_TCP_STATE_TIME_WAIT: return L"Time wait";
    case MIB_TCP_STATE_DELETE_TCB: return L"Delete TCB";
    default: return L"?";
    }
}

std::wstring Exposure(const std::wstring& local) {
    if (local == L"0.0.0.0" || local == L"::") return L"All interfaces";
    if (local.rfind(L"127.", 0) == 0 || local == L"::1") return L"Loopback only";
    return L"One interface";
}

void Collect(std::vector<Conn>& out, bool& truncated) {
    std::vector<BYTE> buf;
    if (FetchTable(buf, [](void* p, DWORD* s) { return GetExtendedTcpTable(p, s, FALSE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0); }, truncated)) {
        const auto* t = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buf.data());
        const size_t max = (buf.size() - offsetof(MIB_TCPTABLE_OWNER_PID, table)) / sizeof(MIB_TCPROW_OWNER_PID);
        for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
            const auto& r = t->table[i];
            Conn c{L"TCP"};
            c.local = Ip4(r.dwLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(r.dwLocalPort));
            c.listening = r.dwState == MIB_TCP_STATE_LISTEN;
            c.established = r.dwState == MIB_TCP_STATE_ESTAB;
            if (!c.listening) {
                c.remote = Ip4(r.dwRemoteAddr);
                c.remotePort = ntohs(static_cast<u_short>(r.dwRemotePort));
            }
            c.state = TcpState(r.dwState);
            c.pid = r.dwOwningPid;
            out.push_back(std::move(c));
        }
    }
    if (FetchTable(buf, [](void* p, DWORD* s) { return GetExtendedTcpTable(p, s, FALSE, AF_INET6, TCP_TABLE_OWNER_PID_ALL, 0); }, truncated)) {
        const auto* t = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buf.data());
        const size_t max = (buf.size() - offsetof(MIB_TCP6TABLE_OWNER_PID, table)) / sizeof(MIB_TCP6ROW_OWNER_PID);
        for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
            const auto& r = t->table[i];
            Conn c{L"TCPv6"};
            c.local = Ip6(r.ucLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(r.dwLocalPort));
            c.listening = r.dwState == MIB_TCP_STATE_LISTEN;
            c.established = r.dwState == MIB_TCP_STATE_ESTAB;
            if (!c.listening) {
                c.remote = Ip6(r.ucRemoteAddr);
                c.remotePort = ntohs(static_cast<u_short>(r.dwRemotePort));
            }
            c.state = TcpState(r.dwState);
            c.pid = r.dwOwningPid;
            out.push_back(std::move(c));
        }
    }
    if (FetchTable(buf, [](void* p, DWORD* s) { return GetExtendedUdpTable(p, s, FALSE, AF_INET, UDP_TABLE_OWNER_PID, 0); }, truncated)) {
        const auto* t = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(buf.data());
        const size_t max = (buf.size() - offsetof(MIB_UDPTABLE_OWNER_PID, table)) / sizeof(MIB_UDPROW_OWNER_PID);
        for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
            const auto& r = t->table[i];
            Conn c{L"UDP"};
            c.local = Ip4(r.dwLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(r.dwLocalPort));
            c.listening = true;
            c.state = L"Bound";
            c.pid = r.dwOwningPid;
            out.push_back(std::move(c));
        }
    }
    if (FetchTable(buf, [](void* p, DWORD* s) { return GetExtendedUdpTable(p, s, FALSE, AF_INET6, UDP_TABLE_OWNER_PID, 0); }, truncated)) {
        const auto* t = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(buf.data());
        const size_t max = (buf.size() - offsetof(MIB_UDP6TABLE_OWNER_PID, table)) / sizeof(MIB_UDP6ROW_OWNER_PID);
        for (size_t i = 0; i < std::min<size_t>(t->dwNumEntries, max); ++i) {
            const auto& r = t->table[i];
            Conn c{L"UDPv6"};
            c.local = Ip6(r.ucLocalAddr);
            c.localPort = ntohs(static_cast<u_short>(r.dwLocalPort));
            c.listening = true;
            c.state = L"Bound";
            c.pid = r.dwOwningPid;
            out.push_back(std::move(c));
        }
    }
    for (Conn& c : out)
        if (c.listening) c.exposure = Exposure(c.local);
}

// What a connection of a given speed / latency is good for, for the results card.
std::vector<ListCard::Line> Suitability(const SpeedResult& r) {
    auto check = [](bool ok) { return ok ? L"\x2713  Yes" : L"\x2717  Not reliably"; };
    auto color = [](bool ok) { return ok ? g_pal.good : g_pal.warn; };
    const bool hd = r.downMbps >= 5, uhd = r.downMbps >= 25, calls = r.downMbps >= 4 && r.upMbps >= 3 && r.pingMs < 150;
    const bool gaming = r.pingMs < 50 && r.jitterMs < 15, bigFiles = r.downMbps >= 100;
    return {
        {L"HD video streaming (5 Mbps)", check(hd), -1, color(hd)},
        {L"4K streaming (25 Mbps)", check(uhd), -1, color(uhd)},
        {L"Video calls (4 down / 3 up, < 150 ms)", check(calls), -1, color(calls)},
        {L"Online gaming (< 50 ms, low jitter)", check(gaming), -1, color(gaming)},
        {L"Big downloads (100+ Mbps)", check(bigFiles), -1, color(bigFiles)},
    };
}

COLORREF LatencyColor(double ms) { return ms < 0 ? g_pal.bad : ms < 30 ? g_pal.good : ms < 80 ? g_pal.warn : g_pal.bad; }

class ConnectionsPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Connections"; }

    void Create() override {
        tabs_[0] = CreateButton(L"\xE774  Connections", true);
        tabs_[1] = CreateButton(L"\xEC4A  Speed test");
        tabs_[2] = CreateButton(L"\xE968  DNS");

        // --- Connections view
        filter_ = CreateCombo({L"All connections", L"Listening / exposed ports", L"Established", L"TCP only", L"UDP only"});
        summary_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        table_.Create({{L"Process", 180},
                       {L"PID", 65, true},
                       {L"Protocol", 70},
                       {L"Local address", 170},
                       {L"Local port", 80, true},
                       {L"Remote address", 170},
                       {L"Remote port", 85, true},
                       {L"State", 100},
                       {L"Exposure", 120}},
                      4, false);

        // --- Speed test view
        speedStart_ = CreateButton(L"\xE768  Start speed test", true);
        speedStop_ = CreateButton(L"\xE71A  Stop");
        EnableWindow(speedStop_, FALSE);
        speedBar_.Create();
        speedStatus_ = CreateLabel(L"Tests against Cloudflare's nearest server. Uses roughly 100–500 MB of data on fast connections.",
                                   SS_CENTERIMAGE | SS_ENDELLIPSIS);
        speedCards_[0].Create(0xE896, L"Download", Graph::Tint::Network, nullptr);
        speedCards_[1].Create(0xE898, L"Upload", Graph::Tint::Gpu, nullptr);
        speedCards_[2].Create(0xEC27, L"Latency", Graph::Tint::Cpu, nullptr);
        speedCards_[3].Create(0xE909, L"Server", Graph::Tint::Memory, nullptr);
        downChart_.Create(Graph::Tint::Network);
        upChart_.Create(Graph::Tint::Gpu);
        speedDetails_.Create(L"Details");
        ResetSpeedView();

        // --- DNS view
        adapter_ = CreateCombo({});
        dnsCurrent_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        grid_.Create([this](size_t) { UpdateApplyButton(); });
        dnsApply_ = CreateButton(L"\xE73E  Use selected DNS", true);
        dnsTest_ = CreateButton(L"\xEC4A  Test latency");
        dnsRefresh_ = CreateButton(L"\xE72C  Refresh");
        dnsStatus_ = CreateLabel(L"Hover over a provider to see what it does and why you might pick it.", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        std::vector<OptionGrid::Item> items;
        for (const DnsProvider& p : DnsProviders()) {
            OptionGrid::Item it;
            it.title = p.name;
            it.detail = p.automatic ? L"From your router / ISP" : std::wstring(p.v4[0]) + L"  ·  " + p.v4[1];
            it.tags = p.tags;
            it.markText = L"In use";
            it.tipTitle = p.name;
            it.tip.push_back({L"What it does", p.what});
            it.tip.push_back({L"Why choose it", p.why});
            if (!p.automatic) {
                std::wstring addrs = std::wstring(L"IPv4  ") + p.v4[0] + L", " + p.v4[1];
                if (p.v6[0]) addrs += std::wstring(L"\nIPv6  ") + p.v6[0] + L", " + p.v6[1];
                it.tip.push_back({L"Addresses", addrs});
            }
            items.push_back(std::move(it));
        }
        grid_.SetItems(std::move(items));
        view_ = static_cast<int>(std::min<DWORD>(ReadSetting(L"ConnView", 0), 2));
        for (int i = 0; i < 3; ++i) SetButtonPrimary(tabs_[i], i == view_);
    }

    // The speed test view has cards and two charts; the others manage their own scrolling.
    int MinHeight(int) const override { return view_ == 1 ? Scale(680) : Scale(440); }

    void Layout(RECT a) override {
        area_ = a;
        const int h = Scale(34), gap = Scale(12);
        int x = a.left;
        for (HWND t : tabs_) {
            Place(t, x, a.top, Scale(150), h);
            x += Scale(150) + Scale(6);
        }
        const int top = a.top + h + gap;
        const int w = a.right - a.left;

        // Connections
        Place(filter_, a.left, top, Scale(240), Scale(300));
        Place(summary_, a.left + Scale(256), top, w - Scale(256), h);
        Place(table_.Handle(), a.left, top + h + gap, w, a.bottom - top - h - gap);

        // Speed test
        Place(speedStart_, a.left, top, Scale(180), h);
        Place(speedStop_, a.left + Scale(188), top, Scale(100), h);
        Place(speedBar_.Handle(), a.left + Scale(304), top + (h - Scale(8)) / 2, Scale(180), Scale(8));
        Place(speedStatus_, a.left + Scale(498), top, w - Scale(498), h);
        const int cardsTop = top + h + gap, cardH = Scale(140);
        const int cw = (w - gap * 3) / 4;
        for (int i = 0; i < 4; ++i) Place(speedCards_[i].Handle(), a.left + i * (cw + gap), cardsTop, cw, cardH);
        const int lower = cardsTop + cardH + gap, lowerH = a.bottom - lower;
        const int chartW = (w - gap) * 3 / 5;
        Place(downChart_.Handle(), a.left, lower, chartW, (lowerH - gap) / 2);
        Place(upChart_.Handle(), a.left, lower + (lowerH - gap) / 2 + gap, chartW, lowerH - (lowerH - gap) / 2 - gap);
        Place(speedDetails_.Handle(), a.left + chartW + gap, lower, w - chartW - gap, lowerH);

        // DNS
        Place(adapter_, a.left, top, Scale(300), Scale(300));
        Place(dnsCurrent_, a.left + Scale(316), top, w - Scale(316), h);
        const int bottomRow = a.bottom - h;
        Place(grid_.Handle(), a.left, top + h + gap, w, bottomRow - gap - (top + h + gap));
        Place(dnsApply_, a.left, bottomRow, Scale(190), h);
        Place(dnsTest_, a.left + Scale(198), bottomRow, Scale(140), h);
        Place(dnsRefresh_, a.left + Scale(346), bottomRow, Scale(110), h);
        Place(dnsStatus_, a.left + Scale(470), bottomRow, w - Scale(470), h);
    }

    void Show(bool v) override {
        visible_ = v;
        for (HWND t : tabs_) ShowWindow(t, v ? SW_SHOW : SW_HIDE);
        const std::vector<HWND> views[3] = {
            {filter_, summary_, table_.Handle()},
            {speedStart_, speedStop_, speedBar_.Handle(), speedStatus_, speedCards_[0].Handle(), speedCards_[1].Handle(),
             speedCards_[2].Handle(), speedCards_[3].Handle(), downChart_.Handle(), upChart_.Handle(), speedDetails_.Handle()},
            {adapter_, dnsCurrent_, grid_.Handle(), dnsApply_, dnsTest_, dnsRefresh_, dnsStatus_},
        };
        for (int i = 0; i < 3; ++i)
            for (HWND h : views[i]) ShowWindow(h, v && i == view_ ? SW_SHOW : SW_HIDE);
        if (!v || view_ != 2) HideRichTip();
    }

    void OnActivate() override {
        if (view_ == 0) Refresh();
        if (view_ == 2) RefreshAdapters();
    }

    void OnTick(bool visible) override {
        if (visible && view_ == 0) Refresh();
    }

    bool OnCommand(HWND control, WORD code) override {
        if (control == filter_ && code == CBN_SELCHANGE) {
            Refresh();
            return true;
        }
        if (control == adapter_ && code == CBN_SELCHANGE) {
            ShowAdapterDns();
            return true;
        }
        if (code != BN_CLICKED) return false;
        for (int i = 0; i < 3; ++i)
            if (control == tabs_[i]) {
                SetView(i);
                return true;
            }
        if (control == speedStart_) StartSpeedTest();
        else if (control == speedStop_) speedWorker_.request_stop();
        else if (control == dnsApply_) ApplySelectedDns();
        else if (control == dnsTest_) TestDnsLatency();
        else if (control == dnsRefresh_) RefreshAdapters();
        else return false;
        return true;
    }

    bool OnNotify(NMHDR* nm, LRESULT& result) override { return table_.HandleNotify(nm, result); }

    bool OnContextMenu(HWND control, POINT pt) override {
        if (control != table_.Handle()) return false;
        const Row* selected = table_.SelectedRow();
        if (!selected) return true;
        // Copy: the menu's message loop lets timer ticks replace the rows.
        const Row row = *selected;
        if (pt.x == -1 && pt.y == -1) GetCursorPos(&pt);
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, L"Copy row");
        AppendMenuW(menu, MF_STRING | (row.cells[5].empty() ? MF_GRAYED : 0), 2, L"Copy remote address");
        AppendMenuW(menu, MF_STRING, 3, L"Look up remote address online");
        const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
        DestroyMenu(menu);
        if (cmd == 1) {
            std::wstring line;
            for (const auto& c : row.cells) line += (line.empty() ? L"" : L"\t") + c;
            CopyToClipboard(line);
        } else if (cmd == 2) {
            CopyToClipboard(row.cells[5]);
        } else if (cmd == 3 && !row.cells[5].empty()) {
            const std::wstring url = L"https://ipinfo.io/" + row.cells[5];
            ShowInExplorer(url, false);
        }
        return true;
    }

private:
    void SetView(int v) {
        view_ = v;
        WriteSetting(L"ConnView", static_cast<DWORD>(v));
        SendMessageW(g_main, WM_SIZE, 0, 0);  // views need different heights: re-lay the page
        for (int i = 0; i < 3; ++i) SetButtonPrimary(tabs_[i], i == v);
        Show(visible_);
        OnActivate();
    }

    // ------------------------------------------------------------ connections

    void Refresh() {
        std::vector<Conn> conns;
        bool truncated = false;
        Collect(conns, truncated);
        const int filter = static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0));
        size_t listening = 0, exposed = 0, established = 0;
        std::vector<Row> rows;
        rows.reserve(conns.size());
        for (const Conn& c : conns) {
            if (c.listening) ++listening;
            if (c.listening && c.exposure == L"All interfaces") ++exposed;
            if (c.established) ++established;
            const bool tcp = c.proto[0] == L'T';
            if ((filter == 1 && !c.listening) || (filter == 2 && !c.established) || (filter == 3 && !tcp) ||
                (filter == 4 && tcp))
                continue;
            Row r;
            std::wstring name = g_procs.NameOf(c.pid);
            if (c.listening && c.exposure == L"All interfaces") r.accent = g_pal.warn;
            r.icon = ProcessIcon(c.pid);
            r.Add(name.empty() ? L"(exited)" : name)
                .Add(std::to_wstring(c.pid), c.pid)
                .Add(c.proto)
                .Add(c.local)
                .Add(std::to_wstring(c.localPort), c.localPort)
                .Add(c.remote)
                .Add(c.remotePort ? std::to_wstring(c.remotePort) : L"", c.remotePort)
                .Add(c.state)
                .Add(c.exposure);
            r.tag = std::hash<std::wstring>{}(std::wstring(c.proto) + c.local + L":" + std::to_wstring(c.localPort) +
                                              L">" + c.remote + L":" + std::to_wstring(c.remotePort) +
                                              L"#" + std::to_wstring(c.pid));
            rows.push_back(std::move(r));
        }
        table_.SetRows(std::move(rows));
        SetText(summary_, Format(L"%zu connections  ·  %zu established  ·  %zu listening  ·  %zu on all interfaces (highlighted)%s",
                                 conns.size(), established, listening, exposed,
                                 truncated ? L" · list truncated (over 8 MB)" : L""));
    }

    // ------------------------------------------------------------ speed test

    void ResetSpeedView() {
        speedCards_[0].SetText(L"", L"Mbps", L"Press Start to measure");
        speedCards_[1].SetText(L"", L"Mbps", L"");
        speedCards_[2].SetText(L"", L"Idle ping · jitter", L"");
        speedCards_[3].SetText(L"", L"", L"Cloudflare speed test");
        downChart_.SetSeries({}, 0.125, L"Download speed · Mbps", L"Mbps", L"");
        upChart_.SetSeries({}, 0.125, L"Upload speed · Mbps", L"Mbps", L"");
        speedDetails_.SetLines({{L"Run a test to see latency, jitter, data used and what your connection is good for.", L""}});
    }

    void StartSpeedTest() {
        if (speedRunning_) return;
        speedRunning_ = true;
        EnableWindow(speedStart_, FALSE);
        EnableWindow(speedStop_, TRUE);
        ResetSpeedView();
        down_.clear();
        up_.clear();
        speedBar_.SetFraction(0);
        speedWorker_ = std::jthread([this](std::stop_token st) {
            SpeedResult r = RunSpeedTest(st, [this](const SpeedSample& s) {
                PostToUi([this, s] { OnSpeedSample(s); });
            });
            PostToUi([this, r = std::move(r)] { OnSpeedDone(r); });
        });
    }

    void OnSpeedSample(const SpeedSample& s) {
        speedBar_.SetFraction(s.fraction);
        SetText(speedStatus_, s.text);
        if (s.phase == SpeedSample::Ping) {
            speedCards_[2].SetText(Format(L"%.0f ms", s.pingMs), L"Measuring…", L"");
        } else if (s.phase == SpeedSample::Download) {
            down_.push_back(s.mbps);
            downChart_.SetSeries(down_, 0.125, L"Download speed · Mbps", L"Mbps", L"6 parallel connections");
            speedCards_[0].SetText(Format(L"%.1f", s.mbps), L"Mbps · measuring…", L"");
        } else if (s.phase == SpeedSample::Upload) {
            up_.push_back(s.mbps);
            upChart_.SetSeries(up_, 0.125, L"Upload speed · Mbps", L"Mbps", L"4 parallel connections");
            speedCards_[1].SetText(Format(L"%.1f", s.mbps), L"Mbps · measuring…", L"");
        }
    }

    void OnSpeedDone(const SpeedResult& r) {
        speedRunning_ = false;
        EnableWindow(speedStart_, TRUE);
        EnableWindow(speedStop_, FALSE);
        if (!r.ok) {
            speedBar_.SetFraction(0);
            SetText(speedStatus_, r.error);
            return;
        }
        speedBar_.SetFraction(1);
        SYSTEMTIME now;
        GetLocalTime(&now);
        SetText(speedStatus_, Format(L"Finished at %02u:%02u — %.1f Mbps down, %.1f Mbps up, %.0f ms ping.", now.wHour,
                                     now.wMinute, r.downMbps, r.upMbps, r.pingMs));
        speedCards_[0].SetText(Format(L"%.1f", r.downMbps), L"Mbps download", FormatBytes(r.downBytes) + L" received");
        speedCards_[1].SetText(Format(L"%.1f", r.upMbps), L"Mbps upload", FormatBytes(r.upBytes) + L" sent");
        speedCards_[2].SetText(Format(L"%.0f ms", r.pingMs), Format(L"Jitter %.1f ms", r.jitterMs),
                               Format(L"%.0f ms under load", r.loadedPingMs));
        speedCards_[3].SetText(r.server.empty() ? L"Cloudflare" : r.server, r.isp, r.ip.empty() ? L"" : L"Your IP " + r.ip);

        std::vector<ListCard::Line> lines = {
            {L"Download", Format(L"%.1f Mbps  (%.1f MB/s)", r.downMbps, r.downMbps / 8), -1},
            {L"Upload", Format(L"%.1f Mbps  (%.1f MB/s)", r.upMbps, r.upMbps / 8), -1},
            {L"Idle latency (median of 20)", Format(L"%.0f ms", r.pingMs), -1, LatencyColor(r.pingMs)},
            {L"Jitter", Format(L"%.1f ms", r.jitterMs), -1, r.jitterMs < 15 ? g_pal.good : g_pal.warn},
            {L"Latency while downloading", Format(L"%.0f ms  (+%.0f)", r.loadedPingMs, std::max(0.0, r.loadedPingMs - r.pingMs)),
             -1, r.loadedPingMs - r.pingMs < 50 ? g_pal.good : g_pal.warn},
            {L"Data used", FormatBytes(r.downBytes + r.upBytes), -1},
        };
        for (auto& l : Suitability(r)) lines.push_back(std::move(l));
        speedDetails_.SetLines(std::move(lines), r.isp);
    }

    // ------------------------------------------------------------ DNS

    void RefreshAdapters() {
        const std::wstring prevGuid = SelectedAdapter() ? SelectedAdapter()->guid : L"";
        adapters_ = ListAdapters();
        SendMessageW(adapter_, CB_RESETCONTENT, 0, 0);
        int pick = 0;
        for (size_t i = 0; i < adapters_.size(); ++i) {
            const NetAdapter& a = adapters_[i];
            const std::wstring text = a.name + (a.up ? (a.hasGateway ? L"  (connected)" : L"") : L"  (disconnected)");
            SendMessageW(adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (a.guid == prevGuid) pick = static_cast<int>(i);
        }
        if (adapters_.empty()) SendMessageW(adapter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"No Ethernet or Wi-Fi adapter"));
        SendMessageW(adapter_, CB_SETCURSEL, pick, 0);
        ShowAdapterDns();
    }

    const NetAdapter* SelectedAdapter() const {
        const int sel = static_cast<int>(SendMessageW(adapter_, CB_GETCURSEL, 0, 0));
        return sel >= 0 && sel < static_cast<int>(adapters_.size()) ? &adapters_[size_t(sel)] : nullptr;
    }

    // Which provider the adapter currently uses (by its IPv4 servers), or SIZE_MAX.
    size_t ProviderInUse(const NetAdapter& a) const {
        const auto& list = DnsProviders();
        if (!a.staticDns) return 0;  // "Automatic"
        for (size_t i = 1; i < list.size(); ++i)
            for (const auto& s : a.dns4)
                if (s == list[i].v4[0] || s == list[i].v4[1]) return i;
        return SIZE_MAX;
    }

    void ShowAdapterDns() {
        const NetAdapter* a = SelectedAdapter();
        const auto& list = DnsProviders();
        const size_t inUse = a ? ProviderInUse(*a) : SIZE_MAX;
        // Move the "In use" chip.
        for (size_t i = 0; i < list.size(); ++i) MarkInUse(i, i == inUse);
        if (!a) {
            SetText(dnsCurrent_, L"");
            EnableWindow(dnsApply_, FALSE);
            return;
        }
        std::wstring servers;
        for (const auto& s : a->dns4) servers += (servers.empty() ? L"" : L", ") + s;
        for (const auto& s : a->dns6) servers += (servers.empty() ? L"" : L", ") + s;
        if (servers.empty()) servers = L"none";
        const std::wstring who = inUse == 0 ? L"automatic, from your router / ISP"
                               : inUse != SIZE_MAX ? std::wstring(L"set by hand: ") + list[inUse].name
                                                   : L"set by hand: custom servers";
        SetText(dnsCurrent_, L"Currently using " + servers + L"  (" + who + L")");
        if (grid_.Selected() == SIZE_MAX && inUse != SIZE_MAX) grid_.SetSelected(inUse);
        UpdateApplyButton();
    }

    void MarkInUse(size_t index, bool inUse) { grid_.SetMarked(index, inUse); }

    void UpdateApplyButton() {
        const NetAdapter* a = SelectedAdapter();
        const size_t sel = grid_.Selected();
        EnableWindow(dnsApply_, a && sel != SIZE_MAX && !dnsBusy_);
        if (sel < DnsProviders().size())
            SetText(dnsStatus_, IsElevated() || sel == SIZE_MAX
                                    ? std::wstring(L"Selected: ") + DnsProviders()[sel].name
                                    : std::wstring(L"Selected: ") + DnsProviders()[sel].name +
                                          L" — Windows will ask for permission (administrator) to change DNS.");
    }

    void ApplySelectedDns() {
        const NetAdapter* a = SelectedAdapter();
        const size_t sel = grid_.Selected();
        if (!a || sel >= DnsProviders().size() || dnsBusy_) return;
        const DnsProvider& p = DnsProviders()[sel];
        std::vector<std::wstring> v4, v6;
        if (!p.automatic) {
            v4 = {p.v4[0], p.v4[1]};
            if (p.v6[0]) v6 = {p.v6[0], p.v6[1]};
        }
        const std::wstring guid = a->guid, adapterName = a->name, name = p.name;
        SetDnsBusy(true, L"Applying " + name + L" to " + adapterName + L"…");
        dnsWorker_ = std::jthread([this, guid, v4, v6, name, adapterName](std::stop_token) {
            const DWORD r = ApplyDnsElevated(guid, v4, v6);
            PostToUi([this, r, name, adapterName] {
                SetDnsBusy(false, L"");
                RefreshAdapters();
                if (r == ERROR_SUCCESS)
                    SetText(dnsStatus_, L"Done — " + adapterName + L" now uses " + name + L". DNS cache flushed.");
                else if (r == ERROR_CANCELLED)
                    SetText(dnsStatus_, L"Not changed — the administrator prompt was cancelled.");
                else
                    SetText(dnsStatus_, L"Couldn't change DNS: " + ErrorText(r));
            });
        });
    }

    void TestDnsLatency() {
        if (dnsBusy_) return;
        SetDnsBusy(true, L"Testing each resolver (3 queries each)…");
        const NetAdapter* a = SelectedAdapter();
        // "Automatic" = the router's server (what DHCP would give), else whatever is in use now.
        const std::wstring current = a && !a->dhcpDns4.empty() ? a->dhcpDns4[0] : a && !a->dns4.empty() ? a->dns4[0] : L"";
        dnsWorker_ = std::jthread([this, current](std::stop_token st) {
            const auto& list = DnsProviders();
            for (size_t i = 0; i < list.size() && !st.stop_requested(); ++i) {
                const std::wstring server = list[i].automatic ? current : list[i].v4[0];
                const double ms = server.empty() ? -1 : DnsLatencyMs(server);
                PostToUi([this, i, ms] {
                    grid_.SetBadge(i, ms < 0 ? L"timeout" : Format(L"%.0f ms", ms), LatencyColor(ms));
                });
            }
            PostToUi([this] {
                SetDnsBusy(false, L"Latency = best of 3 lookups from this PC. Lower is snappier browsing; under ~30 ms is excellent.");
            });
        });
    }

    void SetDnsBusy(bool busy, const std::wstring& text) {
        dnsBusy_ = busy;
        for (HWND h : {dnsTest_, dnsRefresh_, adapter_}) EnableWindow(h, !busy);
        if (!text.empty()) SetText(dnsStatus_, text);
        UpdateApplyButton();
    }

    HWND tabs_[3] = {};
    int view_ = 0;
    bool visible_ = false;
    RECT area_{};

    HWND filter_ = nullptr, summary_ = nullptr;
    Table table_;

    HWND speedStart_ = nullptr, speedStop_ = nullptr, speedStatus_ = nullptr;
    Bar speedBar_;
    ActionCard speedCards_[4];
    Chart downChart_, upChart_;
    ListCard speedDetails_;
    std::vector<double> down_, up_;
    bool speedRunning_ = false;

    HWND adapter_ = nullptr, dnsCurrent_ = nullptr, dnsApply_ = nullptr, dnsTest_ = nullptr, dnsRefresh_ = nullptr,
         dnsStatus_ = nullptr;
    OptionGrid grid_;
    std::vector<NetAdapter> adapters_;
    bool dnsBusy_ = false;

    // Last members: joined before the rest is destroyed.
    std::jthread speedWorker_, dnsWorker_;
};

}  // namespace

std::unique_ptr<Page> MakeConnectionsPage() { return std::make_unique<ConnectionsPage>(); }

}  // namespace mtm
