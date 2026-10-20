#include "inputdev.h"

#include <cfgmgr32.h>
#include <winioctl.h>  // CTL_CODE, for the USB hub IOCTLs
#include <hidsdi.h>
#include <setupapi.h>
#include <usbioctl.h>

#include <algorithm>
#include <map>

namespace mtm {
namespace {

constexpr GUID kUsbHubInterface = {0xf18a0e88, 0xc30c, 0x11d0, {0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8}};
constexpr USHORT kWootingVid = 0x31E3;

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / freq;
}

void ParseIds(const std::wstring& path, USHORT& vid, USHORT& pid) {
    const std::wstring up = ToUpper(path);
    const size_t v = up.find(L"VID_"), p = up.find(L"PID_");
    if (v != std::wstring::npos) vid = USHORT(wcstoul(up.substr(v + 4, 4).c_str(), nullptr, 16));
    if (p != std::wstring::npos) pid = USHORT(wcstoul(up.substr(p + 4, 4).c_str(), nullptr, 16));
}

std::wstring HidName(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    wchar_t maker[128] = {}, product[128] = {};
    HidD_GetManufacturerString(f, maker, sizeof maker);
    HidD_GetProductString(f, product, sizeof product);
    CloseHandle(f);
    std::wstring name = product;
    if (*maker && name.find(maker) != 0 && !wcsstr(maker, L"Microsoft")) name = std::wstring(maker) + L" " + name;
    return name;
}

// "\\?\HID#VID_x&PID_y&MI_01#8&abc&0&0000#{guid}" -> "HID\VID_x&PID_y&MI_01\8&abc&0&0000"
std::wstring InstanceIdFromPath(std::wstring path) {
    if (path.rfind(L"\\\\?\\", 0) == 0) path.erase(0, 4);
    if (const size_t g = path.rfind(L"#{"); g != std::wstring::npos) path.resize(g);
    for (wchar_t& c : path)
        if (c == L'#') c = L'\\';
    return path;
}

// Polling rate the device's fastest interrupt-IN endpoint asks the USB host for. Walks up
// from the HID interface to the node plugged into a hub port, then asks that hub.
int DeclaredUsbHz(const std::wstring& path) {
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, const_cast<wchar_t*>(InstanceIdFromPath(path).c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
        return 0;
    for (int depth = 0; depth < 6; ++depth) {
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) return 0;
        wchar_t parentId[MAX_DEVICE_ID_LEN] = {};
        CM_Get_Device_IDW(parent, parentId, MAX_DEVICE_ID_LEN, 0);
        ULONG len = 0;
        if (CM_Get_Device_Interface_List_SizeW(&len, const_cast<GUID*>(&kUsbHubInterface), parentId,
                                               CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS &&
            len > 1) {
            std::wstring hub(len, L'\0');
            if (CM_Get_Device_Interface_ListW(const_cast<GUID*>(&kUsbHubInterface), parentId, hub.data(), len,
                                              CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
                return 0;
            ULONG port = 0, size = sizeof port;
            if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_ADDRESS, nullptr, &port, &size, 0) != CR_SUCCESS) return 0;
            HANDLE h = CreateFileW(hub.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h == INVALID_HANDLE_VALUE) return 0;
            std::vector<BYTE> buf(sizeof(USB_NODE_CONNECTION_INFORMATION_EX) + 32 * sizeof(USB_PIPE_INFO));
            auto* info = reinterpret_cast<USB_NODE_CONNECTION_INFORMATION_EX*>(buf.data());
            info->ConnectionIndex = port;
            DWORD got = 0;
            const BOOL ok = DeviceIoControl(h, IOCTL_USB_GET_NODE_CONNECTION_INFORMATION_EX, buf.data(), DWORD(buf.size()),
                                            buf.data(), DWORD(buf.size()), &got, nullptr);
            CloseHandle(h);
            if (!ok) return 0;
            int best = 0;
            for (ULONG i = 0; i < info->NumberOfOpenPipes && i < 32; ++i) {
                const USB_ENDPOINT_DESCRIPTOR& ep = info->PipeList[i].EndpointDescriptor;
                if (!(ep.bEndpointAddress & 0x80) || (ep.bmAttributes & 3) != 3 || !ep.bInterval) continue;  // interrupt IN only
                // Low/full speed: bInterval is in 1 ms frames. High speed and faster: 2^(n-1) x 125 us.
                const int hz = info->Speed >= UsbHighSpeed ? 8000 >> std::min<int>(ep.bInterval - 1, 15) : 1000 / ep.bInterval;
                best = std::max(best, hz);
            }
            return best;
        }
        node = parent;
    }
    return 0;
}

}  // namespace

const InputDeviceInfo& DescribeInputDevice(HANDLE device) {
    static std::map<HANDLE, InputDeviceInfo> cache;
    if (auto it = cache.find(device); it != cache.end()) return it->second;
    InputDeviceInfo info;
    UINT size = 0;
    GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, nullptr, &size);
    std::wstring path(size, L'\0');
    if (size && GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, path.data(), &size) != UINT(-1)) {
        path.resize(wcslen(path.c_str()));
        ParseIds(path, info.vid, info.pid);
        info.name = HidName(path);
        info.usbHz = DeclaredUsbHz(path);
    }
    RID_DEVICE_INFO di{sizeof di};
    UINT diSize = sizeof di;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICEINFO, &di, &diSize) != UINT(-1) && di.dwType == RIM_TYPEMOUSE)
        info.buttons = int(di.mouse.dwNumberOfButtons);
    if (info.name.empty())
        info.name = info.vid ? Format(L"USB device %04X:%04X", info.vid, info.pid) : L"Built-in or virtual device";
    return cache[device] = info;
}

KeyboardForm GuessKeyboardForm(const std::wstring& productName) {
    const std::wstring n = ToLower(productName);
    if (n.find(L"tkl") != std::wstring::npos || n.find(L"tenkeyless") != std::wstring::npos) return KeyboardForm::Tkl;
    // The first number in the name that is a common layout size: "80HE", "Huntsman Mini 60", "K65"...
    for (size_t i = 0; i < n.size(); ++i) {
        if (!iswdigit(n[i]) || (i && iswdigit(n[i - 1]))) continue;
        const int v = _wtoi(n.c_str() + i);
        if (v == 80 || v == 87) return KeyboardForm::Tkl;
        if (v == 75 || v == 84) return KeyboardForm::Compact75;
        if (v == 65 || v == 68) return KeyboardForm::Compact65;
        if (v == 60 || v == 61) return KeyboardForm::Compact60;
    }
    return KeyboardForm::Full;
}

int KeyIdFromHidUsage(int u) {
    static const std::array<int, 256> table = [] {
        std::array<int, 256> t{};
        const int letters[26] = {0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
                                 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C};
        for (int i = 0; i < 26; ++i) t[size_t(0x04 + i)] = letters[i];
        for (int i = 0; i < 10; ++i) t[size_t(0x1E + i)] = 0x02 + i;  // 1..9, 0
        const std::pair<int, int> rest[] = {
            {0x28, 0x1C},  {0x29, 0x01},  {0x2A, 0x0E},  {0x2B, 0x0F},  {0x2C, 0x39},  {0x2D, 0x0C},  {0x2E, 0x0D},
            {0x2F, 0x1A},  {0x30, 0x1B},  {0x31, 0x2B},  {0x32, 0x2B},  {0x33, 0x27},  {0x34, 0x28},  {0x35, 0x29},
            {0x36, 0x33},  {0x37, 0x34},  {0x38, 0x35},  {0x39, 0x3A},  {0x3A, 0x3B},  {0x3B, 0x3C},  {0x3C, 0x3D},
            {0x3D, 0x3E},  {0x3E, 0x3F},  {0x3F, 0x40},  {0x40, 0x41},  {0x41, 0x42},  {0x42, 0x43},  {0x43, 0x44},
            {0x44, 0x57},  {0x45, 0x58},  {0x46, 0x137}, {0x47, 0x46},  {0x48, kPauseId}, {0x49, 0x152}, {0x4A, 0x147},
            {0x4B, 0x149}, {0x4C, 0x153}, {0x4D, 0x14F}, {0x4E, 0x151}, {0x4F, 0x14D}, {0x50, 0x14B}, {0x51, 0x150},
            {0x52, 0x148}, {0x53, kNumLockId}, {0x54, 0x135}, {0x55, 0x37}, {0x56, 0x4A}, {0x57, 0x4E}, {0x58, 0x11C},
            {0x59, 0x4F},  {0x5A, 0x50},  {0x5B, 0x51},  {0x5C, 0x4B},  {0x5D, 0x4C},  {0x5E, 0x4D},  {0x5F, 0x47},
            {0x60, 0x48},  {0x61, 0x49},  {0x62, 0x52},  {0x63, 0x53},  {0x64, 0x56},  {0x65, 0x15D}, {0xE0, 0x1D},
            {0xE1, 0x2A},  {0xE2, 0x38},  {0xE3, 0x15B}, {0xE4, 0x11D}, {0xE5, 0x36},  {0xE6, 0x138}, {0xE7, 0x15C}};
        for (const auto& [usage, id] : rest) t[size_t(usage)] = id;
        return t;
    }();
    return u >= 0 && u < 256 ? table[size_t(u)] : 0;
}

// ---- Wooting analog -----------------------------------------------------------------

bool AnalogKeyboard::Start() {
    if (Running()) return true;
    // The vendor-defined interface on a Wooting keyboard that streams analog values.
    std::wstring found;
    HDEVINFO set = SetupDiGetClassDevsW(&kHidInterface, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA ifd{sizeof ifd};
    for (DWORD i = 0; found.empty() && SetupDiEnumDeviceInterfaces(set, nullptr, &kHidInterface, i, &ifd); ++i) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        std::vector<BYTE> buf(need);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof *detail;
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, need, nullptr, nullptr)) continue;
        USHORT vid = 0, pid = 0;
        ParseIds(detail->DevicePath, vid, pid);
        if (vid != kWootingVid) continue;
        HANDLE f = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp = nullptr;
        HIDP_CAPS caps{};
        if (HidD_GetPreparsedData(f, &pp)) {
            HidP_GetCaps(pp, &caps);
            HidD_FreePreparsedData(pp);
        }
        CloseHandle(f);
        if (caps.UsagePage == 0xFF53 || caps.UsagePage == 0xFF54) {
            found = detail->DevicePath;
            name = HidName(found);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    if (found.empty()) return false;
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::jthread([this, found](std::stop_token st) { Run(found, st); });
    return true;
}

void AnalogKeyboard::Stop() {
    if (!Running()) return;
    SetEvent(wake_);
    thread_.join();
    thread_ = {};
    CloseHandle(wake_);
    wake_ = nullptr;
    pressed_.clear();
}

std::vector<AnalogKeyboard::Key> AnalogKeyboard::Pressed() {
    std::lock_guard lock(mutex_);
    return pressed_;
}

double AnalogKeyboard::ReportRate() {
    std::lock_guard lock(mutex_);
    const double now = NowMs();
    while (!times_.empty() && times_.front() < now - 1000) times_.pop_front();
    if (times_.size() < 3 || times_.back() < now - 50) return 0;
    return double(times_.size() - 1) * 1000.0 / std::max(1.0, times_.back() - times_.front());
}

// Each input report lists the keys that are off their top position, after the report id,
// as 4-byte entries: a 16-bit key code, then a 16-bit little-endian value whose top 10 bits
// are the depth (0..1023) and whose lowest bit is the "actuated" flag. A zero code ends
// the list; keys not listed are fully up. (Worked out from captures of a Wooting 80HE.)
void AnalogKeyboard::Run(std::wstring path, std::stop_token stop) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::vector<BYTE> buf(1024);
    while (!stop.stop_requested()) {
        ResetEvent(ov.hEvent);
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), DWORD(buf.size()), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) break;
        const HANDLE waits[2] = {ov.hEvent, wake_};
        if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) != WAIT_OBJECT_0) {
            CancelIoEx(f, &ov);
            GetOverlappedResult(f, &ov, &got, TRUE);
            break;
        }
        if (!GetOverlappedResult(f, &ov, &got, FALSE)) break;  // unplugged
        std::vector<Key> now;
        for (DWORD i = 1; i + 3 < got; i += 4) {
            const uint16_t code = uint16_t(buf[i] | (buf[i + 1] << 8));
            if (!code) break;
            const unsigned value = buf[i + 2] | (buf[i + 3] << 8);
            if (value >> 6) now.push_back({code, float(value >> 6) / 1023.0f, (value & 1) != 0});
        }
        std::sort(now.begin(), now.end(), [](const Key& a, const Key& b) { return a.depth > b.depth; });
        std::lock_guard lock(mutex_);
        pressed_ = std::move(now);
        times_.push_back(NowMs());
        if (times_.size() > 20000) times_.pop_front();
    }
    {
        std::lock_guard lock(mutex_);
        pressed_.clear();
    }
    CloseHandle(ov.hEvent);
    CloseHandle(f);
}

}  // namespace mtm
