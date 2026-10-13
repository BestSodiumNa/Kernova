#pragma once

#include "common.h"

#include <stop_token>
#include <utility>

namespace mtm {

struct BenchResult {
    std::wstring test;     // "CPU", "GPU", "Disk C:\"
    std::wstring device;   // CPU / GPU / drive the test ran on
    std::wstring score, details;
    std::wstring headline;  // short big number for cards ("3,412 MB/s")
    double value = 0;       // headline number, for comparing runs
    bool ok = true;
    uint64_t when = 0;     // FILETIME (UTC) when the run finished
    // Everything measured, shown in the details panel ("Single-thread", "11.3 GFLOPS").
    std::vector<std::pair<std::wstring, std::wstring>> metrics;
    // Throughput sampled during the run, drawn as a chart (e.g. GFLOPS or MB/s over time).
    std::vector<double> timeline;
    std::wstring timelineUnit;
    std::wstring timelineNote;  // e.g. "write then read"
};

// Called at most ~10 times a second from the benchmark thread.
using BenchProgress = std::function<void(double fraction, const std::wstring& text)>;

struct GpuAdapter {
    LUID luid{};
    std::wstring name;
    uint64_t dedicatedVram = 0;
    bool highPerformance = false;  // Windows' choice for "High performance" in graphics settings
    bool integrated = false;       // little or no dedicated memory
};

// Hardware adapters, the high-performance (usually discrete) one first.
std::vector<GpuAdapter> EnumerateGpus();

BenchResult RunCpuBenchmark(std::stop_token st, const BenchProgress& progress);
BenchResult RunDiskBenchmark(const std::wstring& driveRoot, std::stop_token st, const BenchProgress& progress);
BenchResult RunGpuBenchmark(const GpuAdapter& gpu, std::stop_token st, const BenchProgress& progress);

}  // namespace mtm
