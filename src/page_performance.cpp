#include "graph.h"
#include "page.h"
#include "processes.h"
#include "sysinfo.h"

#include <winioctl.h>

#include <algorithm>
#include <map>

namespace mtm {
namespace {

// "NVIDIA GeForce RTX 3060 Laptop GPU" -> "RTX 3060 Laptop GPU": room for the legend.
std::wstring ShortGpuName(std::wstring n) {
    for (const wchar_t* junk : {L"NVIDIA ", L"GeForce ", L"AMD ", L"Intel(R) ", L"(TM)", L"(R)"})
        for (size_t p; (p = n.find(junk)) != std::wstring::npos;) n.erase(p, wcslen(junk));
    return n.empty() ? L"GPU" : n;
}

// Model name of \.\PhysicalDriveN (cached; opening with no access rights needs no admin).
std::wstring DiskModel(int number) {
    static std::map<int, std::wstring> cache;
    auto it = cache.find(number);
    if (it != cache.end()) return it->second;
    std::wstring model;
    HANDLE h = CreateFileW(Format(L"\\\\.\\PhysicalDrive%d", number).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        BYTE buf[1024] = {};
        DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, nullptr) &&
            got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            const auto* d = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf);
            if (d->ProductIdOffset && d->ProductIdOffset < got) {
                const char* p = reinterpret_cast<const char*>(buf) + d->ProductIdOffset;
                std::string s(p, strnlen(p, got - d->ProductIdOffset));
                while (!s.empty() && s.back() == ' ') s.pop_back();
                model.assign(s.begin(), s.end());
            }
        }
        CloseHandle(h);
    }
    cache[number] = model;
    return model;
}

// Distinct colours for up to five lines on one graph.
Graph::Tint SeriesTint(Graph::Tint first, size_t i) {
    static const Graph::Tint order[] = {Graph::Tint::Disk, Graph::Tint::Network, Graph::Tint::Memory, Graph::Tint::Gpu,
                                        Graph::Tint::Cpu};
    if (i == 0) return first;
    size_t k = 0;
    for (Graph::Tint t : order) {
        if (t == first) continue;
        if (++k == i) return t;
    }
    return first;
}

class PerformancePage final : public Page {
public:
    const wchar_t* Title() const override { return L"Performance"; }

    void Create() override {
        cpu_.Create(Graph::Tint::Cpu, 100);
        gpu_.Create(Graph::Tint::Gpu, 100);
        mem_.Create(Graph::Tint::Memory, 100);
        disk_.Create(Graph::Tint::Disk, 100);
        net_.Create(Graph::Tint::Network, 0);
        cores_.Create();
    }

    // Two rows of graph cards need about 300 px each before the plots get too flat.
    int MinHeight(int) const override { return Scale(640); }

    void Layout(RECT a) override {
        const int gap = Scale(14);
        const int w = (a.right - a.left - 2 * gap) / 3;
        const int h = (a.bottom - a.top - gap) / 2;
        Place(cpu_.Handle(), a.left, a.top, w, h);
        Place(gpu_.Handle(), a.left + w + gap, a.top, w, h);
        Place(mem_.Handle(), a.left + 2 * (w + gap), a.top, a.right - a.left - 2 * (w + gap), h);
        const int y = a.top + h + gap;
        Place(disk_.Handle(), a.left, y, w, a.bottom - y);
        Place(net_.Handle(), a.left + w + gap, y, w, a.bottom - y);
        Place(cores_.Handle(), a.left + 2 * (w + gap), y, a.right - a.left - 2 * (w + gap), a.bottom - y);
    }

    void Show(bool v) override {
        ShowAll({cpu_.Handle(), gpu_.Handle(), mem_.Handle(), disk_.Handle(), net_.Handle(), cores_.Handle()}, v);
    }

    // Push every tick, even when hidden, so the history is full when the page opens.
    void OnTick(bool) override {
        const SystemSnapshot& s = Sys();
        const DWORD logical = static_cast<DWORD>(s.cores.size());

        cpu_.Push(s.cpu);
        cpu_.SetTemps({CpuTempText(s.cpuTemp, true)});
        cpu_.SetText(L"CPU", Format(L"%.0f%%", s.cpu),
                     Format(L"%s\n%.2f GHz\t%lu threads\tup %s", CpuBrandName().c_str(), s.avgMhz / 1000.0, logical,
                            FormatDuration(GetTickCount64() / 1000).c_str()));

        std::vector<std::wstring> gpuTemps;
        for (const auto& g : s.gpus) gpuTemps.push_back(TempText(g.tempC));
        gpu_.SetTemps(std::move(gpuTemps));
        if (s.gpus.size() >= 2) {
            // One line per GPU: the dedicated one in the GPU colour, the others in contrasting ones.
            std::vector<Graph::Series> series;
            std::vector<double> values;
            std::vector<std::wstring> legend;
            for (size_t i = 0; i < s.gpus.size(); ++i) {
                const auto& g = s.gpus[i];
                series.push_back({ShortGpuName(g.name) + (g.integrated ? L" (integrated)" : L""),
                                  i == 0 ? Graph::Tint::Gpu : i == 1 ? Graph::Tint::Cpu : SeriesTint(Graph::Tint::Gpu, i)});
                values.push_back(g.usage);
                legend.push_back(Format(L"%.0f%%", g.usage));
            }
            gpu_.SetSeries(std::move(series));
            gpu_.PushMany(values, std::move(legend));
            const auto& d = s.gpus.front();
            gpu_.SetText(L"GPU", Format(L"%.0f%%", s.gpu),
                         Format(L"%s\t%s / %s dedicated\nAll GPUs: dedicated %s\tshared %s", ShortGpuName(d.name).c_str(),
                                FormatBytes(d.dedicatedUsed).c_str(), FormatBytes(d.dedicatedVram).c_str(),
                                FormatBytes(s.gpuDedicated).c_str(), FormatBytes(s.gpuShared).c_str()));
        } else {
            gpu_.Push(s.gpu);
            gpu_.SetText(L"GPU", s.gpuAvailable ? Format(L"%.0f%%", s.gpu) : L"—",
                         Format(L"%s\nDedicated %s%s\tShared %s", s.gpuName.empty() ? L"No GPU counters" : s.gpuName.c_str(),
                                FormatBytes(s.gpuDedicated).c_str(),
                                s.gpuDedicatedTotal ? (L" / " + FormatBytes(s.gpuDedicatedTotal)).c_str() : L"",
                                FormatBytes(s.gpuShared).c_str()));
        }

        const uint64_t used = s.memTotal - s.memAvail;
        const double memPct = s.memTotal ? 100.0 * double(used) / double(s.memTotal) : 0;
        mem_.Push(memPct);
        mem_.SetText(L"Memory", FormatBytes(used),
                     Format(L"%.0f%% of %s\t%s available\nCommitted %s / %s\tCached %s", memPct,
                            FormatBytes(s.memTotal).c_str(), FormatBytes(s.memAvail).c_str(), FormatBytes(s.commitUsed).c_str(),
                            FormatBytes(s.commitLimit).c_str(), FormatBytes(s.memCached).c_str()));

        std::vector<std::wstring> diskTemps;
        for (const auto& d : s.disks) diskTemps.push_back(TempText(d.tempC));
        disk_.SetTemps(std::move(diskTemps));
        if (s.disks.size() >= 2) {
            std::vector<Graph::Series> series;
            std::vector<double> values;
            std::vector<std::wstring> legend;
            double busiest = 0;
            for (size_t i = 0; i < s.disks.size(); ++i) {
                const auto& d = s.disks[i];
                const std::wstring model = DiskModel(d.number);
                std::wstring label = Format(L"Disk %d", d.number);
                if (!d.volumes.empty()) label += L" (" + d.volumes + L")";
                if (!model.empty()) label += L"  " + model;
                series.push_back({std::move(label), SeriesTint(Graph::Tint::Disk, i)});
                values.push_back(d.active);
                legend.push_back(Format(L"%.0f%%", d.active));
                busiest = std::max(busiest, d.active);
            }
            disk_.SetSeries(std::move(series));
            disk_.PushMany(values, std::move(legend));
            disk_.SetText(L"Disk", Format(L"%.0f%%", busiest),
                          Format(L"Active time, busiest of %zu disks\nRead %s/s\tWrite %s/s", s.disks.size(),
                                 FormatBytes(uint64_t(s.diskRead)).c_str(), FormatBytes(uint64_t(s.diskWrite)).c_str()));
        } else {
            disk_.Push(s.diskActive);
            disk_.SetText(L"Disk", Format(L"%.0f%%", s.diskActive),
                          Format(L"Active time, all drives\nRead %s/s\tWrite %s/s", FormatBytes(uint64_t(s.diskRead)).c_str(),
                                 FormatBytes(uint64_t(s.diskWrite)).c_str()));
        }

        const double net = s.netIn + s.netOut;
        net_.Push(net);
        net_.SetText(L"Network", FormatBytes(uint64_t(net)) + L"/s",
                     Format(L"Receive %s/s\tSend %s/s\n%d active adapter%s", FormatBytes(uint64_t(s.netIn)).c_str(),
                            FormatBytes(uint64_t(s.netOut)).c_str(), s.netAdapters, s.netAdapters == 1 ? L"" : L"s"));

        std::vector<CoreGrid::Core> cores;
        double busiest = 0;
        for (const CoreInfo& c : s.cores) {
            cores.push_back({c.usage, c.currentMhz});
            busiest = std::max(busiest, c.usage);
        }
        cores_.SetCores(std::move(cores), Format(L"busiest %.0f%%", busiest));
    }

private:
    Graph cpu_, gpu_, mem_, disk_, net_;
    CoreGrid cores_;
};

}  // namespace

std::unique_ptr<Page> MakePerformancePage() { return std::make_unique<PerformancePage>(); }

}  // namespace mtm
