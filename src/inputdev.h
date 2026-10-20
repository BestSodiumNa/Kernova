#pragma once

#include "common.h"

#include <array>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

namespace mtm {

// What we can learn about a Raw Input mouse / keyboard without vendor software.
struct InputDeviceInfo {
    std::wstring name;  // "Wooting 80HE", from the device's HID strings
    USHORT vid = 0, pid = 0;
    int usbHz = 0;      // polling rate its USB endpoint asks for (0 = unknown / not USB)
    int buttons = 0;    // mice: number of buttons Windows reports
};
const InputDeviceInfo& DescribeInputDevice(HANDLE rawInputDevice);  // cached per handle

// Keyboard form factors the tester can draw.
enum class KeyboardForm { Full, Tkl, Compact75, Compact65, Compact60 };
// Best guess from the product name ("Wooting 80HE" -> TKL); Full when nothing matches.
KeyboardForm GuessKeyboardForm(const std::wstring& productName);

// Key ids used by the tester: the scan code (+0x100 for E0-prefixed keys); Num Lock and
// Pause get their own ids since their scan codes collide with other keys.
constexpr int kNumLockId = 0x1000 | VK_NUMLOCK, kPauseId = 0x1000 | VK_PAUSE;
// The tester's key id for a HID keyboard usage (page 7), 0 if it has none.
int KeyIdFromHidUsage(int usage);

// Hall-effect key depth from Wooting keyboards, read from their analog HID interface.
// Each report lists the keys that are moving or down; which physical key a code means
// isn't documented, so the tester learns it by pairing codes with Raw Input key presses.
class AnalogKeyboard {
public:
    struct Key {
        uint16_t code;      // the keyboard's own key code
        float depth;        // 0 = up, 1 = bottomed out
        bool actuated;      // past the switch's actuation point (as the keyboard reports it)
    };
    ~AnalogKeyboard() { Stop(); }
    bool Start();  // false when no supported keyboard is connected
    void Stop();
    bool Running() const { return thread_.joinable(); }
    std::wstring name;
    float travelMm = 4.0f;  // full key travel, for showing depth in millimetres
    std::vector<Key> Pressed();  // keys off their top position right now, deepest first
    // Analog reports per second over the last second: the keyboard's real report rate
    // while a key is held down.
    double ReportRate();

private:
    void Run(std::wstring path, std::stop_token stop);
    std::mutex mutex_;
    std::vector<Key> pressed_;
    std::deque<double> times_;  // arrival times (ms) of the last second's reports
    HANDLE wake_ = nullptr;     // set to stop the reader
    std::jthread thread_;       // last: joined before the rest is destroyed
};

}  // namespace mtm
