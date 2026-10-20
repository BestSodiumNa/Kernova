#include "razer.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace mtm {
namespace {

constexpr USHORT kRazerVid = 0x1532;
constexpr int kReportLen = 91;  // report id + 90-byte packet

std::mutex g_razerLock;  // one command in flight per process: the mouse answers into a single buffer

// Packet: [0] status, [1] transaction id, [2..3] remaining, [4] protocol, [5] data size,
// [6] command class, [7] command id, [8..87] arguments, [88] XOR of bytes 2..87.
// (Offsets are +1 in the buffers below, after the report id.)
struct Reply {
    DWORD error = ERROR_GEN_FAILURE;
    BYTE args[80] = {};
};

// The transaction id is part of every packet, and which one a mouse accepts depends on the model:
// 0x1F on newer ones (the DeathAdder V3 Pro, tested), 0x3F or 0xFF on some older ones such as
// early Vipers (per the OpenRazer project; not tested on those here). So it is found once per
// mouse by asking for the firmware version, then remembered by device path.
constexpr BYTE kTransactionIds[] = {0x1F, 0x3F, 0xFF};
std::map<std::wstring, BYTE> g_transaction;  // guarded by g_razerLock

BYTE KnownTransaction(const std::wstring& path) {
    std::lock_guard lock(g_razerLock);
    const auto it = g_transaction.find(path);
    return it == g_transaction.end() ? kTransactionIds[0] : it->second;
}

// `transaction` < 0 = the one remembered for this mouse.
Reply Command(const RazerMouse& m, BYTE cls, BYTE id, BYTE size, const std::vector<BYTE>& args = {},
              int transaction = -1) {
    const BYTE txn = transaction >= 0 ? static_cast<BYTE>(transaction) : KnownTransaction(m.path);
    std::lock_guard lock(g_razerLock);
    Reply out;
    // Zero access is enough for feature reports, and works while Windows (and Synapse) use the mouse.
    HANDLE h = CreateFileW(m.path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out.error = GetLastError();
        return out;
    }
    BYTE req[kReportLen] = {};
    req[2] = txn;
    req[6] = size;
    req[7] = cls;
    req[8] = id;
    size_t i = 9;
    for (BYTE a : args)
        if (i < 89) req[i++] = a;
    BYTE crc = 0;
    for (int k = 3; k < 89; ++k) crc ^= req[k];
    req[89] = crc;
    if (!HidD_SetFeature(h, req, kReportLen)) {
        out.error = GetLastError();
        CloseHandle(h);
        return out;
    }
    // Wireless mice answer through the dongle a few ms later; poll until it's not "busy".
    for (int tries = 0; tries < 15; ++tries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(tries ? 25 : 15));
        BYTE rep[kReportLen] = {};
        if (!HidD_GetFeature(h, rep, kReportLen)) {
            out.error = GetLastError();
            break;
        }
        if (rep[1] == 0x01) continue;  // busy
        if (rep[7] != cls || rep[8] != id) continue;  // an answer to something else
        out.error = rep[1] == 0x02 ? ERROR_SUCCESS : rep[1] == 0x05 ? ERROR_NOT_SUPPORTED : rep[1] == 0x04 ? ERROR_TIMEOUT : ERROR_GEN_FAILURE;
        memcpy(out.args, rep + 9, sizeof out.args);
        break;
    }
    CloseHandle(h);
    return out;
}

// Polling-rate codes: the classic command (125-1000 Hz) and the newer one (up to 8000 Hz).
constexpr std::pair<int, BYTE> kClassicRates[] = {{1000, 0x01}, {500, 0x02}, {125, 0x08}};
constexpr std::pair<int, BYTE> kExtendedRates[] = {{8000, 0x01}, {4000, 0x02}, {2000, 0x04}, {1000, 0x08}, {500, 0x10}, {125, 0x40}};

// Asks for the firmware version with each transaction id in turn (the remembered one first) and
// remembers the one that is answered. False when none is: asleep, out of range, or not a protocol mouse.
bool Identify(const RazerMouse& m, Reply& firmware) {
    std::vector<BYTE> order = {KnownTransaction(m.path)};
    for (BYTE t : kTransactionIds)
        if (t != order[0]) order.push_back(t);
    for (BYTE t : order) {
        firmware = Command(m, 0x00, 0x81, 0x02, {}, t);
        if (firmware.error != ERROR_SUCCESS) continue;
        std::lock_guard lock(g_razerLock);
        g_transaction[m.path] = t;
        return true;
    }
    return false;
}

}  // namespace

std::vector<RazerMouse> FindRazerMice() {
    std::vector<RazerMouse> out;
    HDEVINFO set = SetupDiGetClassDevsW(&kHidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return out;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kHidInterface, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        HANDLE f = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        HIDD_ATTRIBUTES attr{sizeof attr};
        HIDP_CAPS caps{};
        wchar_t product[128] = {};
        if (HidD_GetAttributes(f, &attr) && attr.VendorID == kRazerVid) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(f, &pp)) {
                HidP_GetCaps(pp, &caps);
                HidD_FreePreparsedData(pp);
            }
            HidD_GetProductString(f, product, sizeof product);
        }
        CloseHandle(f);
        // The mouse collection that carries the control channel; one per device.
        if (attr.VendorID != kRazerVid || caps.UsagePage != 0x01 || caps.Usage != 0x02 || caps.FeatureReportByteLength != kReportLen)
            continue;
        if (std::any_of(out.begin(), out.end(), [&](const RazerMouse& m) { return m.pid == attr.ProductID; })) continue;
        out.push_back({*product ? product : Format(L"Razer mouse %04X", attr.ProductID), detail->DevicePath, attr.ProductID});
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

RazerState ReadRazer(const RazerMouse& m) {
    RazerState s;
    if (Reply fw; Identify(m, fw)) {
        s.ok = true;
        s.firmware = Format(L"%u.%u", fw.args[0], fw.args[1]);
    }
    if (!s.ok) return s;  // asleep, out of range or not a protocol mouse
    const Reply battery = Command(m, 0x07, 0x80, 0x02);
    const Reply idle = Command(m, 0x07, 0x83, 0x02);
    // Wired-only mice refuse the battery command, or answer 0 and have no sleep timer either.
    s.wired = battery.error != ERROR_SUCCESS || (idle.error != ERROR_SUCCESS && battery.args[1] == 0);
    if (!s.wired) {
        s.battery = (battery.args[1] * 100 + 127) / 255;
        if (idle.error == ERROR_SUCCESS) s.sleepSeconds = (idle.args[0] << 8) | idle.args[1];
        if (const Reply r = Command(m, 0x07, 0x84, 0x02); r.error == ERROR_SUCCESS) s.charging = r.args[1] != 0;
    }
    if (const Reply r = Command(m, 0x00, 0xC0, 0x01); r.error == ERROR_SUCCESS) {
        for (const auto& [hz, code] : kExtendedRates)
            if (r.args[1] == code) s.pollHz = hz, s.pollExtended = true;
    }
    if (!s.pollExtended)
        if (const Reply r = Command(m, 0x00, 0x85, 0x01); r.error == ERROR_SUCCESS)
            for (const auto& [hz, code] : kClassicRates)
                if (r.args[0] == code) s.pollHz = hz;
    if (const Reply r = Command(m, 0x04, 0x85, 0x07, {0x01}); r.error == ERROR_SUCCESS) {
        s.dpiX = (r.args[1] << 8) | r.args[2];
        s.dpiY = (r.args[3] << 8) | r.args[4];
    }
    // Stages: [store, active id, count, then 7 bytes each: id, x hi, x lo, y hi, y lo, 0, 0].
    if (const Reply r = Command(m, 0x04, 0x86, 0x26, {0x01}); r.error == ERROR_SUCCESS) {
        const int count = std::min<int>(r.args[2], 5);
        for (int i = 0; i < count; ++i) {
            const BYTE* st = r.args + 3 + i * 7;
            s.stages.push_back((st[1] << 8) | st[2]);
            if (st[0] == r.args[1]) s.activeStage = i;
        }
    }
    return s;
}

DWORD RazerSetDpi(const RazerMouse& m, int dpi) {
    if (dpi < 100 || dpi > 30000) return ERROR_INVALID_PARAMETER;
    const BYTE hi = BYTE(dpi >> 8), lo = BYTE(dpi);
    return Command(m, 0x04, 0x05, 0x07, {0x01, hi, lo, hi, lo, 0, 0}).error;  // 0x01: kept in the mouse
}

DWORD RazerSetActiveStage(const RazerMouse& m, const RazerState& s, int stage) {
    if (stage < 0 || stage >= int(s.stages.size())) return ERROR_INVALID_PARAMETER;
    // The stage table is written back unchanged; only the active id differs.
    BYTE args[3 + 5 * 7] = {0x01, BYTE(stage + 1), BYTE(s.stages.size())};
    for (size_t i = 0; i < s.stages.size(); ++i) {
        BYTE* st = args + 3 + i * 7;
        st[0] = BYTE(i + 1);
        st[1] = st[3] = BYTE(s.stages[i] >> 8);
        st[2] = st[4] = BYTE(s.stages[i]);
    }
    const DWORD r = Command(m, 0x04, 0x06, 0x26, std::vector<BYTE>(args, args + 3 + s.stages.size() * 7)).error;
    return r == ERROR_SUCCESS ? RazerSetDpi(m, s.stages[size_t(stage)]) : r;  // and switch to it now
}

DWORD RazerSetPolling(const RazerMouse& m, bool extended, int hz) {
    if (extended) {
        for (const auto& [rate, code] : kExtendedRates)
            if (rate == hz) return Command(m, 0x00, 0x40, 0x02, {0x00, code}).error;
    } else {
        for (const auto& [rate, code] : kClassicRates)
            if (rate == hz) return Command(m, 0x00, 0x05, 0x01, {code}).error;
    }
    return ERROR_INVALID_PARAMETER;
}

std::vector<int> RazerPollingChoices(bool extended) {
    return extended ? std::vector<int>{8000, 4000, 2000, 1000, 500, 125} : std::vector<int>{1000, 500, 125};
}

}  // namespace mtm
