#include "health.h"

#include <winioctl.h>
#include <ntddscsi.h>
#include <nvme.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace mtm {

namespace {

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------

bool IsWs(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && IsWs(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && IsWs(s[i])) ++i;
    if (i == 0 && s.size() == s.capacity()) return s;
    return s.substr(i);
}

// Storage descriptors are ANSI (per ATA/SCSI spec), NUL-terminated within
// `size` bytes of `base`. Convert properly instead of zero-extending chars.
std::wstring AnsiAt(const BYTE* base, DWORD size, DWORD offset) {
    if (!offset || offset >= size) return L"";
    const char* s = reinterpret_cast<const char*>(base + offset);
    const int n = static_cast<int>(strnlen(s, size - offset));
    if (n <= 0) return L"";
    int wlen = MultiByteToWideChar(CP_ACP, 0, s, n, nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s, n, w.data(), wlen);
    return Trim(std::move(w));
}

// ---------------------------------------------------------------------------
// Bus / health helpers
// ---------------------------------------------------------------------------

const wchar_t* BusName(STORAGE_BUS_TYPE t) {
    switch (t) {
    case BusTypeNvme: return L"NVMe";
    case BusTypeSata: return L"SATA";
    case BusTypeAta:  return L"ATA";
    case BusTypeUsb:  return L"USB";
    case BusTypeScsi: return L"SCSI";
    case BusTypeSas:  return L"SAS";
    case BusTypeRAID: return L"RAID";
    case BusTypeSd:   return L"SD";
    case BusTypeMmc:  return L"eMMC";
    case BusTypeSpaces: return L"Storage Spaces";
    case BusTypeFileBackedVirtual: return L"Virtual";
    default: return L"Other";
    }
}

// NVMe counters are little-endian 128-bit; the low 64 bits are plenty for
// any real drive. memcpy avoids alignment assumptions.
uint64_t Low64(const UCHAR* p) {
    uint64_t v = 0;
    memcpy(&v, p, sizeof v);
    return v;
}

// What the drive handle was opened with; decides which queries can work.
enum class Access { None, Read, ReadWrite };

void Worst(Health& current, Health h) {
    if (static_cast<int>(h) > static_cast<int>(current)) current = h;
}

// ---------------------------------------------------------------------------
// NVMe health log (log page 0x02)
// ---------------------------------------------------------------------------

bool ReadNvmeHealth(HANDLE h, DriveHealth& d) {
    // Buffer holds: STORAGE_PROPERTY_QUERY + STORAGE_PROTOCOL_SPECIFIC_DATA
    // (the "additional parameters"), plus the NVMe health log payload, plus
    // slack so a future Windows version that grows the descriptor header
    // can't smash the stack.
    constexpr size_t kSlack = 512;
    constexpr size_t kLen =
        offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters) +
        sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) +
        sizeof(NVME_HEALTH_INFO_LOG) +
        kSlack;

    std::vector<BYTE> buf(kLen, 0);
    auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buf.data());
    auto* proto = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
        query->AdditionalParameters);

    query->PropertyId = StorageDeviceProtocolSpecificProperty;
    query->QueryType  = PropertyStandardQuery;

    proto->ProtocolType                = ProtocolTypeNvme;
    proto->DataType                    = NVMeDataTypeLogPage;
    proto->ProtocolDataRequestValue    = NVME_LOG_PAGE_HEALTH_INFO;
    proto->ProtocolDataRequestSubValue = 0;
    proto->ProtocolDataOffset          = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    proto->ProtocolDataLength          = sizeof(NVME_HEALTH_INFO_LOG);

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         buf.data(), static_cast<DWORD>(buf.size()),
                         buf.data(), static_cast<DWORD>(buf.size()),
                         &ret, nullptr))
        return false;

    if (ret < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) return false;

    const auto* desc = reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buf.data());
    // Version is a size-like value today; require equality. Size may grow in
    // future Windows releases, so only require it to be at least the header.
    if (desc->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) return false;
    if (desc->Size    <  sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR)) return false;

    const auto* data = &desc->ProtocolSpecificData;
    const size_t avail = ret - offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    const size_t need  = data->ProtocolDataOffset + sizeof(NVME_HEALTH_INFO_LOG);
    if (need > avail) return false;

    const auto* log = reinterpret_cast<const NVME_HEALTH_INFO_LOG*>(
        reinterpret_cast<const BYTE*>(data) + data->ProtocolDataOffset);

    Health overall = Health::Good;
    d.attributes.reserve(24);

    auto add = [&](const wchar_t* name, std::wstring value,
                   std::wstring detail, Health hl) {
        d.attributes.push_back({name, std::move(value), std::move(detail), hl});
        Worst(overall, hl);
    };

    const UCHAR warn = log->CriticalWarning.AsUchar;
    std::wstring flags;
    if (warn & 0x01) flags += L"spare below threshold; ";
    if (warn & 0x02) flags += L"temperature out of range; ";
    if (warn & 0x04) flags += L"reliability degraded; ";
    if (warn & 0x08) flags += L"read-only mode; ";
    if (warn & 0x10) flags += L"volatile backup failed; ";

    add(L"Critical warning",
        warn ? Format(L"0x%02X", warn) : L"None",
        warn ? flags : L"Controller reports no problems",
        warn ? Health::Bad : Health::Good);

    const int kelvin = log->Temperature[0] | (log->Temperature[1] << 8);
    if (kelvin > 200) {
        d.temperatureC = kelvin - 273;
        add(L"Composite temperature",
            Format(L"%d °C", d.temperatureC),
            L"",
            d.temperatureC >= 70 ? Health::Warning : Health::Good);
    }

    add(L"Available spare",
        Format(L"%u%%", log->AvailableSpare),
        Format(L"Threshold %u%%", log->AvailableSpareThreshold),
        log->AvailableSpare < log->AvailableSpareThreshold ? Health::Bad : Health::Good);

    d.percentUsed = log->PercentageUsed;
    add(L"Percentage used (wear)",
        Format(L"%u%%", log->PercentageUsed),
        L"Vendor estimate of rated endurance consumed",
        log->PercentageUsed >= 100 ? Health::Bad
      : log->PercentageUsed >= 90  ? Health::Warning
                                   : Health::Good);

    const uint64_t unitsRead    = Low64(log->DataUnitRead);
    const uint64_t unitsWritten = Low64(log->DataUnitWritten);
    d.bytesWritten = unitsWritten * 512000ull;   // 1 data unit = 1000 * 512 bytes

    add(L"Data read",
        FormatBytes(unitsRead * 512000ull),
        Format(L"%s data units", FormatCount(unitsRead).c_str()),
        Health::Unknown);
    add(L"Data written",
        FormatBytes(d.bytesWritten),
        Format(L"%s data units", FormatCount(unitsWritten).c_str()),
        Health::Unknown);

    d.powerOnHours = static_cast<int>(
        std::min<uint64_t>(Low64(log->PowerOnHours), INT32_MAX));
    add(L"Power-on hours", FormatCount(Low64(log->PowerOnHours)), L"", Health::Unknown);
    add(L"Power cycles",   FormatCount(Low64(log->PowerCycle)),    L"", Health::Unknown);

    const uint64_t unsafe = Low64(log->UnsafeShutdowns);
    add(L"Unsafe shutdowns", FormatCount(unsafe),
        L"Power lost without a clean shutdown", Health::Unknown);

    const uint64_t media = Low64(log->MediaErrors);
    add(L"Media and data integrity errors", FormatCount(media), L"",
        media ? Health::Warning : Health::Good);

    add(L"Error log entries",
        FormatCount(Low64(log->ErrorInfoLogEntryCount)), L"", Health::Unknown);

    add(L"Controller busy time",
        Format(L"%s min", FormatCount(Low64(log->ControllerBusyTime)).c_str()),
        L"", Health::Unknown);

    add(L"Time above warning temperature",
        Format(L"%lu min", log->WarningCompositeTemperatureTime), L"",
        log->WarningCompositeTemperatureTime ? Health::Warning : Health::Good);

    add(L"Time above critical temperature",
        Format(L"%lu min", log->CriticalCompositeTemperatureTime), L"",
        log->CriticalCompositeTemperatureTime ? Health::Warning : Health::Good);

    d.health = overall;
    return true;
}

// ---------------------------------------------------------------------------
// ATA SMART
// ---------------------------------------------------------------------------

const wchar_t* SmartName(BYTE id) {
    switch (id) {
    case 1:   return L"Read error rate";
    case 5:   return L"Reallocated sectors";
    case 9:   return L"Power-on hours";
    case 10:  return L"Spin retry count";
    case 12:  return L"Power cycles";
    case 170: return L"Available reserved space";
    case 171: return L"Program fail count";
    case 172: return L"Erase fail count";
    case 173: return L"Wear leveling count";
    case 174: return L"Unexpected power loss";
    case 177: return L"Wear range delta";
    case 181: return L"Program fail count (total)";
    case 182: return L"Erase fail count (total)";
    case 184: return L"End-to-end errors";
    case 187: return L"Reported uncorrectable errors";
    case 188: return L"Command timeouts";
    case 190: return L"Airflow temperature";
    case 192: return L"Power-off retract count";
    case 194: return L"Temperature";
    case 195: return L"Hardware ECC recovered";
    case 196: return L"Reallocation events";
    case 197: return L"Pending sectors";
    case 198: return L"Offline uncorrectable";
    case 199: return L"UDMA CRC errors";
    case 231: return L"SSD life left";
    case 232: return L"Endurance remaining";
    case 233: return L"Media wearout indicator";
    case 241: return L"Total LBAs written";
    case 242: return L"Total LBAs read";
    default:  return nullptr;
    }
}

bool SmartCommand(HANDLE h, BYTE feature, BYTE* out512) {
    // On x64 ATA_PASS_THROUGH_EX is 48 bytes, so `data` lands at offset 52 and
    // sizeof(Buffer) is 568 (tail padding). The driver reports
    // offsetof(Buffer, data) + 512 bytes returned, never sizeof(Buffer).
    struct Buffer {
        ATA_PASS_THROUGH_EX apt;
        ULONG               filler;
        BYTE                data[512];
    } b{};

    b.apt.Length             = sizeof(ATA_PASS_THROUGH_EX);
    b.apt.AtaFlags           = ATA_FLAGS_DATA_IN;
    b.apt.DataTransferLength = sizeof b.data;
    b.apt.TimeOutValue       = 5;
    b.apt.DataBufferOffset   = offsetof(Buffer, data);
    b.apt.CurrentTaskFile[0] = feature;   // features
    b.apt.CurrentTaskFile[1] = 1;         // sector count
    b.apt.CurrentTaskFile[2] = 1;         // sector number
    b.apt.CurrentTaskFile[3] = 0x4F;      // cylinder low  (SMART signature)
    b.apt.CurrentTaskFile[4] = 0xC2;      // cylinder high
    b.apt.CurrentTaskFile[5] = 0xA0;      // device/head (master)
    b.apt.CurrentTaskFile[6] = 0xB0;      // SMART command

    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_ATA_PASS_THROUGH,
                         &b, sizeof b, &b, sizeof b, &ret, nullptr))
        return false;
    if (ret < offsetof(Buffer, data) + sizeof b.data) return false;
    if (b.apt.CurrentTaskFile[6] & 0x01) return false;  // ATA error bit
    memcpy(out512, b.data, 512);
    return true;
}

bool ReadAtaSmart(HANDLE h, DriveHealth& d) {
    BYTE values[512]     = {};
    BYTE thresholds[512] = {};
    if (!SmartCommand(h, 0xD0, values)) return false;
    const bool haveThresholds = SmartCommand(h, 0xD1, thresholds);

    // One-pass threshold lookup, replacing the O(n^2) inner scan.
    BYTE thresholdById[256] = {};
    if (haveThresholds) {
        for (int k = 0; k < 30; ++k) {
            const BYTE id = thresholds[2 + k * 12];
            if (id) thresholdById[id] = thresholds[3 + k * 12];
        }
    }

    Health overall = Health::Good;
    d.attributes.reserve(32);

    for (int i = 0; i < 30; ++i) {
        const BYTE* a = values + 2 + i * 12;
        const BYTE id = a[0];
        if (!id) continue;

        const BYTE current = a[3];
        const BYTE worst   = a[4];

        // Raw is 6 bytes little-endian.
        uint64_t raw = 0;
        for (int k = 5; k >= 0; --k) raw = (raw << 8) | a[5 + k];

        const BYTE threshold = thresholdById[id];

        Health hl = Health::Unknown;
        if (threshold && current <= threshold)                                   hl = Health::Bad;
        else if ((id == 5 || id == 196 || id == 197 || id == 198) && raw > 0)    hl = Health::Warning;
        else if (id == 5 || id == 197 || id == 198)                              hl = Health::Good;
        Worst(overall, hl);

        std::wstring value = FormatCount(raw);
        if (id == 194 || id == 190) {
            d.temperatureC = static_cast<int>(raw & 0xFF);
            value = Format(L"%d °C", d.temperatureC);
        }
        if (id == 9) {
            d.powerOnHours = static_cast<int>(raw & 0xFFFFFFFF);
        }

        const wchar_t* known = SmartName(id);
        d.attributes.push_back({
            Format(L"%3u  %s", id, known ? known : L"Vendor specific"),
            std::move(value),
            Format(L"Normalized %u · worst %u%s",
                   current, worst,
                   threshold ? Format(L" · threshold %u", threshold).c_str() : L""),
            hl
        });
    }

    // An empty table counts as "no SMART data", so the caller can still fall
    // back to the predict-failure query. (The summary is set by the caller.)
    if (d.attributes.empty()) return false;
    d.health = overall;
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const wchar_t* HealthName(Health h) {
    switch (h) {
    case Health::Good:    return L"Healthy";
    case Health::Warning: return L"Caution";
    case Health::Bad:     return L"Failing";
    default:              return L"Unknown";
    }
}

std::vector<DriveHealth> QueryDriveHealth() {
    std::vector<DriveHealth> drives;
    drives.reserve(8);

    for (int n = 0; n < 32; ++n) {
        const std::wstring path = Format(L"\\\\.\\PhysicalDrive%d", n);

        // Three-tier open: read+write, then read-only, then metadata-only.
        // Only IOCTL_ATA_PASS_THROUGH needs read+write; the property queries
        // (identity, temperature, NVMe health log) and predict-failure are
        // FILE_ANY_ACCESS and work on any handle.
        const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        Access access = Access::ReadWrite;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               share, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            access = Access::Read;
            h = CreateFileW(path.c_str(), GENERIC_READ,
                            share, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) {
            access = Access::None;
            h = CreateFileW(path.c_str(), 0,
                            share, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        if (h == INVALID_HANDLE_VALUE) continue;

        DriveHealth d;
        d.number = n;

        // ---- Identity --------------------------------------------------
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType  = PropertyStandardQuery;

        std::vector<BYTE> buf(4096, 0);
        DWORD ret = 0;
        STORAGE_BUS_TYPE bus = BusTypeUnknown;

        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &q, sizeof q,
                            buf.data(), static_cast<DWORD>(buf.size()),
                            &ret, nullptr) &&
            ret >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {

            const auto* desc = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
            const std::wstring vendor  = AnsiAt(buf.data(), ret, desc->VendorIdOffset);
            const std::wstring product = AnsiAt(buf.data(), ret, desc->ProductIdOffset);

            if (vendor.empty() || product.rfind(vendor, 0) == 0)
                d.model = product;
            else
                d.model = vendor + L" " + product;
            d.model    = Trim(std::move(d.model));
            d.firmware = AnsiAt(buf.data(), ret, desc->ProductRevisionOffset);
            d.serial   = AnsiAt(buf.data(), ret, desc->SerialNumberOffset);
            bus        = desc->BusType;
            d.bus      = BusName(bus);
        }

        // ---- Size ------------------------------------------------------
        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                            nullptr, 0, &geo, sizeof geo, &ret, nullptr))
            d.size = static_cast<uint64_t>(geo.DiskSize.QuadPart);

        // ---- Media kind (hint only) -----------------------------------
        q.PropertyId = StorageDeviceSeekPenaltyProperty;
        DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &q, sizeof q, &seek, sizeof seek, &ret, nullptr))
            d.kind = seek.IncursSeekPenalty ? L"HDD" : L"SSD";

        // ---- Temperature (bus-agnostic, tried before SMART) -----------
        q.PropertyId = StorageDeviceTemperatureProperty;
        std::vector<BYTE> tbuf(
            sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) +
            8 * sizeof(STORAGE_TEMPERATURE_INFO), 0);
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                            &q, sizeof q,
                            tbuf.data(), static_cast<DWORD>(tbuf.size()),
                            &ret, nullptr)) {
            const auto* t = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(tbuf.data());
            if (t->InfoCount > 0 && t->TemperatureInfo[0].Temperature > -100)
                d.temperatureC = t->TemperatureInfo[0].Temperature;
        }

        // ---- Health ----------------------------------------------------
        const bool ataLike = bus == BusTypeSata || bus == BusTypeAta || bus == BusTypeUsb;
        bool gotHealth = false;
        if (bus == BusTypeNvme) {
            gotHealth = ReadNvmeHealth(h, d);
        } else if (ataLike && access == Access::ReadWrite) {
            gotHealth = ReadAtaSmart(h, d);
        }

        if (!gotHealth) {
            // Drive may still support the older predict-failure ioctl.
            STORAGE_PREDICT_FAILURE pf{};
            if (DeviceIoControl(h, IOCTL_STORAGE_PREDICT_FAILURE,
                                nullptr, 0, &pf, sizeof pf, &ret, nullptr)) {
                d.health = pf.PredictFailure ? Health::Bad : Health::Good;
                d.attributes.push_back({
                    L"SMART failure prediction",
                    pf.PredictFailure ? L"Failure predicted" : L"OK",
                    L"Reported by the drive",
                    d.health
                });
                gotHealth = true;
            }
        }

        // ---- Summary ---------------------------------------------------
        // "Run as administrator" only when elevation would actually help: ATA
        // SMART needs a read+write handle, and some NVMe drivers refuse the
        // health log on a handle opened without any access.
        const bool needsAdmin = (ataLike && access != Access::ReadWrite) ||
                                (bus == BusTypeNvme && access == Access::None);
        if (d.health == Health::Unknown)
            d.summary = needsAdmin
                ? L"Run as administrator to read health data."
                : L"This drive doesn't expose health data through Windows.";
        else if (d.health == Health::Good)
            d.summary = L"No problems reported.";
        else if (d.health == Health::Warning)
            d.summary = L"Some counters need attention; back up important data.";
        else
            d.summary = L"The drive reports a failure condition. Back up now.";

        CloseHandle(h);
        drives.push_back(std::move(d));
    }
    return drives;
}

}  // namespace mtm