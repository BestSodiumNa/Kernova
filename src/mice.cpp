#include "mice.h"

#include "razer.h"

namespace mtm {
namespace {

// Razer: the protocol code in razer.cpp, behind the common interface.
class RazerAdapter final : public VendorMouse {
public:
    explicit RazerAdapter(RazerMouse m) : m_(std::move(m)) {}
    std::wstring Name() const override { return m_.name; }
    std::wstring Brand() const override { return L"Razer"; }

    MouseState Read() override {
        const RazerState r = ReadRazer(m_);
        MouseState s;
        s.ok = r.ok;
        s.wired = r.wired;
        s.battery = r.battery;
        s.charging = r.charging;
        s.pollHz = r.pollHz;
        if (r.ok) s.pollChoices = RazerPollingChoices(r.pollExtended);
        s.dpi = r.dpiX;
        s.dpiY = r.dpiY == r.dpiX ? 0 : r.dpiY;
        s.stages = r.stages;
        s.activeStage = r.activeStage;
        s.sleepSeconds = r.sleepSeconds;
        s.firmware = r.firmware;
        if (r.pollExtended) s.note = L"Supports up to 8000 Hz with a HyperPolling receiver";
        return s;
    }

    DWORD SetDpi(int dpi) override { return RazerSetDpi(m_, dpi); }
    DWORD SetPolling(const MouseState& s, int hz) override { return RazerSetPolling(m_, s.pollChoices.size() > 3, hz); }
    DWORD SetActiveStage(const MouseState& s, int stage) override {
        RazerState r;
        r.stages = s.stages;
        return RazerSetActiveStage(m_, r, stage);
    }

private:
    RazerMouse m_;
};

}  // namespace

std::vector<std::shared_ptr<VendorMouse>> FindVendorMice() {
    std::vector<std::shared_ptr<VendorMouse>> out;
    for (RazerMouse& m : FindRazerMice()) out.push_back(std::make_shared<RazerAdapter>(std::move(m)));
    for (auto& m : FindLogitechMice()) out.push_back(std::move(m));
    return out;
}

}  // namespace mtm
