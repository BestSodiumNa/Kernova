#pragma once

#include "common.h"

#include <memory>

namespace mtm {

// What a gaming mouse reports about itself, whatever the brand.
struct MouseState {
    bool ok = false;           // the mouse answered at all (it can be asleep or out of range)
    bool wired = false;        // no battery at all
    int battery = -1;          // %, -1 when wired / not reported
    bool batteryApprox = false;  // estimated from the battery voltage
    bool charging = false;
    int pollHz = 0;
    std::vector<int> pollChoices;  // rates the mouse accepts, fastest first
    int dpi = 0, dpiY = 0;         // dpiY = 0: same as dpi
    int dpiMin = 100, dpiMax = 30000;
    std::vector<int> stages;   // DPI of each stage (empty when the mouse doesn't expose them)
    int activeStage = -1;
    int sleepSeconds = 0;
    std::wstring firmware, note;  // note: anything the user should know about this mouse
};

// One brand's control protocol for one mouse. Every call talks to the hardware and can
// take a few hundred ms over wireless: call them off the UI thread.
class VendorMouse {
public:
    virtual ~VendorMouse() = default;
    virtual std::wstring Name() const = 0;
    virtual std::wstring Brand() const = 0;
    virtual bool Tested() const { return true; }  // false: written from the spec, not yet tried on real hardware
    virtual MouseState Read() = 0;
    // Changes are stored in the mouse. Each returns a Win32 error (ERROR_NOT_SUPPORTED when
    // the mouse refuses the setting).
    virtual DWORD SetDpi(int dpi) = 0;
    virtual DWORD SetPolling(const MouseState& state, int hz) = 0;
    virtual DWORD SetActiveStage(const MouseState& /*state*/, int /*stage*/) { return ERROR_NOT_SUPPORTED; }
};

// Every supported mouse that is connected (Razer, Logitech).
std::vector<std::shared_ptr<VendorMouse>> FindVendorMice();
std::vector<std::shared_ptr<VendorMouse>> FindLogitechMice();  // logitech.cpp

}  // namespace mtm
