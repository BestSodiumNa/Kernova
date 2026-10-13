#include "recorder.h"

#include "processes.h"
#include "sysinfo.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <type_traits>

namespace mtm {

FlightRecorder g_recorder;

namespace {

// ---------------------------------------------------------------------------
// File layout
//   offset  size  field
//   0       8     magic "MTMTRACE"
//   8       4     version (u32, little-endian)
//   12      4     reserved (u32, must be 0)
//   16      ...   records: [u32 length][u32 crc32c][payload]
// ---------------------------------------------------------------------------
constexpr std::array<char, 8> kMagic = {'M','T','M','T','R','A','C','E'};
constexpr uint32_t kVersion        = 1;
constexpr size_t   kHeaderBytes    = 16;
constexpr uint64_t kMaxFileBytes   = 1ull << 30;   // 1 GiB
static_assert(kMaxFileBytes <= 0xFFFFFFFFull, "ReadFile size cast assumes <= 4 GiB");

// Bounds shared by the encoder and the decoder, so they can never disagree.
constexpr size_t kMaxCores = 0xFFFF;
constexpr size_t kMaxProcs = 32;

// ---------------------------------------------------------------------------
// CRC32C (Castagnoli), reflected, poly 0x82F63B78. Table generated at compile time.
// ---------------------------------------------------------------------------
struct Crc32cTable {
    std::array<uint32_t, 256> t{};
    constexpr Crc32cTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
constexpr Crc32cTable kCrcTable{};

uint32_t Crc32c(const uint8_t* p, size_t n, uint32_t seed = 0) {
    uint32_t c = ~seed;
    for (size_t i = 0; i < n; ++i)
        c = kCrcTable.t[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
class Writer {
public:
    template <typename T>
    void Put(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }

    void Str(const std::wstring& s) {
        const uint16_t n = static_cast<uint16_t>(
            std::min(s.size(), static_cast<size_t>(0xFFFF)));
        Put(n);
        if (n) {
            const auto* p = reinterpret_cast<const uint8_t*>(s.data());
            bytes.insert(bytes.end(), p, p + n * sizeof(wchar_t));
        }
    }

    std::vector<uint8_t> bytes;
};

// ---------------------------------------------------------------------------
// Reader — bounds-checked, alignment-safe.
// ---------------------------------------------------------------------------
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), end_(p + n) {}

    template <typename T>
    T Get() {
        static_assert(std::is_trivially_copyable_v<T>);
        T v{};
        if (!ok || static_cast<size_t>(end_ - p_) < sizeof(T)) { ok = false; return v; }
        memcpy(&v, p_, sizeof(T));       // alignment-safe
        p_ += sizeof(T);
        return v;
    }

    std::wstring Str() {
        const uint16_t n = Get<uint16_t>();
        if (!ok || static_cast<size_t>(end_ - p_) < static_cast<size_t>(n) * sizeof(wchar_t)) {
            ok = false;
            return L"";
        }
        std::wstring s(n, L'\0');
        if (n) memcpy(s.data(), p_, static_cast<size_t>(n) * sizeof(wchar_t));
        p_ += static_cast<size_t>(n) * sizeof(wchar_t);
        return s;
    }

    bool ok = true;
    size_t Remaining() const { return static_cast<size_t>(end_ - p_); }

private:
    const uint8_t* p_;
    const uint8_t* end_;
};

// ---------------------------------------------------------------------------
// Encode / Decode
// ---------------------------------------------------------------------------
std::vector<uint8_t> Encode(const TraceSample& s) {
    Writer w;
    w.Put(s.time);
    w.Put(s.monotonic);
    w.Put(s.cpu);
    w.Put(s.gpu);
    w.Put(s.diskActive);
    for (uint64_t v : {s.memUsed, s.memTotal, s.diskRead, s.diskWrite,
                       s.netIn, s.netOut, s.gpuMemory})
        w.Put(v);

    const uint16_t cores = static_cast<uint16_t>(
        std::min(std::min(s.coreUsage.size(), s.coreMhz.size()), kMaxCores));
    w.Put(cores);
    for (uint16_t i = 0; i < cores; ++i) {
        w.Put(s.coreUsage[i]);
        w.Put(s.coreMhz[i]);
    }

    const uint8_t procs = static_cast<uint8_t>(
        std::min(s.top.size(), kMaxProcs));
    w.Put(procs);
    for (uint8_t i = 0; i < procs; ++i) {
        w.Put(s.top[i].pid);
        w.Put(s.top[i].cpu);
        w.Put(s.top[i].memory);
        w.Str(s.top[i].name);
    }
    return w.bytes;
}

bool Decode(const uint8_t* p, size_t n, TraceSample& s) {
    // Reset so appends are safe even if the caller reuses `s`.
    s = TraceSample{};

    Reader r(p, n);
    s.time       = r.Get<uint64_t>();
    s.monotonic  = r.Get<uint64_t>();
    s.cpu        = r.Get<float>();
    s.gpu        = r.Get<float>();
    s.diskActive = r.Get<float>();
    for (uint64_t* v : {&s.memUsed, &s.memTotal, &s.diskRead, &s.diskWrite,
                        &s.netIn, &s.netOut, &s.gpuMemory})
        *v = r.Get<uint64_t>();

    if (!r.ok) return false;   // header was short

    const uint16_t cores = r.Get<uint16_t>();
    if (!r.ok || cores > kMaxCores) return false;
    s.coreUsage.reserve(cores);
    s.coreMhz.reserve(cores);
    for (uint16_t i = 0; i < cores && r.ok; ++i) {
        s.coreUsage.push_back(r.Get<float>());
        s.coreMhz.push_back(r.Get<uint16_t>());
    }
    if (!r.ok) return false;

    const uint8_t procs = r.Get<uint8_t>();
    if (!r.ok || procs > kMaxProcs) return false;
    s.top.reserve(procs);
    for (uint8_t i = 0; i < procs && r.ok; ++i) {
        TraceProcess t;
        t.pid    = r.Get<uint32_t>();
        t.cpu    = r.Get<float>();
        t.memory = r.Get<uint64_t>();
        t.name   = r.Str();
        s.top.push_back(std::move(t));
    }
    return r.ok;
}

// ---------------------------------------------------------------------------
// Low-level file helpers
// ---------------------------------------------------------------------------
bool WriteAll(HANDLE h, const void* data, size_t n) {
    // Chunked so `n` may exceed DWORD_MAX without truncation. The chunk size
    // is deliberately 1 MiB: large enough to keep syscall overhead low, small
    // enough that a stalled write is observable within a reasonable window.
    const auto* p = static_cast<const uint8_t*>(data);
    while (n > 0) {
        const DWORD chunk = static_cast<DWORD>(
            std::min(n, static_cast<size_t>(1u << 20)));
        DWORD done = 0;
        if (!WriteFile(h, p, chunk, &done, nullptr) || done != chunk)
            return false;
        p += chunk;
        n -= chunk;
    }
    return true;
}

bool WriteHeader(HANDLE h) {
    uint8_t hdr[kHeaderBytes] = {};
    memcpy(hdr, kMagic.data(), kMagic.size());
    memcpy(hdr + 8, &kVersion, sizeof kVersion);
    // hdr[12..15] reserved = 0
    return WriteAll(h, hdr, sizeof hdr);
}

bool WriteRecord(HANDLE h, const TraceSample& s) {
    const auto payload = Encode(s);
    const uint32_t len = static_cast<uint32_t>(payload.size());
    const uint32_t crc = Crc32c(payload.data(), payload.size());
    return WriteAll(h, &len, sizeof len)
        && WriteAll(h, &crc, sizeof crc)
        && WriteAll(h, payload.data(), payload.size());
}

HANDLE CreateTraceFile(const std::wstring& path, std::wstring& error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                           nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = ErrorText(GetLastError());
        return h;
    }
    if (!WriteHeader(h)) {
        error = ErrorText(GetLastError());
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

}  // namespace

// ---------------------------------------------------------------------------
// FlightRecorder — public API
// ---------------------------------------------------------------------------
void FlightRecorder::Add(const TraceSample& s) {
    {
        std::lock_guard lk(mu_);
        buffer_.push_back(s);
        while (buffer_.size() > kBufferSize) buffer_.pop_front();
    }
    if (Recording()) EnqueueForWrite(s);
}

std::vector<TraceSample> FlightRecorder::Snapshot() const {
    std::lock_guard lk(mu_);
    return {buffer_.begin(), buffer_.end()};
}

bool FlightRecorder::Start(const std::wstring& path, std::wstring& error) {
    Stop();

    HANDLE h = CreateTraceFile(path, error);
    if (h == INVALID_HANDLE_VALUE) return false;

    // Without the event, WaitForSingleObject(nullptr, ...) returns at once and
    // the writer loop would spin at 100% CPU.
    writerWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!writerWake_) {
        error = ErrorText(GetLastError());
        CloseHandle(h);
        return false;
    }

    {
        std::lock_guard lk(mu_);
        file_      = h;
        path_      = path;
        lastError_.clear();
        written_   = 0;
        writeQueue_.clear();
        writeHead_ = 0;
    }
    writerStop_.store(false);
    writer_ = std::thread([this]{ WriterLoop(); });
    return true;
}

void FlightRecorder::Stop() {
    if (writer_.joinable()) {
        writerStop_.store(true);
        if (writerWake_) SetEvent(writerWake_);
        writer_.join();
    }
    if (writerWake_) { CloseHandle(writerWake_); writerWake_ = nullptr; }
    FlushAndClose();
}

void FlightRecorder::FlushAndClose() {
    std::lock_guard lk(mu_);
    if (file_ != INVALID_HANDLE_VALUE) {
        FlushFileBuffers(file_);
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
}

void FlightRecorder::EnqueueForWrite(const TraceSample& s) {
    std::lock_guard lk(mu_);
    if (file_ == INVALID_HANDLE_VALUE) return;

    // Bounded queue: if the writer falls behind, drop oldest.
    constexpr size_t kQueueCap = 256;
    if (writeQueue_.size() - writeHead_ >= kQueueCap) {
        ++writeHead_;
        if (writeHead_ > 64 && writeHead_ * 2 > writeQueue_.size()) {
            writeQueue_.erase(writeQueue_.begin(),
                              writeQueue_.begin() + writeHead_);
            writeHead_ = 0;
        }
    }
    writeQueue_.push_back(s);
    if (writerWake_) SetEvent(writerWake_);
}

void FlightRecorder::WriterLoop() {
    for (;;) {
        TraceSample s;
        bool have = false;

        {
            std::lock_guard lk(mu_);
            if (writeHead_ < writeQueue_.size()) {
                s = std::move(writeQueue_[writeHead_++]);
                have = true;
                if (writeHead_ > 64 && writeHead_ * 2 > writeQueue_.size()) {
                    writeQueue_.erase(writeQueue_.begin(),
                                      writeQueue_.begin() + writeHead_);
                    writeHead_ = 0;
                }
            }
        }

        if (!have) {
            if (writerStop_.load()) return;
            WaitForSingleObject(writerWake_, 100);
            continue;
        }

        HANDLE h;
        {
            std::lock_guard lk(mu_);
            h = file_;
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        if (!WriteRecord(h, s)) {
            // Disk full, removed, or I/O error: stop recording cleanly and keep
            // the reason so the UI can tell the user why it stopped.
            const DWORD err = GetLastError();
            {
                std::lock_guard lk(mu_);
                lastError_ = L"Recording stopped: " + ErrorText(err);
            }
            FlushAndClose();
            return;
        }
        ++written_;
    }
}

// ---------------------------------------------------------------------------
// Save / Load
// ---------------------------------------------------------------------------
bool FlightRecorder::Save(const std::wstring& path,
                          const std::vector<TraceSample>& samples,
                          std::wstring& error) {
    HANDLE h = CreateTraceFile(path, error);
    if (h == INVALID_HANDLE_VALUE) return false;

    for (const auto& s : samples) {
        if (!WriteRecord(h, s)) {
            error = ErrorText(GetLastError());
            CloseHandle(h);
            return false;
        }
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    return true;
}

bool FlightRecorder::Load(const std::wstring& path,
                          std::vector<TraceSample>& out,
                          std::wstring& error) {
    out.clear();

    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = ErrorText(GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < static_cast<LONGLONG>(kHeaderBytes)) {
        CloseHandle(h);
        error = L"The file is empty or not a trace.";
        return false;
    }
    if (static_cast<uint64_t>(size.QuadPart) > kMaxFileBytes) {
        CloseHandle(h);
        error = L"The trace is larger than 1 GB.";
        return false;
    }

    HANDLE mapping = CreateFileMappingW(h, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) {
        error = ErrorText(GetLastError());
        CloseHandle(h);
        return false;
    }
    const uint8_t* base = static_cast<const uint8_t*>(
        MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    if (!base) {
        error = ErrorText(GetLastError());
        CloseHandle(mapping);
        CloseHandle(h);
        return false;
    }

    bool ok = true;
    do {
        if (memcmp(base, kMagic.data(), kMagic.size()) != 0) {
            error = L"This isn't a Kernova trace (.mtmtrace) file.";
            ok = false;
            break;
        }
        uint32_t version = 0;
        memcpy(&version, base + 8, sizeof version);
        if (version != kVersion) {
            error = L"Unsupported trace version.";
            ok = false;
            break;
        }

        const size_t total = static_cast<size_t>(size.QuadPart);
        size_t pos = kHeaderBytes;
        while (pos + 8 <= total) {
            uint32_t len = 0, crc = 0;
            memcpy(&len, base + pos, sizeof len);
            memcpy(&crc, base + pos + 4, sizeof crc);
            pos += 8;

            if (len == 0 || len > total - pos) {
                // Truncated final record — accept what we have.
                break;
            }
            const uint32_t actual = Crc32c(base + pos, len);
            if (actual != crc) {
                error = L"Trace is corrupt at offset " + std::to_wstring(pos);
                ok = false;
                break;
            }
            TraceSample s;
            if (!Decode(base + pos, len, s)) {
                error = L"Trace is corrupt at offset " + std::to_wstring(pos);
                ok = false;
                break;
            }
            out.push_back(std::move(s));
            pos += len;
        }
    } while (false);

    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(h);

    if (!ok) { out.clear(); return false; }
    if (out.empty()) {
        error = L"The trace doesn't contain any samples.";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
TraceSample CaptureSample() {
    const SystemSnapshot& sys = Sys();
    TraceSample s;

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    s.time      = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    s.monotonic = GetTickCount64();

    s.cpu        = float(sys.cpu);
    s.gpu        = float(sys.gpu);
    s.diskActive = float(sys.diskActive);
    s.memTotal   = sys.memTotal;
    s.memUsed    = sys.memTotal - sys.memAvail;
    s.diskRead   = static_cast<uint64_t>(sys.diskRead);
    s.diskWrite  = static_cast<uint64_t>(sys.diskWrite);
    s.netIn      = static_cast<uint64_t>(sys.netIn);
    s.netOut     = static_cast<uint64_t>(sys.netOut);
    s.gpuMemory  = sys.gpuDedicated;

    s.coreUsage.reserve(sys.cores.size());
    s.coreMhz.reserve(sys.cores.size());
    for (const CoreInfo& c : sys.cores) {
        s.coreUsage.push_back(float(c.usage));
        s.coreMhz.push_back(static_cast<uint16_t>(
            std::min<ULONG>(c.currentMhz, 65535)));
    }

    std::vector<const ProcInfo*> procs;
    procs.reserve(g_procs.Processes().size());
    for (const ProcInfo& p : g_procs.Processes()) procs.push_back(&p);

    const size_t keep = std::min(procs.size(), kMaxProcs);
    std::partial_sort(procs.begin(),
                      procs.begin() + static_cast<std::ptrdiff_t>(keep),
                      procs.end(),
                      [](const ProcInfo* a, const ProcInfo* b) {
                          return a->cpu > b->cpu;
                      });
    s.top.reserve(keep);
    for (size_t i = 0; i < keep; ++i)
        s.top.push_back({procs[i]->pid,
                         float(procs[i]->cpu),
                         procs[i]->privateWs,
                         procs[i]->name});
    return s;
}

}  // namespace mtm