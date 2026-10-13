#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mtm {

struct TraceProcess {
    uint32_t    pid;
    float       cpu;
    uint64_t    memory;
    std::wstring name;
};

struct TraceSample {
    uint64_t time;          // FILETIME (100 ns since 1601), Windows-only
    uint64_t monotonic;     // GetTickCount64 at capture, for interval math

    float    cpu;
    float    gpu;
    float    diskActive;

    uint64_t memUsed;
    uint64_t memTotal;
    uint64_t diskRead;
    uint64_t diskWrite;
    uint64_t netIn;
    uint64_t netOut;
    uint64_t gpuMemory;

    std::vector<float>    coreUsage;   // percent, one per logical CPU
    std::vector<uint16_t> coreMhz;     // current MHz, clipped to 65535

    std::vector<TraceProcess> top;     // top-N by CPU at capture time
};

// Ring buffer of recent samples, plus optional on-disk recording.
// Thread-safe: Add() may be called from the sampler thread while Start/Stop/
// Save are called from the UI thread.
class FlightRecorder {
public:
    static constexpr size_t kBufferSize = 3600;   // ~1 h at 1 Hz

    // Joins the writer thread; a still-running std::thread at destruction would
    // call std::terminate. Prefer calling Stop() explicitly (e.g. in WM_DESTROY)
    // rather than relying on global destruction order.
    ~FlightRecorder() { Stop(); }

    void Add(const TraceSample& s);

    bool Start(const std::wstring& path, std::wstring& error);
    void Stop();
    // Locked: the writer thread closes file_ when a write fails.
    bool Recording() const {
        std::lock_guard lk(mu_);
        return file_ != INVALID_HANDLE_VALUE;
    }

    // For the page's status line.
    std::wstring Path() const {
        std::lock_guard lk(mu_);
        return path_;
    }
    uint64_t Written() const { return written_.load(); }
    // Buffered sample count without copying the buffer.
    size_t Size() const {
        std::lock_guard lk(mu_);
        return buffer_.size();
    }
    // Why the last recording stopped on its own (empty after a normal Stop()).
    std::wstring LastError() const {
        std::lock_guard lk(mu_);
        return lastError_;
    }

    // Snapshot of the in-memory ring, oldest first.
    std::vector<TraceSample> Snapshot() const;

    // Save an arbitrary set of samples to a new trace file.
    static bool Save(const std::wstring& path,
                     const std::vector<TraceSample>& samples,
                     std::wstring& error);

    // Load a trace file. Tolerates a truncated final record; fails on a
    // corrupt middle record rather than producing garbage samples.
    static bool Load(const std::wstring& path,
                     std::vector<TraceSample>& out,
                     std::wstring& error);

private:
    // Writer thread state -----------------------------------------------
    void WriterLoop();
    void EnqueueForWrite(const TraceSample& s);
    void FlushAndClose();

    mutable std::mutex      mu_;
    std::deque<TraceSample> buffer_;
    std::vector<TraceSample> writeQueue_;   // SPSC: sampler pushes, writer drains
    size_t                  writeHead_ = 0;

    HANDLE                  file_ = INVALID_HANDLE_VALUE;
    std::wstring            path_;
    std::wstring            lastError_;
    std::atomic<uint64_t>   written_{0};             // incremented by the writer thread

    std::thread             writer_;
    std::atomic<bool>       writerStop_{false};
    HANDLE                  writerWake_ = nullptr;   // auto-reset event
};

extern FlightRecorder g_recorder;

TraceSample CaptureSample();

}  // namespace mtm