#include <winsock2.h>
#include <ws2tcpip.h>

#include "net.h"

#include "power.h"

#include <iphlpapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <winhttp.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <functional>
#include <stop_token>
#include <thread>

namespace mtm {

// ================================================================ speed test

namespace {

constexpr wchar_t kSpeedHost[] = L"speed.cloudflare.com";

// RAII for WinHTTP handles.
struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET v) : h(v) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() {
        if (h) WinHttpCloseHandle(h);
    }
    void Reset(HINTERNET v = nullptr) {
        if (h) WinHttpCloseHandle(h);
        h = v;
    }
    explicit operator bool() const { return h != nullptr; }
};

// One HTTPS connection to the speed test host. WinHTTP keeps the TCP/TLS connection
// alive between requests on the same connect handle, so latency probes after the
// first measure a bare round trip.
class Connection {
public:
    Connection() {
        session_.h = WinHttpOpen(L"Kernova/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session_) {  // WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1+
            session_.h = WinHttpOpen(L"Kernova/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (!session_) return;
        WinHttpSetTimeouts(session_.h, 10000, 10000, 15000, 15000);
        connect_.h = WinHttpConnect(session_.h, kSpeedHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    }
    bool Ok() const { return static_cast<bool>(connect_); }

    // Sends a request with an optional body produced in `chunk`-sized pieces.
    // Returns the open request (ready to read) or null.
    HINTERNET Send(const wchar_t* verb, const std::wstring& path, uint64_t bodySize = 0,
                   const std::function<bool(const BYTE*, DWORD)>& onWritten = {}) {
        // speed.cloudflare.com answers 403 to /meta without the referrer its own page sends.
        request_.Reset(WinHttpOpenRequest(connect_.h, verb, path.c_str(), nullptr, L"https://speed.cloudflare.com/",
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!request_) return nullptr;
        const wchar_t* headers = bodySize ? L"Content-Type: application/octet-stream\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
        if (!WinHttpSendRequest(request_.h, headers, bodySize ? DWORD(-1L) : 0, WINHTTP_NO_REQUEST_DATA, 0,
                                static_cast<DWORD>(bodySize), 0))
            return nullptr;
        if (bodySize) {
            static const std::vector<BYTE> junk = [] {
                std::vector<BYTE> v(256 * 1024);
                uint32_t x = 2463534242u;
                for (BYTE& b : v) {
                    x ^= x << 13;
                    x ^= x >> 17;
                    x ^= x << 5;
                    b = static_cast<BYTE>(x);
                }
                return v;
            }();
            uint64_t left = bodySize;
            while (left) {
                const DWORD n = static_cast<DWORD>(std::min<uint64_t>(left, junk.size()));
                DWORD wrote = 0;
                if (!WinHttpWriteData(request_.h, junk.data(), n, &wrote) || wrote == 0) return nullptr;
                left -= wrote;
                if (onWritten && !onWritten(junk.data(), wrote)) return nullptr;
            }
        }
        if (!WinHttpReceiveResponse(request_.h, nullptr)) return nullptr;
        DWORD status = 0, size = sizeof status;
        WinHttpQueryHeaders(request_.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status != 200) {
            SetLastError(ERROR_WINHTTP_INVALID_SERVER_RESPONSE);
            lastStatus_ = status;
            return nullptr;
        }
        return request_.h;
    }

    // Reads the whole body, calling onData for each piece. False on error or when onData says stop.
    bool Read(const std::function<bool(const BYTE*, DWORD)>& onData) {
        std::vector<BYTE> buf(64 * 1024);
        for (;;) {
            DWORD got = 0;
            if (!WinHttpReadData(request_.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) return false;
            if (got == 0) return true;
            if (onData && !onData(buf.data(), got)) return false;
        }
    }

    DWORD LastStatus() const { return lastStatus_; }

private:
    Handle session_, connect_, request_;
    DWORD lastStatus_ = 0;
};

// Round trip of an empty download on a warm connection, in ms (< 0 on failure).
double Probe(Connection& c) {
    Stopwatch sw;
    if (!c.Send(L"GET", L"/__down?bytes=0")) return -1;
    const double ms = sw.Seconds() * 1000.0;
    c.Read({});
    return ms;
}

std::wstring JsonString(const std::string& json, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    size_t p = json.find(needle);
    if (p == std::string::npos) return L"";
    p += needle.size();
    while (p < json.size() && json[p] == ' ') ++p;
    if (p >= json.size() || json[p] != '"') return L"";
    const size_t end = json.find('"', p + 1);
    if (end == std::string::npos) return L"";
    const std::string v = json.substr(p + 1, end - p - 1);
    const int n = MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, v.data(), static_cast<int>(v.size()), w.data(), n);
    return w;
}

double Median(std::vector<double> v) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

// Runs `streams` parallel transfers for `seconds`, reporting throughput as it goes.
// Returns the steady-state rate in Mbps (first second skipped: TCP ramp-up).
double Transfer(bool upload, int streams, double seconds, double fractionFrom, double fractionTo, std::stop_token st,
                const std::function<void(const SpeedSample&)>& onSample, uint64_t& totalBytes,
                std::vector<double>* loadedPings) {
    std::atomic<uint64_t> bytes{0};
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::vector<std::thread> pool;
    for (int i = 0; i < streams; ++i) {
        pool.emplace_back([&] {
            Connection c;
            if (!c.Ok()) {
                ++failures;
                return;
            }
            auto count = [&](const BYTE*, DWORD n) {
                bytes.fetch_add(n, std::memory_order_relaxed);
                return !stop.load(std::memory_order_relaxed);
            };
            while (!stop.load(std::memory_order_relaxed)) {
                bool ok;
                if (upload) ok = c.Send(L"POST", L"/__up", 8u << 20, count) != nullptr && c.Read({});
                else ok = c.Send(L"GET", L"/__down?bytes=25000000") != nullptr && c.Read(count);
                if (!ok && !stop.load()) {
                    if (++failures > streams * 3) return;  // give up on persistent errors
                    Sleep(200);
                }
            }
        });
    }
    // Latency under load, on its own connection.
    std::thread pinger;
    if (loadedPings) {
        pinger = std::thread([&] {
            Connection c;
            if (!c.Ok()) return;
            Probe(c);  // handshake
            while (!stop.load()) {
                const double ms = Probe(c);
                if (ms > 0) loadedPings->push_back(ms);
                for (int i = 0; i < 5 && !stop.load(); ++i) Sleep(50);
            }
        });
    }

    Stopwatch sw;
    uint64_t steadyStart = 0;
    double steadyTime = 0;
    bool steady = false;
    // Sliding window for the live read-out.
    struct Point {
        double t;
        uint64_t b;
    };
    std::vector<Point> window;
    while (sw.Seconds() < seconds && !st.stop_requested()) {
        Sleep(125);
        const double t = sw.Seconds();
        const uint64_t b = bytes.load();
        if (!steady && t >= 1.0) {
            steady = true;
            steadyStart = b;
            steadyTime = t;
        }
        window.push_back({t, b});
        while (window.size() > 2 && window.front().t < t - 0.75) window.erase(window.begin());
        const double dt = t - window.front().t;
        SpeedSample s;
        s.phase = upload ? SpeedSample::Upload : SpeedSample::Download;
        s.mbps = dt > 0 ? double(b - window.front().b) * 8 / 1e6 / dt : 0;
        s.fraction = fractionFrom + (fractionTo - fractionFrom) * std::min(1.0, t / seconds);
        s.text = Format(upload ? L"Uploading… %.1f Mbps" : L"Downloading… %.1f Mbps", s.mbps);
        onSample(s);
        if (failures.load() > streams * 3) break;
    }
    const double end = sw.Seconds();
    const uint64_t endBytes = bytes.load();
    stop = true;
    for (auto& t : pool) t.join();
    if (pinger.joinable()) pinger.join();
    totalBytes = bytes.load();
    if (!steady || end - steadyTime < 0.5) return end > 0 ? double(endBytes) * 8 / 1e6 / end : 0;
    return double(endBytes - steadyStart) * 8 / 1e6 / (end - steadyTime);
}

}  // namespace

SpeedResult RunSpeedTest(std::stop_token st, const std::function<void(const SpeedSample&)>& onSample) {
    SpeedResult r;
    Connection c;
    if (!c.Ok()) {
        r.error = L"Couldn't start WinHTTP: " + ErrorText(GetLastError());
        return r;
    }
    onSample({SpeedSample::Info, 0, 0, 0.01, L"Connecting to speed.cloudflare.com…"});
    // Server / ISP info is nice to have; only a connection failure stops the test.
    std::string meta;
    if (c.Send(L"GET", L"/meta")) {
        c.Read([&](const BYTE* p, DWORD n) {
            meta.append(reinterpret_cast<const char*>(p), n);
            return meta.size() < 64 * 1024;
        });
    } else if (!c.LastStatus()) {
        r.error = L"Couldn't reach speed.cloudflare.com: " + ErrorText(GetLastError());
        return r;
    }
    r.ip = JsonString(meta, "clientIp");
    r.isp = JsonString(meta, "asOrganization");
    // "colo" is the Cloudflare data centre that served us: {"iata":"HKG",...,"city":"Hong Kong"}.
    // (The top-level "city" is the client's own location.)
    const size_t colo = meta.find("\"colo\":{");
    const std::string coloJson = colo == std::string::npos ? std::string() : meta.substr(colo, meta.find('}', colo) - colo);
    const std::wstring city = JsonString(coloJson, "city"), iata = JsonString(coloJson, "iata");
    r.server = city.empty() ? iata : (iata.empty() ? city : city + L" (" + iata + L")");

    // 1) Idle latency: 20 probes on the warm connection.
    std::vector<double> pings;
    for (int i = 0; i < 20 && !st.stop_requested(); ++i) {
        const double ms = Probe(c);
        if (ms < 0) continue;
        pings.push_back(ms);
        onSample({SpeedSample::Ping, 0, ms, 0.02 + 0.08 * (i + 1) / 20.0, Format(L"Measuring latency… %.0f ms", ms)});
    }
    if (st.stop_requested()) { r.error = L"Cancelled."; return r; }
    if (pings.empty()) {
        r.error = L"Latency probes failed: " + ErrorText(GetLastError());
        return r;
    }
    r.pingMs = Median(pings);
    double jitter = 0;
    for (size_t i = 1; i < pings.size(); ++i) jitter += std::abs(pings[i] - pings[i - 1]);
    r.jitterMs = pings.size() > 1 ? jitter / double(pings.size() - 1) : 0;

    // 2) Download, 6 connections, 10 s (with latency under load measured alongside).
    std::vector<double> loaded;
    r.downMbps = Transfer(false, 6, 10.0, 0.10, 0.58, st, onSample, r.downBytes, &loaded);
    r.loadedPingMs = Median(loaded);
    if (st.stop_requested()) { r.error = L"Cancelled."; return r; }

    // 3) Upload, 4 connections, 8 s.
    r.upMbps = Transfer(true, 4, 8.0, 0.58, 1.0, st, onSample, r.upBytes, nullptr);
    if (st.stop_requested()) { r.error = L"Cancelled."; return r; }
    if (r.downBytes == 0 && r.upBytes == 0) {
        r.error = L"No data could be transferred — a firewall or proxy may be blocking speed.cloudflare.com.";
        return r;
    }
    r.ok = true;
    return r;
}

// ================================================================ DNS

const std::vector<DnsProvider>& DnsProviders() {
    static const std::vector<DnsProvider> list = {
        {L"Automatic (router / ISP)", {nullptr, nullptr}, {nullptr, nullptr}, L"Default",
         L"Uses whatever DNS servers your router or internet provider hands out (DHCP). This is how Windows is set up out of the box.",
         L"Pick this to undo a custom DNS. Your ISP's resolver is often close by, but it may be slower, log what you visit, "
         L"or redirect typos to ad pages.",
         true},
        {L"Cloudflare", {L"1.1.1.1", L"1.0.0.1"}, {L"2606:4700:4700::1111", L"2606:4700:4700::1001"}, L"Fastest · Privacy",
         L"Cloudflare's public resolver. Answers every lookup, no filtering.",
         L"Usually the fastest resolver in independent tests thanks to Cloudflare's huge network, and it has a strict, "
         L"audited privacy policy (no selling data, query logs wiped within 25 hours). Windows 11 can encrypt it with "
         L"DNS over HTTPS."},
        {L"Cloudflare · Malware blocking", {L"1.1.1.2", L"1.0.0.2"}, {L"2606:4700:4700::1112", L"2606:4700:4700::1002"},
         L"Fast · Security",
         L"Same Cloudflare resolver (1.1.1.1 for Families), but it refuses to resolve domains known to host malware or phishing.",
         L"All the speed of 1.1.1.1 plus a safety net: if you click a bad link, the site simply won't load."},
        {L"Cloudflare · Family", {L"1.1.1.3", L"1.0.0.3"}, {L"2606:4700:4700::1113", L"2606:4700:4700::1003"},
         L"Fast · Family",
         L"Cloudflare for Families: blocks malware and adult content.",
         L"A simple, free parental filter for a shared or kids' PC without installing any software."},
        {L"Google Public DNS", {L"8.8.8.8", L"8.8.4.4"}, {L"2001:4860:4860::8888", L"2001:4860:4860::8844"},
         L"Reliable · Accurate",
         L"Google's public resolver, the most widely used in the world. No filtering.",
         L"Extremely reliable with servers almost everywhere, and very accurate at sending you to the nearest copy of big "
         L"sites (CDNs). Logs are temporary but it is Google. Supports DNS over HTTPS in Windows 11."},
        {L"Quad9", {L"9.9.9.9", L"149.112.112.112"}, {L"2620:fe::fe", L"2620:fe::9"}, L"Security · Privacy",
         L"A Swiss non-profit resolver that blocks domains flagged as malicious by more than a dozen threat-intelligence feeds.",
         L"Strong malware and phishing protection with a privacy-first policy (no IP addresses logged, covered by Swiss and "
         L"EU privacy law). A great \"safer by default\" choice. Supports DNS over HTTPS in Windows 11."},
        {L"AdGuard DNS", {L"94.140.14.14", L"94.140.15.15"}, {L"2a10:50c0::ad1:ff", L"2a10:50c0::ad2:ff"},
         L"Blocks ads · Trackers",
         L"Blocks ads, trackers and analytics domains for the whole PC, in every browser and app.",
         L"The easiest way to get system-wide ad blocking without extensions — also cuts tracking inside apps and games. "
         L"If a site breaks, switch back or use AdGuard Unfiltered."},
        {L"AdGuard · Family", {L"94.140.14.15", L"94.140.15.16"}, {L"2a10:50c0::bad1:ff", L"2a10:50c0::bad2:ff"},
         L"Ads · Family",
         L"Everything AdGuard DNS blocks, plus adult sites, and it forces Safe Search in Google, Bing and YouTube.",
         L"Ad blocking and parental filtering in one setting."},
        {L"AdGuard · Unfiltered", {L"94.140.14.140", L"94.140.14.141"}, {L"2a10:50c0::1:ff", L"2a10:50c0::2:ff"},
         L"No filtering",
         L"AdGuard's resolver without any blocking.",
         L"An alternative to Cloudflare or Google from a European company, for when you want speed and privacy but no filtering."},
        {L"OpenDNS Home (Cisco)", {L"208.67.222.222", L"208.67.220.220"}, {L"2620:119:35::35", L"2620:119:53::53"},
         L"Reliable · Anti-phishing",
         L"Cisco's long-running public resolver with built-in phishing protection.",
         L"Very dependable (around since 2006) and a free account adds custom category filtering for your home network."},
        {L"OpenDNS FamilyShield", {L"208.67.222.123", L"208.67.220.123"}, {L"2620:119:35::123", L"2620:119:53::123"},
         L"Family",
         L"Pre-configured by Cisco to block adult content, proxies and phishing — no account needed.",
         L"Set-and-forget parental filtering from a well-known provider."},
        {L"CleanBrowsing · Family", {L"185.228.168.168", L"185.228.169.168"}, {L"2a0d:2a00:1::", L"2a0d:2a00:2::"},
         L"Family · Strict",
         L"Blocks adult content, proxies/VPN sites used to get around filters, and mixed-content sites; enforces Safe Search.",
         L"The strictest free family filter here — good for kids' devices where bypass sites matter."},
        {L"Control D · Ads & trackers", {L"76.76.2.2", L"76.76.10.2"}, {nullptr, nullptr}, L"Blocks ads · Malware",
         L"Control D's free resolver that blocks ads, trackers and malware.",
         L"A fast, modern alternative to AdGuard with a large global network. IPv4 only here; IPv6 DNS stays automatic."},
        {L"DNS.SB", {L"185.222.222.222", L"45.11.45.11"}, {L"2a09::", L"2a11::"}, L"Privacy",
         L"A small non-profit, no-logging resolver based in Germany. No filtering.",
         L"For people who prefer a resolver that isn't run by a big tech company or ad network."},
    };
    return list;
}

namespace {

std::wstring SockText(const SOCKADDR* sa) {
    wchar_t buf[INET6_ADDRSTRLEN] = {};
    if (sa->sa_family == AF_INET)
        InetNtopW(AF_INET, &reinterpret_cast<const sockaddr_in*>(sa)->sin_addr, buf, ARRAYSIZE(buf));
    else if (sa->sa_family == AF_INET6)
        InetNtopW(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(sa)->sin6_addr, buf, ARRAYSIZE(buf));
    return buf;
}

bool ValidIp(const std::wstring& s, int family) {
    BYTE buf[16];
    return InetPtonW(family, s.c_str(), buf) == 1;
}

std::wstring Join(const std::vector<std::wstring>& v) {
    std::wstring out;
    for (const auto& s : v) out += (out.empty() ? L"" : L",") + s;
    return out;
}

std::vector<std::wstring> Split(const std::wstring& s) {
    std::vector<std::wstring> out;
    if (s == L"-") return out;
    size_t start = 0;
    while (start <= s.size()) {
        const size_t c = s.find(L',', start);
        std::wstring part = s.substr(start, c == std::wstring::npos ? std::wstring::npos : c - start);
        if (!part.empty()) out.push_back(std::move(part));
        if (c == std::wstring::npos) break;
        start = c + 1;
    }
    return out;
}

}  // namespace

std::vector<NetAdapter> ListAdapters() {
    std::vector<NetAdapter> out;
    ULONG size = 32 * 1024;
    std::vector<BYTE> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 4 && r == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(size);
        r = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->IfType != IF_TYPE_ETHERNET_CSMACD && a->IfType != IF_TYPE_IEEE80211) continue;
        NetAdapter n;
        const std::string g = a->AdapterName;
        n.guid.assign(g.begin(), g.end());
        n.name = a->FriendlyName;
        n.description = a->Description;
        n.ifIndex = a->IfIndex;
        n.up = a->OperStatus == IfOperStatusUp;
        n.hasGateway = a->FirstGatewayAddress != nullptr;
        for (auto* d = a->FirstDnsServerAddress; d; d = d->Next) {
            const std::wstring s = SockText(d->Address.lpSockaddr);
            if (s.empty() || s.rfind(L"fec0:0:0:ffff::", 0) == 0) continue;  // Windows' placeholder "no IPv6 DNS"
            (d->Address.lpSockaddr->sa_family == AF_INET ? n.dns4 : n.dns6).push_back(s);
        }
        // Hand-set servers live in NameServer; DHCP-provided ones in DhcpNameServer.
        const std::wstring key = L"SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces\\" + n.guid;
        wchar_t value[512] = {};
        DWORD bytes = sizeof value;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"NameServer", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS)
            n.staticDns = value[0] != 0;
        bytes = sizeof value;
        value[0] = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"DhcpNameServer", RRF_RT_REG_SZ, nullptr, value, &bytes) == ERROR_SUCCESS) {
            std::wstring list = value;  // space separated
            for (wchar_t& ch : list)
                if (ch == L' ') ch = L',';
            for (auto& s : Split(list))
                if (ValidIp(s, AF_INET)) n.dhcpDns4.push_back(s);
        }
        out.push_back(std::move(n));
    }
    std::stable_sort(out.begin(), out.end(), [](const NetAdapter& a, const NetAdapter& b) {
        if (a.hasGateway != b.hasGateway) return a.hasGateway;
        return a.up && !b.up;
    });
    return out;
}

DWORD ApplyDns(const std::wstring& adapterGuid, const std::vector<std::wstring>& v4, const std::vector<std::wstring>& v6) {
    GUID guid{};
    if (FAILED(IIDFromString(adapterGuid.c_str(), &guid))) return ERROR_INVALID_PARAMETER;
    for (const auto& s : v4)
        if (!ValidIp(s, AF_INET)) return ERROR_INVALID_PARAMETER;
    for (const auto& s : v6)
        if (!ValidIp(s, AF_INET6)) return ERROR_INVALID_PARAMETER;

    auto set = [&](const std::vector<std::wstring>& list, bool ipv6) -> DWORD {
        std::wstring servers = Join(list);  // empty = back to DHCP
        DNS_INTERFACE_SETTINGS s{};
        s.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        s.Flags = DNS_SETTING_NAMESERVER | (ipv6 ? DNS_SETTING_IPV6 : 0);
        s.NameServer = servers.data();
        return SetInterfaceDnsSettings(guid, &s);
    };
    DWORD r = set(v4, false);
    if (r == ERROR_SUCCESS) r = set(v6, true);
    FlushDnsCache();
    return r;
}

DWORD ApplyDnsElevated(const std::wstring& adapterGuid, const std::vector<std::wstring>& v4, const std::vector<std::wstring>& v6) {
    if (IsElevated()) return ApplyDns(adapterGuid, v4, v6);
    const std::wstring params = L"--set-dns " + adapterGuid + L" " + (v4.empty() ? L"-" : Join(v4)) + L" " +
                                (v6.empty() ? L"-" : Join(v6));
    const DWORD result = RunSelfElevated(params, 60000);
    FlushDnsCache();
    return result;
}

bool HandleSetDnsCommandLine(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    const bool match = argc >= 2 && wcscmp(argv[1], L"--set-dns") == 0;
    if (match) {
        // Everything is validated in ApplyDns (GUID and each address must parse),
        // so the elevated process can't be talked into anything else.
        exitCode = argc == 5 ? static_cast<int>(ApplyDns(argv[2], Split(argv[3]), Split(argv[4])))
                             : static_cast<int>(ERROR_INVALID_PARAMETER);
    }
    LocalFree(argv);
    return match;
}

double DnsLatencyMs(const std::wstring& server, int tries) {
    static const bool wsa = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    if (!wsa) return -1;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    if (InetPtonW(AF_INET, server.c_str(), &to.sin_addr) != 1) return -1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return -1;
    double best = -1;
    for (int t = 0; t < tries; ++t) {
        // Standard query: A record for www.google.com, recursion desired. Popular names
        // are always cached, so this measures the trip to the resolver itself.
        const uint16_t id = static_cast<uint16_t>(GetTickCount64() * 2654435761u >> 7) ^ static_cast<uint16_t>(t);
        BYTE q[64];
        size_t n = 0;
        q[n++] = BYTE(id >> 8); q[n++] = BYTE(id);
        q[n++] = 0x01; q[n++] = 0x00;          // flags: RD
        q[n++] = 0; q[n++] = 1;                 // QDCOUNT
        for (int i = 0; i < 6; ++i) q[n++] = 0; // AN/NS/AR
        for (const char* label : {"www", "google", "com"}) {
            const size_t len = strlen(label);
            q[n++] = BYTE(len);
            memcpy(q + n, label, len);
            n += len;
        }
        q[n++] = 0;
        q[n++] = 0; q[n++] = 1;  // QTYPE A
        q[n++] = 0; q[n++] = 1;  // QCLASS IN
        Stopwatch sw;
        if (sendto(s, reinterpret_cast<const char*>(q), static_cast<int>(n), 0, reinterpret_cast<sockaddr*>(&to), sizeof to) < 0)
            continue;
        for (;;) {
            const double left = 1.5 - sw.Seconds();
            if (left <= 0) break;
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(s, &fds);
            timeval tv{0, static_cast<long>(left * 1e6)};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) break;
            BYTE reply[512];
            const int got = recv(s, reinterpret_cast<char*>(reply), sizeof reply, 0);
            if (got >= 12 && reply[0] == BYTE(id >> 8) && reply[1] == BYTE(id)) {
                const double ms = sw.Seconds() * 1000.0;
                if (best < 0 || ms < best) best = ms;
                break;
            }
        }
    }
    closesocket(s);
    return best;
}

void FlushDnsCache() {
    // Exported by dnsapi.dll but not in the SDK headers; same as "ipconfig /flushdns".
    if (HMODULE dll = LoadLibraryExW(L"dnsapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        using Fn = BOOL(WINAPI*)();
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(dll, "DnsFlushResolverCache"))) fn();
        FreeLibrary(dll);
    }
}

}  // namespace mtm
