#include "mice.h"

#include <hidsdi.h>
#include <setupapi.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

// Logitech mice over HID++ 2.0, Logitech's documented device protocol (the one G HUB,
// Options+ and Solaar use). Written from the protocol description; no Logitech hardware
// was available to test it, so the page labels it as untested.
//
// A request is a 20-byte "long" report: [0x11, device index, feature index, function << 4 |
// software id, 16 parameter bytes]. The answer comes back as an input report with the same
// first four bytes; an error answer has 0xFF as its feature index. Feature indexes differ per
// device and are looked up through the root feature (index 0), function 0 getFeature(id).

namespace mtm {
namespace {

constexpr USHORT kLogitechVid = 0x046D;
constexpr BYTE kLongReport = 0x11, kSoftwareId = 0x0B, kDirect = 0xFF;  // 0xFF: a device plugged in directly

constexpr uint16_t kDeviceName = 0x0005, kBatteryStatus = 0x1000, kBatteryVoltage = 0x1001, kUnifiedBattery = 0x1004,
                   kAdjustableDpi = 0x2201, kReportRate = 0x8060, kExtendedReportRate = 0x8061;

using Params = std::array<BYTE, 16>;

// One open HID++ channel (the device's vendor "long report" collection).
class Channel {
public:
    explicit Channel(const std::wstring& path) {
        h_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, nullptr);
        ov_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~Channel() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        CloseHandle(ov_.hEvent);
    }
    bool Ok() const { return h_ != INVALID_HANDLE_VALUE; }

    // Sends one request and waits for its answer. ERROR_TIMEOUT when nothing answers (asleep,
    // switched off, or an empty receiver slot).
    DWORD Call(BYTE dev, BYTE feature, BYTE function, std::initializer_list<BYTE> params, Params* out = nullptr, int timeoutMs = 600) {
        BYTE req[20] = {kLongReport, dev, feature, BYTE(function << 4 | kSoftwareId)};
        size_t i = 4;
        for (BYTE p : params)
            if (i < sizeof req) req[i++] = p;
        if (!Transfer(true, req, 1000)) return ERROR_WRITE_FAULT;
        const ULONGLONG deadline = GetTickCount64() + ULONGLONG(timeoutMs);
        while (GetTickCount64() < deadline) {
            BYTE rep[20] = {};
            if (!Transfer(false, rep, int(deadline - GetTickCount64()))) break;
            if (rep[0] != kLongReport || rep[1] != dev) continue;  // a notification, or another device's answer
            if (rep[2] == feature && rep[3] == req[3]) {
                if (out) std::copy(rep + 4, rep + 20, out->begin());
                return ERROR_SUCCESS;
            }
            if (rep[2] == 0xFF && rep[3] == feature && rep[4] == req[3]) {  // HID++ 2.0 error
                switch (rep[5]) {
                    case 2: case 3: return ERROR_INVALID_PARAMETER;  // invalid argument, out of range
                    case 6: case 7: case 9: return ERROR_NOT_SUPPORTED;  // bad feature / function, unsupported
                    case 8: return ERROR_BUSY;
                    default: return ERROR_GEN_FAILURE;
                }
            }
        }
        return ERROR_TIMEOUT;
    }

    // Index of a feature on this device, 0 when it doesn't have it (or didn't answer).
    BYTE Feature(BYTE dev, uint16_t id, int timeoutMs = 600) {
        const auto key = std::make_pair(dev, id);
        if (auto it = features_.find(key); it != features_.end()) return it->second;
        Params p{};
        const BYTE index = Call(dev, 0x00, 0, {BYTE(id >> 8), BYTE(id)}, &p, timeoutMs) == ERROR_SUCCESS ? p[0] : 0;
        return features_[key] = index;
    }

private:
    bool Transfer(bool write, BYTE* buf, int timeoutMs) {
        ResetEvent(ov_.hEvent);
        DWORD n = 0;
        const BOOL done = write ? WriteFile(h_, buf, 20, nullptr, &ov_) : ReadFile(h_, buf, 20, nullptr, &ov_);
        if (!done && GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(ov_.hEvent, DWORD(std::max(1, timeoutMs))) != WAIT_OBJECT_0) {
            CancelIoEx(h_, &ov_);
            GetOverlappedResult(h_, &ov_, &n, TRUE);
            return false;
        }
        return GetOverlappedResult(h_, &ov_, &n, FALSE) && n == 20;
    }
    HANDLE h_ = INVALID_HANDLE_VALUE;
    OVERLAPPED ov_{};
    std::map<std::pair<BYTE, uint16_t>, BYTE> features_;
};

std::wstring ReadName(Channel& c, BYTE dev) {
    const BYTE f = c.Feature(dev, kDeviceName);
    Params p{};
    if (!f || c.Call(dev, f, 0, {}, &p) != ERROR_SUCCESS) return L"";
    const int length = std::min<int>(p[0], 64);
    std::string name;
    while (int(name.size()) < length) {
        if (c.Call(dev, f, 1, {BYTE(name.size())}, &p) != ERROR_SUCCESS) break;
        for (BYTE ch : p)
            if (int(name.size()) < length && ch) name += char(ch);
        if (std::find(p.begin(), p.end(), 0) != p.end()) break;
    }
    std::wstring out(name.begin(), name.end());  // device names are plain ASCII
    return out.rfind(L"Logitech", 0) == 0 ? out : L"Logitech " + out;
}

// 1 = mouse-like (mouse, trackpad, trackball), 0 = something else, -1 = didn't say.
int IsPointer(Channel& c, BYTE dev) {
    const BYTE f = c.Feature(dev, kDeviceName);
    Params p{};
    if (!f || c.Call(dev, f, 2, {}, &p) != ERROR_SUCCESS) return -1;
    return p[0] == 3 || p[0] == 4 || p[0] == 5 ? 1 : 0;
}

class LogitechMouse final : public VendorMouse {
public:
    LogitechMouse(std::wstring path, BYTE dev, std::wstring name) : path_(std::move(path)), dev_(dev), name_(std::move(name)) {}
    std::wstring Name() const override { return name_; }
    std::wstring Brand() const override { return L"Logitech"; }
    bool Tested() const override { return false; }

    MouseState Read() override {
        MouseState s;
        Channel c(path_);
        if (!c.Ok()) return s;
        Params p{};
        // Battery, from whichever of the three battery features the mouse has.
        if (const BYTE f = c.Feature(dev_, kUnifiedBattery); f && c.Call(dev_, f, 1, {}, &p) == ERROR_SUCCESS) {
            s.ok = true;
            s.battery = p[0];
            s.charging = p[2] >= 1 && p[2] <= 3;  // charging, slow charging, full on the charger
        } else if (const BYTE f2 = c.Feature(dev_, kBatteryStatus); f2 && c.Call(dev_, f2, 0, {}, &p) == ERROR_SUCCESS) {
            s.ok = true;
            s.battery = p[0];
            s.charging = p[2] == 1 || p[2] == 2 || p[2] == 4;
        } else if (const BYTE f3 = c.Feature(dev_, kBatteryVoltage); f3 && c.Call(dev_, f3, 0, {}, &p) == ERROR_SUCCESS) {
            s.ok = true;
            const int mv = (p[0] << 8) | p[1];
            s.battery = std::clamp((mv - 3500) * 100 / (4150 - 3500), 0, 100);  // rough Li-ion curve
            s.batteryApprox = true;
            s.charging = (p[2] & 0x80) != 0;
        } else {
            s.wired = true;
        }
        // DPI (sensor 0): current value, and the range from the sensor's DPI list.
        if (const BYTE f = c.Feature(dev_, kAdjustableDpi)) {
            if (c.Call(dev_, f, 2, {0}, &p) == ERROR_SUCCESS) {
                s.ok = true;
                s.dpi = (p[1] << 8) | p[2];
            }
            // List: 16-bit values after the sensor index, 0-terminated; 0xE000 | step marks a range.
            if (c.Call(dev_, f, 1, {0}, &p) == ERROR_SUCCESS) {
                std::vector<int> values;
                for (size_t i = 1; i + 1 < p.size(); i += 2) {
                    const int v = (p[i] << 8) | p[i + 1];
                    if (!v) break;
                    if ((v & 0xE000) != 0xE000) values.push_back(v);
                }
                if (!values.empty()) {
                    s.dpiMin = *std::min_element(values.begin(), values.end());
                    s.dpiMax = *std::max_element(values.begin(), values.end());
                }
            }
        }
        // Report rate: a bit per supported interval (bit 0 = 1 ms ... bit 7 = 8 ms).
        if (const BYTE f = c.Feature(dev_, kReportRate)) {
            if (c.Call(dev_, f, 0, {}, &p) == ERROR_SUCCESS)
                for (int bit = 0; bit < 8; ++bit)
                    if (p[0] & (1 << bit)) s.pollChoices.push_back(1000 / (bit + 1));
            if (c.Call(dev_, f, 1, {}, &p) == ERROR_SUCCESS && p[0]) {
                s.ok = true;
                s.pollHz = 1000 / p[0];
            }
        } else if (c.Feature(dev_, kExtendedReportRate)) {
            s.note = L"2000-8000 Hz Logitech mice aren't supported yet; set the rate in G HUB";
        }
        if (s.note.empty()) s.note = L"Logitech support is new and hasn't been tested on real hardware yet";
        return s;
    }

    DWORD SetDpi(int dpi) override {
        Channel c(path_);
        const BYTE f = c.Ok() ? c.Feature(dev_, kAdjustableDpi) : 0;
        if (!f) return ERROR_NOT_SUPPORTED;
        return c.Call(dev_, f, 3, {0, BYTE(dpi >> 8), BYTE(dpi)});
    }

    // Mice in G HUB's "onboard memory" mode may refuse this or reapply their profile later.
    DWORD SetPolling(const MouseState&, int hz) override {
        Channel c(path_);
        const BYTE f = c.Ok() ? c.Feature(dev_, kReportRate) : 0;
        if (!f || hz <= 0) return ERROR_NOT_SUPPORTED;
        return c.Call(dev_, f, 2, {BYTE(std::clamp(1000 / hz, 1, 8))});
    }

private:
    std::wstring path_;
    BYTE dev_;
    std::wstring name_;
};

struct Found {
    BYTE dev;
    std::wstring name;
};
std::mutex g_cacheLock;
std::map<std::wstring, std::vector<Found>> g_cache;  // per HID++ path: the mice behind it

// The mice reachable through one HID++ channel: the device itself, or up to six paired to
// a Unifying / Lightspeed / Bolt receiver.
std::vector<Found> Discover(const std::wstring& path, const std::wstring& product) {
    std::vector<Found> out;
    Channel c(path);
    if (!c.Ok()) return out;
    const bool receiver = product.find(L"Receiver") != std::wstring::npos;
    std::vector<BYTE> slots = receiver ? std::vector<BYTE>{1, 2, 3, 4, 5, 6} : std::vector<BYTE>{kDirect};
    for (BYTE dev : slots) {
        // Any answer from the root feature means something is there (empty slots time out).
        Params p{};
        if (c.Call(dev, 0x00, 0, {BYTE(kDeviceName >> 8), BYTE(kDeviceName)}, &p, 250) != ERROR_SUCCESS) continue;
        if (IsPointer(c, dev) == 0) continue;  // a keyboard on the same receiver
        std::wstring name = ReadName(c, dev);
        out.push_back({dev, name.empty() ? product : name});
    }
    return out;
}

}  // namespace

std::vector<std::shared_ptr<VendorMouse>> FindLogitechMice() {
    std::vector<std::shared_ptr<VendorMouse>> out;
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
        if (HidD_GetAttributes(f, &attr) && attr.VendorID == kLogitechVid) {
            PHIDP_PREPARSED_DATA pp = nullptr;
            if (HidD_GetPreparsedData(f, &pp)) {
                HidP_GetCaps(pp, &caps);
                HidD_FreePreparsedData(pp);
            }
            HidD_GetProductString(f, product, sizeof product);
        }
        CloseHandle(f);
        // HID++ "long report" collection: vendor page 0xFF00 (USB) or 0xFF43 (Bluetooth), 20-byte reports.
        if (attr.VendorID != kLogitechVid || (caps.UsagePage != 0xFF00 && caps.UsagePage != 0xFF43) || caps.InputReportByteLength != 20)
            continue;
        const std::wstring path = detail->DevicePath;
        std::vector<Found> found;
        {
            std::lock_guard lock(g_cacheLock);
            if (auto it = g_cache.find(path); it != g_cache.end()) found = it->second;
        }
        if (found.empty()) {  // asking is slow (empty receiver slots time out): remember what answered
            found = Discover(path, *product ? product : L"Logitech device");
            std::lock_guard lock(g_cacheLock);
            if (!found.empty()) g_cache[path] = found;
        }
        for (const Found& m : found) out.push_back(std::make_shared<LogitechMouse>(path, m.dev, m.name));
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

}  // namespace mtm
