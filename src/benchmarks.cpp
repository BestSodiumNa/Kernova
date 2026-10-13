#include "benchmarks.h"

#include "sysinfo.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <thread>

namespace mtm {
namespace {

using Microsoft::WRL::ComPtr;

BenchResult Fail(BenchResult r, std::wstring why) {
    r.ok = false;
    r.score = L"Failed";
    r.details = std::move(why);
    return r;
}

BenchResult Cancelled(BenchResult r) {
    r.ok = false;
    r.score = L"Cancelled";
    return r;
}

uint64_t NowFileTime() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// Rate-limits progress callbacks to ~10 per second.
class Throttle {
public:
    bool Ready() {
        if (sw_.Seconds() < 0.1) return false;
        sw_.Reset();
        return true;
    }

private:
    Stopwatch sw_;
};

// Turns a running total into per-interval rates for the results chart.
class RateSampler {
public:
    explicit RateSampler(double interval) : interval_(interval) {}
    void Add(double amount) { pending_ += amount; }
    // Call often; records a sample whenever `interval_` seconds have passed.
    void Tick(std::vector<double>& out, double scale) {
        const double secs = sw_.Seconds();
        if (secs < interval_) return;
        out.push_back(pending_ / secs * scale);
        pending_ = 0;
        sw_.Reset();
    }

private:
    double interval_;
    double pending_ = 0;
    Stopwatch sw_;
};

// ---------------------------------------------------------------- CPU

volatile double g_sink = 0;  // keeps the optimizer from deleting the work

// 8 independent multiply-add chains: 16 floating-point operations per iteration.
double CpuKernel(double seed, uint64_t iterations) {
    double a0 = seed, a1 = seed + 0.1, a2 = seed + 0.2, a3 = seed + 0.3;
    double a4 = seed + 0.4, a5 = seed + 0.5, a6 = seed + 0.6, a7 = seed + 0.7;
    const double m = 0.9999999, c = 1e-7;
    for (uint64_t i = 0; i < iterations; ++i) {
        a0 = a0 * m + c; a1 = a1 * m + c; a2 = a2 * m + c; a3 = a3 * m + c;
        a4 = a4 * m + c; a5 = a5 * m + c; a6 = a6 * m + c; a7 = a7 * m + c;
    }
    return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

constexpr uint64_t kCpuIterations = 1u << 20;
constexpr double kCpuFlopsPerBlock = 16.0 * double(kCpuIterations);

// ---------------------------------------------------------------- GPU

// Each thread: 256 iterations x 2 float4 multiply-adds = 256 x 16 = 4096 FLOPs.
constexpr char kGpuShader[] = R"(
RWStructuredBuffer<float4> output : register(u0);

[numthreads(256, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    float4 a = float4(tid.x * 1.0e-7, 0.5, 0.25, 0.125);
    float4 b = a + 0.75;
    [loop]
    for (uint i = 0; i < 256; ++i)
    {
        a = a * 0.99999 + 0.00001;
        b = b * 1.00001 - 0.00001;
    }
    output[tid.x] = a + b;
}
)";
constexpr UINT kGpuGroups = 8192;
constexpr UINT kGpuThreads = kGpuGroups * 256;
constexpr double kGpuFlopsPerDispatch = double(kGpuThreads) * 4096.0;

const wchar_t* FeatureLevelName(D3D_FEATURE_LEVEL fl) {
    switch (fl) {
    case D3D_FEATURE_LEVEL_11_1: return L"11_1";
    case D3D_FEATURE_LEVEL_11_0: return L"11_0";
    default: return L"?";
    }
}

}  // namespace

std::vector<GpuAdapter> EnumerateGpus() {
    std::vector<GpuAdapter> out;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return out;

    // On hybrid laptops Windows can rank adapters by preference; the first one in
    // high-performance order is the discrete GPU that games would use.
    LUID preferred{};
    bool havePreferred = false;
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory.As(&factory6))) {
        ComPtr<IDXGIAdapter1> best;
        if (SUCCEEDED(factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&best)))) {
            DXGI_ADAPTER_DESC1 d{};
            if (SUCCEEDED(best->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                preferred = d.AdapterLuid;
                havePreferred = true;
            }
        }
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 d{};
        adapter->GetDesc1(&d);
        adapter.Reset();
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;  // skip "Microsoft Basic Render Driver"
        GpuAdapter g;
        g.luid = d.AdapterLuid;
        g.name = d.Description;
        g.dedicatedVram = d.DedicatedVideoMemory;
        g.integrated = d.DedicatedVideoMemory < (1ull << 30);  // iGPUs carve out well under 1 GB
        g.highPerformance = havePreferred && d.AdapterLuid.LowPart == preferred.LowPart &&
                            d.AdapterLuid.HighPart == preferred.HighPart;
        out.push_back(std::move(g));
    }
    // High-performance first, then by dedicated memory.
    std::stable_sort(out.begin(), out.end(), [](const GpuAdapter& a, const GpuAdapter& b) {
        if (a.highPerformance != b.highPerformance) return a.highPerformance;
        return a.dedicatedVram > b.dedicatedVram;
    });
    return out;
}

BenchResult RunCpuBenchmark(std::stop_token st, const BenchProgress& progress) {
    BenchResult r;
    r.test = L"CPU";
    r.device = CpuBrandName();
    r.timelineUnit = L"GFLOPS";
    r.timelineNote = L"single-thread for 3 s, then all threads for 5 s";
    Throttle throttle;
    RateSampler rate(0.25);

    // Phase 1: one thread, 3 seconds.
    Stopwatch single;
    uint64_t blocks = 0;
    double sink = 0;
    while (single.Seconds() < 3.0) {
        if (st.stop_requested()) return Cancelled(r);
        sink += CpuKernel(double(blocks) * 1e-9, kCpuIterations);
        ++blocks;
        rate.Add(kCpuFlopsPerBlock);
        rate.Tick(r.timeline, 1e-9);
        if (throttle.Ready()) progress(single.Seconds() / 8.0, L"CPU: single-thread test…");
    }
    const double singleSecs = single.Seconds();
    const double singleGflops = double(blocks) * kCpuFlopsPerBlock / singleSecs / 1e9;

    // Phase 2: one thread per logical processor, 5 seconds.
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> total{0};  // blocks completed by all threads, updated as they go
    std::vector<std::thread> pool;
    Stopwatch multi;
    for (unsigned t = 0; t < threads; ++t) {
        pool.emplace_back([&, t] {
            uint64_t local = 0;
            double s = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                s += CpuKernel(double(t) + double(local) * 1e-9, kCpuIterations);
                ++local;
                total.fetch_add(1, std::memory_order_relaxed);
            }
            g_sink = g_sink + s;
        });
    }
    uint64_t lastTotal = 0;
    while (multi.Seconds() < 5.0 && !st.stop_requested()) {
        Sleep(50);
        const uint64_t now = total.load(std::memory_order_relaxed);
        rate.Add(double(now - lastTotal) * kCpuFlopsPerBlock);
        lastTotal = now;
        rate.Tick(r.timeline, 1e-9);
        if (throttle.Ready()) progress((3.0 + multi.Seconds()) / 8.0, Format(L"CPU: multi-thread test on %u threads…", threads));
    }
    stop = true;
    for (auto& t : pool) t.join();
    const double multiSecs = multi.Seconds();
    g_sink = g_sink + sink;
    if (st.stop_requested()) return Cancelled(r);

    const double multiGflops = double(total.load()) * kCpuFlopsPerBlock / multiSecs / 1e9;
    const double scaling = singleGflops > 0 ? multiGflops / singleGflops : 0.0;
    r.score = Format(L"%.1f GFLOPS multi-thread · %.2f single-thread", multiGflops, singleGflops);
    r.headline = Format(L"%.1f GFLOPS", multiGflops);
    r.value = multiGflops;
    r.details = Format(L"%s · %u threads · %.1fx scaling · FP64 multiply-add", r.device.c_str(), threads, scaling);
    double peak = 0;
    for (double v : r.timeline) peak = std::max(peak, v);
    r.metrics = {
        {L"Processor", r.device},
        {L"Multi-thread", Format(L"%.1f GFLOPS", multiGflops)},
        {L"Single-thread", Format(L"%.2f GFLOPS", singleGflops)},
        {L"Peak (250 ms window)", Format(L"%.1f GFLOPS", peak)},
        {L"Threads used", std::to_wstring(threads)},
        {L"Multi-core scaling", Format(L"%.1fx", scaling)},
        {L"Per-thread efficiency", Format(L"%.0f%% of single-thread", threads ? 100.0 * scaling / threads : 0.0)},
        {L"Average clock", Format(L"%.2f GHz", Sys().avgMhz / 1000.0)},
        {L"Workload", L"FP64 multiply-add, 8 independent chains"},
        {L"Duration", Format(L"%.1f s single + %.1f s multi", singleSecs, multiSecs)},
    };
    r.when = NowFileTime();
    return r;
}

BenchResult RunDiskBenchmark(const std::wstring& driveRoot, std::stop_token st, const BenchProgress& progress) {
    BenchResult r;
    r.test = L"Disk " + driveRoot;
    r.timelineUnit = L"MB/s";
    r.timelineNote = L"sequential write, then sequential read";
    constexpr uint64_t MiB = 1ull << 20, GiB = 1ull << 30;
    constexpr DWORD kChunk = 8 * MiB;
    constexpr DWORD kBlock = 4096;
    constexpr double kPhaseSeconds = 5.0;

    wchar_t label[MAX_PATH] = {}, fs[MAX_PATH] = {};
    GetVolumeInformationW(driveRoot.c_str(), label, MAX_PATH, nullptr, nullptr, nullptr, fs, MAX_PATH);
    r.device = driveRoot + (label[0] ? std::wstring(L" ") + label : L"") + (fs[0] ? std::wstring(L" (") + fs + L")" : L"");

    // The system drive's root needs admin rights to write to, so use %TEMP% there.
    std::wstring folder = driveRoot;
    wchar_t winDir[MAX_PATH] = {};
    GetWindowsDirectoryW(winDir, MAX_PATH);
    if (_wcsnicmp(winDir, driveRoot.c_str(), 2) == 0) {
        wchar_t tmp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmp);
        folder = tmp;
    }
    if (folder.back() != L'\\') folder += L'\\';

    ULARGE_INTEGER freeBytes{};
    if (!GetDiskFreeSpaceExW(folder.c_str(), &freeBytes, nullptr, nullptr)) return Fail(r, ErrorText(GetLastError()));
    if (freeBytes.QuadPart < GiB + 256 * MiB) return Fail(r, L"Not enough free space (needs about 1.3 GB free).");
    const uint64_t fileSize = std::clamp<uint64_t>(freeBytes.QuadPart - GiB, 256 * MiB, 4 * GiB) / kChunk * kChunk;

    const std::wstring path = folder + Format(L".mtm-diskbench-%lu.tmp", GetCurrentProcessId());
    // Unbuffered + write-through measures the drive, not the Windows cache.
    // Delete-on-close removes the file even if the app crashes.
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_NO_BUFFERING |
                                  FILE_FLAG_WRITE_THROUGH | FILE_FLAG_DELETE_ON_CLOSE,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return Fail(r, L"Couldn't create the test file: " + ErrorText(GetLastError()));
    auto* buf = static_cast<BYTE*>(VirtualAlloc(nullptr, kChunk, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buf) {
        CloseHandle(file);
        return Fail(r, L"Couldn't allocate the transfer buffer.");
    }
    // Random data so drives that compress can't inflate the result.
    uint64_t x = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < kChunk / 8; ++i) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        reinterpret_cast<uint64_t*>(buf)[i] = x;
    }
    auto cleanup = [&] {
        VirtualFree(buf, 0, MEM_RELEASE);
        CloseHandle(file);
    };
    auto io = [&](bool write, uint64_t offset, DWORD bytes) {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD done = 0;
        const BOOL ok = write ? WriteFile(file, buf, bytes, &done, &ov) : ReadFile(file, buf, bytes, &done, &ov);
        return ok && done == bytes;
    };
    Throttle throttle;
    RateSampler rate(0.25);
    const double toMB = 1.0 / double(MiB);

    // 1) Sequential write, 8 MB blocks.
    uint64_t offset = 0, written = 0, region = 0;
    Stopwatch sw;
    while (sw.Seconds() < kPhaseSeconds) {
        if (st.stop_requested()) { cleanup(); return Cancelled(r); }
        if (!io(true, offset, kChunk)) { const DWORD e = GetLastError(); cleanup(); return Fail(r, L"Write failed: " + ErrorText(e)); }
        written += kChunk;
        offset += kChunk;
        region = std::max(region, offset);
        if (offset >= fileSize) offset = 0;
        rate.Add(kChunk);
        rate.Tick(r.timeline, toMB);
        if (throttle.Ready())
            progress(sw.Seconds() / (3 * kPhaseSeconds), Format(L"Disk: sequential write %.0f MB/s…", double(written) / MiB / sw.Seconds()));
    }
    FlushFileBuffers(file);
    const double writeSecs = sw.Seconds();
    const double writeMBs = double(written) / MiB / writeSecs;

    // 2) Sequential read over what was written.
    uint64_t read = 0;
    offset = 0;
    sw.Reset();
    while (sw.Seconds() < kPhaseSeconds) {
        if (st.stop_requested()) { cleanup(); return Cancelled(r); }
        if (!io(false, offset, kChunk)) { const DWORD e = GetLastError(); cleanup(); return Fail(r, L"Read failed: " + ErrorText(e)); }
        read += kChunk;
        offset += kChunk;
        if (offset >= region) offset = 0;
        rate.Add(kChunk);
        rate.Tick(r.timeline, toMB);
        if (throttle.Ready())
            progress((kPhaseSeconds + sw.Seconds()) / (3 * kPhaseSeconds), Format(L"Disk: sequential read %.0f MB/s…", double(read) / MiB / sw.Seconds()));
    }
    const double readMBs = double(read) / MiB / sw.Seconds();

    // 3) Random 4 KB reads, one at a time (latency-bound, like app launches).
    //    Each read is timed so we can report typical and worst-case latency.
    const uint64_t blocks = region / kBlock;
    std::vector<float> latencyUs;
    latencyUs.reserve(200000);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    sw.Reset();
    while (sw.Seconds() < kPhaseSeconds && blocks) {
        if (st.stop_requested()) { cleanup(); return Cancelled(r); }
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        if (!io(false, (x % blocks) * kBlock, kBlock)) { const DWORD e = GetLastError(); cleanup(); return Fail(r, L"Random read failed: " + ErrorText(e)); }
        QueryPerformanceCounter(&b);
        latencyUs.push_back(float(double(b.QuadPart - a.QuadPart) * 1e6 / double(freq.QuadPart)));
        if ((latencyUs.size() & 255) == 0 && throttle.Ready())
            progress((2 * kPhaseSeconds + sw.Seconds()) / (3 * kPhaseSeconds),
                     Format(L"Disk: random 4K read %.0f IOPS…", double(latencyUs.size()) / sw.Seconds()));
    }
    const double randSecs = sw.Seconds();
    const double iops = double(latencyUs.size()) / randSecs;
    cleanup();

    auto percentile = [&](double p) -> double {
        if (latencyUs.empty()) return 0;
        const size_t k = std::min(latencyUs.size() - 1, static_cast<size_t>(p * double(latencyUs.size())));
        std::nth_element(latencyUs.begin(), latencyUs.begin() + static_cast<std::ptrdiff_t>(k), latencyUs.end());
        return latencyUs[k];
    };
    const double p50 = percentile(0.50), p99 = percentile(0.99);

    r.score = Format(L"Read %.0f MB/s · Write %.0f MB/s · 4K %.0f IOPS", readMBs, writeMBs, iops);
    r.headline = Format(L"%.0f MB/s", readMBs);
    r.value = readMBs;
    r.details = Format(L"Sequential 8 MB blocks, unbuffered · random 4K reads at queue depth 1 · %s test file", FormatBytes(region).c_str());
    r.metrics = {
        {L"Drive", r.device},
        {L"Sequential read", Format(L"%.0f MB/s", readMBs)},
        {L"Sequential write", Format(L"%.0f MB/s", writeMBs)},
        {L"Random 4K read (QD1)", Format(L"%.0f IOPS  ·  %.1f MB/s", iops, iops * kBlock / MiB)},
        {L"4K latency, typical (p50)", Format(L"%.0f µs", p50)},
        {L"4K latency, worst 1% (p99)", Format(L"%.0f µs", p99)},
        {L"Data written", FormatBytes(written)},
        {L"Data read", FormatBytes(read + uint64_t(latencyUs.size()) * kBlock)},
        {L"Test file", FormatBytes(region) + L" in " + folder},
        {L"Method", L"Unbuffered, write-through, random data"},
        {L"Duration", Format(L"%.1f s write + 5.0 s read + %.1f s random", writeSecs, randSecs)},
    };
    r.when = NowFileTime();
    return r;
}

BenchResult RunGpuBenchmark(const GpuAdapter& gpu, std::stop_token st, const BenchProgress& progress) {
    BenchResult r;
    r.test = L"GPU";
    r.device = gpu.name;
    r.timelineUnit = L"GFLOPS";
    r.timelineNote = L"FP32 compute throughput over 5 s";

    // Find the adapter the user picked; on hybrid laptops the default device is
    // usually the integrated GPU, so it has to be selected explicitly.
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter1> chosen;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            a->GetDesc1(&d);
            if (d.AdapterLuid.LowPart == gpu.luid.LowPart && d.AdapterLuid.HighPart == gpu.luid.HighPart) {
                chosen = a;
                break;
            }
            a.Reset();
        }
    }
    if (!chosen) return Fail(r, L"That GPU is no longer available.");

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // With an explicit adapter the driver type must be UNKNOWN.
    HRESULT hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                   &device, &got, &ctx);
    if (hr == E_INVALIDARG)  // runtimes without 11.1 reject the whole list
        hr = D3D11CreateDevice(chosen.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels + 1, 1, D3D11_SDK_VERSION,
                               &device, &got, &ctx);
    if (FAILED(hr)) return Fail(r, L"Couldn't create a Direct3D 11 device on " + gpu.name + L".");

    ComPtr<ID3DBlob> code, errors;
    hr = D3DCompile(kGpuShader, sizeof kGpuShader - 1, "mtm_gpu_benchmark", nullptr, nullptr, "main", "cs_5_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        std::wstring msg = L"Shader compile failed.";
        if (errors) {
            const char* e = static_cast<const char*>(errors->GetBufferPointer());
            msg += L" " + std::wstring(e, e + errors->GetBufferSize());
        }
        return Fail(r, msg);
    }
    ComPtr<ID3D11ComputeShader> shader;
    if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)))
        return Fail(r, L"Couldn't create the compute shader.");

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = kGpuThreads * 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = 16;
    ComPtr<ID3D11Buffer> buffer;
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = kGpuThreads;
    ComPtr<ID3D11UnorderedAccessView> uav;
    D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> done;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &buffer)) ||
        FAILED(device->CreateUnorderedAccessView(buffer.Get(), &ud, &uav)) || FAILED(device->CreateQuery(&qd, &done)))
        return Fail(r, L"Couldn't allocate GPU resources.");

    ctx->CSSetShader(shader.Get(), nullptr, 0);
    ID3D11UnorderedAccessView* uavs[] = {uav.Get()};
    ctx->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

    // Waits until the GPU has really finished everything submitted so far.
    auto finish = [&]() -> bool {
        ctx->End(done.Get());
        for (;;) {
            const HRESULT s = ctx->GetData(done.Get(), nullptr, 0, 0);
            if (s == S_OK) return true;
            if (FAILED(s)) return false;
            SwitchToThread();
        }
    };

    Stopwatch warm;
    ctx->Dispatch(kGpuGroups, 1, 1);  // warm-up: clocks ramp, shader gets cached
    if (!finish()) return Fail(r, L"The GPU stopped responding.");
    const double warmMs = warm.Seconds() * 1000.0;

    uint32_t batch = 1;
    uint64_t dispatches = 0;
    double bestBatchMs = 1e9;
    Stopwatch sw;
    Throttle throttle;
    RateSampler rate(0.25);
    while (sw.Seconds() < 5.0) {
        if (st.stop_requested()) return Cancelled(r);
        Stopwatch batchTime;
        for (uint32_t i = 0; i < batch; ++i) ctx->Dispatch(kGpuGroups, 1, 1);
        if (!finish()) return Fail(r, L"The GPU stopped responding (device removed).");
        dispatches += batch;
        bestBatchMs = std::min(bestBatchMs, batchTime.Seconds() * 1000.0 / batch);
        rate.Add(double(batch) * kGpuFlopsPerDispatch);
        rate.Tick(r.timeline, 1e-9);
        // Grow batches until each takes ~50 ms so sync overhead doesn't dominate.
        if (batchTime.Seconds() < 0.05 && batch < 4096) batch *= 2;
        if (throttle.Ready())
            progress(sw.Seconds() / 5.0, Format(L"GPU: %.0f GFLOPS on %s…", double(dispatches) * kGpuFlopsPerDispatch / sw.Seconds() / 1e9, gpu.name.c_str()));
    }
    const double secs = sw.Seconds();
    const double gflops = double(dispatches) * kGpuFlopsPerDispatch / secs / 1e9;
    double peak = 0;
    for (double v : r.timeline) peak = std::max(peak, v);
    r.score = Format(L"%.0f GFLOPS", gflops);
    r.headline = r.score;
    r.value = gflops;
    r.details = Format(L"%s · FP32 compute · %llu dispatches in %.1f s", gpu.name.c_str(),
                       static_cast<unsigned long long>(dispatches), secs);
    r.metrics = {
        {L"Graphics adapter", gpu.name},
        {L"Type", gpu.integrated ? L"Integrated" : L"Dedicated"},
        {L"Dedicated memory", FormatBytes(gpu.dedicatedVram)},
        {L"Sustained throughput", Format(L"%.0f GFLOPS  (%.2f TFLOPS)", gflops, gflops / 1000.0)},
        {L"Peak (250 ms window)", Format(L"%.0f GFLOPS", peak)},
        {L"Fastest dispatch", Format(L"%.2f ms for %.1f GFLOP", bestBatchMs, kGpuFlopsPerDispatch / 1e9)},
        {L"Dispatches", FormatCount(dispatches)},
        {L"Threads per dispatch", FormatCount(kGpuThreads)},
        {L"First dispatch (warm-up)", Format(L"%.1f ms", warmMs)},
        {L"API", Format(L"Direct3D 11, feature level %s, compute shader cs_5_0", FeatureLevelName(got))},
        {L"Duration", Format(L"%.1f s", secs)},
    };
    r.when = NowFileTime();
    return r;
}

}  // namespace mtm
