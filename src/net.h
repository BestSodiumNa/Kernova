#pragma once

#include "common.h"

#include <stop_token>

namespace mtm {

// ---------------------------------------------------------------- speed test

struct SpeedSample {
    enum Phase { Info, Ping, Download, Upload, Done } phase = Info;
    double mbps = 0;        // current throughput (Download/Upload)
    double pingMs = 0;      // latest round trip (Ping)
    double fraction = 0;    // overall progress 0..1
    std::wstring text;      // status line
};

struct SpeedResult {
    bool ok = false;
    std::wstring error;
    double downMbps = 0, upMbps = 0;
    double pingMs = 0, jitterMs = 0;  // idle latency (median) and its variation
    double loadedPingMs = 0;          // latency while downloading ("bufferbloat")
    uint64_t downBytes = 0, upBytes = 0;
    std::wstring server, ip, isp;     // from speed.cloudflare.com/meta
};

// Measures against Cloudflare's speed test service (speed.cloudflare.com), the same
// endpoints its web test uses: ~20 latency probes, ~10 s download and ~8 s upload on
// several parallel connections. `onSample` is called from worker threads, about
// 8 times a second.
SpeedResult RunSpeedTest(std::stop_token st, const std::function<void(const SpeedSample&)>& onSample);

// ---------------------------------------------------------------- DNS

struct DnsProvider {
    const wchar_t* name;
    const wchar_t* v4[2];
    const wchar_t* v6[2];   // may be nullptr: IPv6 then stays automatic
    const wchar_t* tags;    // short chips: "Fast · Privacy"
    const wchar_t* what;    // what it does
    const wchar_t* why;     // why pick this one
    bool automatic = false; // "use what the router / ISP hands out"
};
const std::vector<DnsProvider>& DnsProviders();

struct NetAdapter {
    std::wstring guid;          // "{...}"
    std::wstring name;          // "Wi-Fi"
    std::wstring description;   // driver name
    ULONG ifIndex = 0;
    bool up = false;
    bool hasGateway = false;    // carries internet traffic
    bool staticDns = false;     // DNS servers set by hand (not from DHCP / router)
    std::vector<std::wstring> dns4, dns6;
    std::vector<std::wstring> dhcpDns4;  // what the router hands out (used by "Automatic")
};
// Ethernet / Wi-Fi adapters, the one with the default route first.
std::vector<NetAdapter> ListAdapters();

// Sets the adapter's DNS servers. Empty lists = automatic (DHCP). Needs admin.
// Returns ERROR_SUCCESS or a Win32 error.
DWORD ApplyDns(const std::wstring& adapterGuid, const std::vector<std::wstring>& v4, const std::vector<std::wstring>& v6);

// Applies DNS: directly when elevated, otherwise by relaunching this exe elevated with
// --set-dns (one UAC prompt) and waiting for it. Blocks; call from a worker thread.
DWORD ApplyDnsElevated(const std::wstring& adapterGuid, const std::vector<std::wstring>& v4, const std::vector<std::wstring>& v6);

// Handles "--set-dns <guid> <v4,list|-> <v6,list|->" from the command line.
// Returns true (and sets exitCode) if the command line was such a request.
bool HandleSetDnsCommandLine(int& exitCode);

// Round trip of a DNS query (A record for a well-known name) to `server` over UDP,
// best of `tries`. Returns a negative value on timeout / failure.
double DnsLatencyMs(const std::wstring& server, int tries = 3);

void FlushDnsCache();

}  // namespace mtm
