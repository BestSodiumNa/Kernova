// Temperature sensors. GPU: WDDM adapter perf data. CPU: LibreHardwareMonitor/OpenHardwareMonitor
// over WMI when present, else the ACPI thermal zone. No kernel driver, no elevation, no .NET.
#include "sensors.h"

#include <winioctl.h>
#include <winternl.h>  // NTSTATUS, needed by d3dkmthk.h
#include <d3dkmthk.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace mtm {

namespace {

using namespace std::chrono_literals;

bool Plausible(double c) { return c >= 1 && c <= 125; }

// LibreHardwareMonitor and OpenHardwareMonitor expose the same Sensor class in their own namespaces.
struct Wmi {
    const wchar_t* ns;
    const wchar_t* label;
    IWbemServices* svc = nullptr;
};

bool Connect(Wmi& w) {
    IWbemLocator* loc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator,
                                reinterpret_cast<void**>(&loc))))
        return false;
    BSTR ns = SysAllocString(w.ns);
    const HRESULT hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &w.svc);
    SysFreeString(ns);
    loc->Release();
    if (FAILED(hr)) return false;
    CoSetProxyBlanket(w.svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

// Longest the sensor thread waits for one more row. The app waits for this thread when it exits, so
// this bounds how long closing can stall. It does not cover ExecQuery or ConnectServer themselves.
constexpr LONG kWmiRowTimeoutMs = 500;

// Package temperature if the monitor reports one, else the hottest CPU sensor. -1 if none.
// `timedOut` is set when a row didn't arrive in time: the monitor is slow, not gone, and what was
// read so far may lack the package sensor, so the result is -1 for this round.
double QueryCpu(IWbemServices* svc, bool& timedOut) {
    timedOut = false;
    BSTR lang = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT Name, Identifier, Value FROM Sensor WHERE SensorType='Temperature'");
    IEnumWbemClassObject* en = nullptr;
    const HRESULT hr = svc->ExecQuery(lang, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
    SysFreeString(lang);
    SysFreeString(query);
    if (FAILED(hr)) return -1;

    double package = -1, hottest = -1;
    for (;;) {
        IWbemClassObject* obj = nullptr;
        ULONG n = 0;
        const HRESULT next = en->Next(kWmiRowTimeoutMs, 1, &obj, &n);
        if (next == WBEM_S_TIMEDOUT) {
            timedOut = true;
            break;
        }
        if (next != S_OK || !n) break;
        VARIANT id, name, val;
        VariantInit(&id);
        VariantInit(&name);
        VariantInit(&val);
        obj->Get(L"Identifier", 0, &id, nullptr, nullptr);
        obj->Get(L"Name", 0, &name, nullptr, nullptr);
        obj->Get(L"Value", 0, &val, nullptr, nullptr);
        if (id.vt == VT_BSTR && name.vt == VT_BSTR && SUCCEEDED(VariantChangeType(&val, &val, 0, VT_R8))) {
            const std::wstring ident = id.bstrVal, nm = name.bstrVal;
            const bool cpu = ident.starts_with(L"/intelcpu/") || ident.starts_with(L"/amdcpu/");
            if (cpu && Plausible(val.dblVal)) {
                hottest = std::max(hottest, val.dblVal);
                if (nm == L"CPU Package" || nm == L"Core (Tctl/Tdie)") package = val.dblVal;
            }
        }
        VariantClear(&id);
        VariantClear(&name);
        VariantClear(&val);
        obj->Release();
    }
    en->Release();
    if (timedOut) return -1;
    return package >= 0 ? package : hottest;
}

// \Thermal Zone Information(*)\Temperature is in kelvin and readable without elevation.
double AcpiZone(PDH_HQUERY q, PDH_HCOUNTER c) {
    if (!q || !c || PdhCollectQueryData(q) != ERROR_SUCCESS) return -1;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA))
        return -1;
    std::vector<char> buf(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return -1;
    double best = -1;
    for (DWORD i = 0; i < count; ++i) {
        const double celsius = items[i].FmtValue.doubleValue - 273.15;
        if (Plausible(celsius)) best = std::max(best, celsius);
    }
    return best;
}

// Zero-access handle is enough for the bus-agnostic temperature property (same as health.cpp).
std::map<int, double> ReadDiskTemps() {
    std::map<int, double> out;
    for (int n = 0; n < 32; ++n) {
        HANDLE h = CreateFileW(Format(L"\\\\.\\PhysicalDrive%d", n).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceTemperatureProperty;
        q.QueryType = PropertyStandardQuery;
        std::vector<BYTE> buf(sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) + 8 * sizeof(STORAGE_TEMPERATURE_INFO));
        DWORD ret = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf.data(), static_cast<DWORD>(buf.size()),
                            &ret, nullptr)) {
            const auto* t = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buf.data());
            if (t->InfoCount > 0 && Plausible(t->TemperatureInfo[0].Temperature)) out[n] = t->TemperatureInfo[0].Temperature;
        }
        CloseHandle(h);
    }
    return out;
}

class SensorWorker {
public:
    SensorWorker() : thread_([this](std::stop_token st) { Run(st); }) {}

    CpuTemp Get() {
        std::lock_guard lock(mu_);
        return value_;
    }

    double DiskTemp(int number) {
        std::lock_guard lock(mu_);
        auto it = disks_.find(number);
        return it == disks_.end() ? -1 : it->second;
    }

private:
    void Run(std::stop_token st) {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        Wmi monitors[] = {{L"ROOT\\LibreHardwareMonitor", L"LibreHardwareMonitor"},
                          {L"ROOT\\OpenHardwareMonitor", L"OpenHardwareMonitor"}};
        PDH_HQUERY q = nullptr;
        PDH_HCOUNTER zone = nullptr;
        if (PdhOpenQueryW(nullptr, 0, &q) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(q, L"\\Thermal Zone Information(*)\\Temperature", 0, &zone) != ERROR_SUCCESS)
            zone = nullptr;

        for (unsigned cycle = 0; !st.stop_requested(); ++cycle) {
            CpuTemp t;
            for (Wmi& m : monitors) {
                // A missing namespace fails fast but not free; recheck it every 10th cycle.
                if (!m.svc && (cycle % 10 != 0 || !Connect(m))) continue;
                bool timedOut = false;
                const double c = QueryCpu(m.svc, timedOut);
                if (c >= 0) {
                    t = {c, m.label};
                    break;
                }
                if (timedOut) break;  // slow this round: keep the connection, show the ACPI value for now
                m.svc->Release();  // monitor closed: drop the connection and look again later
                m.svc = nullptr;
            }
            if (t.c < 0)
                if (const double c = AcpiZone(q, zone); c >= 0) t = {c, L"ACPI thermal zone"};
            std::map<int, double> disks;
            if (cycle % 5 == 0) disks = ReadDiskTemps();
            {
                std::lock_guard lock(mu_);
                value_ = std::move(t);
                if (cycle % 5 == 0) disks_ = std::move(disks);
            }
            std::mutex m;
            std::unique_lock lock(m);
            cv_.wait_for(lock, st, 2s, [] { return false; });
        }

        for (Wmi& m : monitors)
            if (m.svc) m.svc->Release();
        if (q) PdhCloseQuery(q);
        if (com) CoUninitialize();
    }

    std::mutex mu_;
    CpuTemp value_;
    std::map<int, double> disks_;  // physical disk number -> °C
    std::condition_variable_any cv_;
    std::jthread thread_;  // last: joined before the members it uses are destroyed
};

SensorWorker& Worker() {
    static SensorWorker worker;
    return worker;
}

}  // namespace

double GpuTempC(const LUID& adapter) {
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = adapter;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return -1;
    D3DKMT_ADAPTER_PERFDATA perf{};
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_ADAPTERPERFDATA;
    q.pPrivateDriverData = &perf;
    q.PrivateDriverDataSize = sizeof perf;
    const NTSTATUS st = D3DKMTQueryAdapterInfo(&q);
    D3DKMT_CLOSEADAPTER close{open.hAdapter};
    D3DKMTCloseAdapter(&close);
    const double c = perf.Temperature / 10.0;  // the driver reports tenths of a degree
    return st == 0 && Plausible(c) ? c : -1;
}

CpuTemp CpuTempNow() { return Worker().Get(); }

std::wstring TempText(double c) { return c < 0 ? L"" : Format(L"%.0f °C", c); }

std::wstring CpuTempText(const CpuTemp& t, bool brief) {
    if (t.c < 0) return L"";
    if (t.source != L"ACPI thermal zone") return TempText(t.c);
    return brief ? L"~" + TempText(t.c) : TempText(t.c) + L" (ACPI zone, approx.)";
}

double DiskTempC(int number) {
    return Worker().DiskTemp(number);
}

}  // namespace mtm
