#include "graph.h"
#include "page.h"
#include "razer.h"
#include "theme.h"

#include <thread>

// Mouse settings without vendor software: Razer mice over their control protocol, plus
// Windows' own "Enhance pointer precision" (mouse acceleration).

namespace mtm {
namespace {

bool PointerPrecisionOn() {
    int p[3] = {};
    return SystemParametersInfoW(SPI_GETMOUSE, 0, p, 0) && p[2] != 0;
}

// Off = no acceleration at all; On = Windows' defaults (thresholds 6 / 10, speed 1).
bool SetPointerPrecision(bool on) {
    int p[3] = {on ? 6 : 0, on ? 10 : 0, on ? 1 : 0};
    return SystemParametersInfoW(SPI_SETMOUSE, 0, p, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE) != FALSE;
}

class DevicesPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Mouse settings"; }

    void Create() override {
        status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        refresh_ = CreateButton(L"\xE72C  Refresh");
        battery_.Create(0xE83F, L"Battery", Graph::Tint::Memory, nullptr);
        poll_.Create(0xE962, L"Polling rate", Graph::Tint::Network, nullptr);
        dpi_.Create(0xE7C9, L"DPI", Graph::Tint::Cpu, nullptr);
        pointer_.Create(0xE7C2, L"Enhance pointer precision", Graph::Tint::Gpu, nullptr);
        pollPick_ = CreateCombo({});
        pollApply_ = CreateButton(L"\xE73E  Apply", true);
        stagePick_ = CreateCombo({});
        stageApply_ = CreateButton(L"\xE73E  Apply", true);
        dpiEdit_ = CreateEdit(L"", L"Custom DPI (100 – 30000)", 0);
        dpiApply_ = CreateButton(L"\xE73E  Set");
        pointerPick_ = CreateCombo({L"Off (recommended for games)", L"On (Windows default)"});
        pointerApply_ = CreateButton(L"\xE73E  Apply", true);
        info_.Create(L"About these settings");
        info_.SetSections(
            {{L"Razer mice",
              L"Kernova talks to the mouse directly, the same way Razer Synapse does, so Synapse isn't needed. Changes "
              L"are saved inside the mouse and survive unplugging. If Synapse is running it may apply its own profile "
              L"over them."},
             {L"Polling rate",
              L"How often the mouse reports its position. 1000 Hz is the usual sweet spot; 2000-8000 Hz needs a mouse "
              L"and receiver that support it (Razer HyperPolling) and costs more CPU time and battery."},
             {L"Enhance pointer precision",
              L"Windows' mouse acceleration: the faster you move, the further the pointer goes. That makes the same "
              L"hand movement land in different places, which is why it's usually turned off for aiming in games. "
              L"Raw-input games ignore it, but many don't use raw input."}});
        RefreshPointer();
    }

    static int CardH() { return Scale(140); }
    int MinHeight(int) const override { return Scale(42) + CardH() + 2 * Scale(42) + Scale(12) + Scale(200); }

    void Layout(RECT a) override {
        const int gap = Scale(12), h = Scale(34), w = a.right - a.left;
        Place(status_, a.left + Scale(2), a.top, w - Scale(140), h);
        Place(refresh_, a.right - Scale(130), a.top, Scale(130), h);
        int y = a.top + h + Scale(8);
        const int cw = (w - 3 * gap) / 4;
        ActionCard* cards[] = {&battery_, &poll_, &dpi_, &pointer_};
        for (int i = 0; i < 4; ++i) Place(cards[i]->Handle(), a.left + i * (cw + gap), y, i == 3 ? a.right - (a.left + 3 * (cw + gap)) : cw, CardH());
        y += CardH() + Scale(8);
        // Controls under their cards: a dropdown and its button side by side.
        // (A dropdown's height includes its open list; an edit box gets the plain row height.)
        auto row = [&](int col, int yy, HWND pick, HWND apply, int applyW, int pickH) {
            const int x = a.left + col * (cw + gap);
            Place(pick, x, yy, cw - applyW - Scale(8), pickH);
            Place(apply, x + cw - applyW, yy, applyW, h);
        };
        row(1, y, pollPick_, pollApply_, Scale(110), Scale(300));
        row(2, y, stagePick_, stageApply_, Scale(110), Scale(300));
        row(3, y, pointerPick_, pointerApply_, Scale(110), Scale(300));
        row(2, y + h + Scale(8), dpiEdit_, dpiApply_, Scale(110), h);
        y += 2 * (h + Scale(8)) + Scale(4);
        Place(info_.Handle(), a.left, y, w, std::max(Scale(200), int(a.bottom) - y));
    }

    void Show(bool v) override {
        ShowAll({status_, refresh_, battery_.Handle(), poll_.Handle(), dpi_.Handle(), pointer_.Handle(), pollPick_, pollApply_,
                 stagePick_, stageApply_, dpiEdit_, dpiApply_, pointerPick_, pointerApply_, info_.Handle()},
                v);
    }

    void OnActivate() override {
        RefreshPointer();
        Read(L"Looking for a supported mouse…");
    }

    void OnTick(bool visible) override {
        if (visible && ++ticks_ % 60 == 0 && !busy_) Read(L"");  // battery moves slowly; once a minute is plenty
    }

    bool OnCommand(HWND control, WORD code) override {
        if (code != BN_CLICKED) return false;
        if (control == refresh_) Read(L"Reading the mouse…");
        else if (control == pointerApply_) ApplyPointer();
        else if (control == pollApply_) ApplyPolling();
        else if (control == stageApply_) ApplyStage();
        else if (control == dpiApply_) ApplyDpi();
        else return false;
        return true;
    }

private:
    // Reads every mouse on a worker; the first one that answers is shown.
    void Read(std::wstring busyText) { Run(std::move(busyText), nullptr, L""); }

    // Optionally changes something first, then reads everything back so the cards show
    // what the mouse really has now (not what we asked for).
    void Run(std::wstring busyText, std::function<DWORD(const RazerMouse&, const RazerState&)> change, std::wstring doneText) {
        if (busy_) return;
        busy_ = true;
        UpdateButtons();
        if (!busyText.empty()) SetText(status_, busyText);
        worker_ = std::jthread([this, change = std::move(change), doneText = std::move(doneText),
                                mouse = mouse_, state = state_](std::stop_token) {
            DWORD err = ERROR_SUCCESS;
            if (change) err = change(mouse, state);
            std::vector<RazerMouse> mice = FindRazerMice();
            RazerMouse found;
            RazerState now;
            for (const RazerMouse& m : mice) {
                now = ReadRazer(m);
                found = m;
                if (now.ok) break;
            }
            PostToUi([this, err, doneText, found, now, any = !mice.empty()] {
                busy_ = false;
                mouse_ = found;
                state_ = now;
                Show();
                if (err == ERROR_NOT_SUPPORTED) SetText(status_, L"The mouse refused that setting (not supported by this model or receiver).");
                else if (err != ERROR_SUCCESS) SetText(status_, L"Couldn't change it: " + ErrorText(err));
                else if (!doneText.empty()) SetText(status_, doneText);
                else if (!any) SetText(status_, L"No supported mouse found. Razer mice are supported so far; more brands to come.");
                else if (!now.ok) SetText(status_, L"Found " + found.name + L", but it didn't answer. Wireless mice asleep or out of range don't reply; move it and press Refresh.");
                else SetText(status_, found.name + L" · firmware " + now.firmware + L" · read directly from the mouse");
            });
        });
    }

    void Show() {
        const RazerState& s = state_;
        const bool ok = s.ok;
        if (ok && s.wired)
            battery_.SetText(L"Wired", mouse_.name, L"Powered by the cable, no battery");
        else if (ok)
            battery_.SetText(Format(L"%d%%", s.battery), mouse_.name,
                             std::wstring(s.charging ? L"Charging over cable" : L"On battery") +
                                 (s.sleepSeconds ? Format(L" · sleeps after %d min idle", s.sleepSeconds / 60) : L""));
        else battery_.SetText(L"—", L"No mouse", L"");
        poll_.SetText(ok && s.pollHz ? Format(L"%d Hz", s.pollHz) : L"—", ok ? L"Reports per second" : L"",
                      ok ? (s.pollExtended ? L"Supports up to 8000 Hz with a HyperPolling receiver" : L"125 – 1000 Hz") : L"");
        std::wstring stages;
        for (size_t i = 0; i < s.stages.size(); ++i)
            stages += (i ? L" · " : L"") + (int(i) == s.activeStage ? L"[" + std::to_wstring(s.stages[i]) + L"]" : std::to_wstring(s.stages[i]));
        dpi_.SetText(ok && s.dpiX ? (s.dpiX == s.dpiY ? Format(L"%d", s.dpiX) : Format(L"%d × %d", s.dpiX, s.dpiY)) : L"—",
                     ok ? L"Current sensitivity" : L"", stages.empty() ? L"" : L"Stages: " + stages);

        SendMessageW(pollPick_, CB_RESETCONTENT, 0, 0);
        pollChoices_ = ok ? RazerPollingChoices(s.pollExtended) : std::vector<int>{};
        for (int hz : pollChoices_) {
            const std::wstring label = Format(L"%d Hz", hz) + (hz == s.pollHz ? L"   ✓ current" : L"");
            SendMessageW(pollPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (hz == s.pollHz) SendMessageW(pollPick_, CB_SETCURSEL, SendMessageW(pollPick_, CB_GETCOUNT, 0, 0) - 1, 0);
        }
        SendMessageW(stagePick_, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < s.stages.size(); ++i) {
            const std::wstring label = Format(L"Stage %zu · %d DPI", i + 1, s.stages[i]) + (int(i) == s.activeStage ? L"   ✓ current" : L"");
            SendMessageW(stagePick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(stagePick_, CB_SETCURSEL, std::max(0, s.activeStage), 0);
        UpdateButtons();
    }

    void RefreshPointer() {
        const bool on = PointerPrecisionOn();
        pointer_.SetText(on ? L"On" : L"Off", L"Windows mouse acceleration",
                         on ? L"Pointer speed changes with hand speed" : L"1 : 1 movement, best for aiming");
        SendMessageW(pointerPick_, CB_RESETCONTENT, 0, 0);
        const wchar_t* labels[2] = {L"Off (recommended for games)", L"On (Windows default)"};
        for (int i = 0; i < 2; ++i) {
            const std::wstring label = std::wstring(labels[i]) + ((i == 1) == on ? L"   ✓ current" : L"");
            SendMessageW(pointerPick_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(pointerPick_, CB_SETCURSEL, on ? 1 : 0, 0);
    }

    void UpdateButtons() {
        const bool ok = state_.ok && !busy_;
        for (HWND hw : {pollPick_, pollApply_}) EnableWindow(hw, ok && !pollChoices_.empty());
        for (HWND hw : {stagePick_, stageApply_}) EnableWindow(hw, ok && !state_.stages.empty());
        for (HWND hw : {dpiEdit_, dpiApply_}) EnableWindow(hw, ok);
        EnableWindow(refresh_, !busy_);
    }

    void ApplyPointer() {
        const bool on = SendMessageW(pointerPick_, CB_GETCURSEL, 0, 0) == 1;
        if (on == PointerPrecisionOn()) return;
        SetText(status_, SetPointerPrecision(on) ? (on ? L"Enhance pointer precision turned on." : L"Enhance pointer precision turned off: the pointer now moves 1 : 1 with your hand.")
                                                 : L"Couldn't change the mouse setting: " + ErrorText(GetLastError()));
        RefreshPointer();
    }

    void ApplyPolling() {
        const int sel = static_cast<int>(SendMessageW(pollPick_, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel >= int(pollChoices_.size()) || pollChoices_[size_t(sel)] == state_.pollHz) return;
        const int hz = pollChoices_[size_t(sel)];
        if (hz > 1000 && !Confirm(Format(L"Set the polling rate to %d Hz?\n\nAbove 1000 Hz needs a receiver that supports it (Razer "
                                         L"HyperPolling). With a regular receiver the mouse may refuse or stay at 1000 Hz. "
                                         L"Higher rates also use more CPU time and battery.", hz)))
            return;
        const bool ext = state_.pollExtended;
        Run(L"Setting the polling rate…", [ext, hz](const RazerMouse& m, const RazerState&) { return RazerSetPolling(m, ext, hz); },
            Format(L"Polling rate set to %d Hz. Check it on the Input tester page.", hz));
    }

    void ApplyStage() {
        const int sel = static_cast<int>(SendMessageW(stagePick_, CB_GETCURSEL, 0, 0));
        if (sel < 0 || sel == state_.activeStage) return;
        Run(L"Switching DPI stage…", [sel](const RazerMouse& m, const RazerState& s) { return RazerSetActiveStage(m, s, sel); },
            Format(L"Now on stage %d (%d DPI).", sel + 1, state_.stages[size_t(sel)]));
    }

    void ApplyDpi() {
        const int dpi = _wtoi(GetText(dpiEdit_).c_str());
        if (dpi < 100 || dpi > 30000) {
            SetText(status_, L"Enter a DPI between 100 and 30000.");
            return;
        }
        Run(L"Setting DPI…", [dpi](const RazerMouse& m, const RazerState&) { return RazerSetDpi(m, dpi); },
            Format(L"DPI set to %d. The DPI button on the mouse still switches between its stages.", dpi));
    }

    HWND status_ = nullptr, refresh_ = nullptr, pollPick_ = nullptr, pollApply_ = nullptr, stagePick_ = nullptr,
         stageApply_ = nullptr, dpiEdit_ = nullptr, dpiApply_ = nullptr, pointerPick_ = nullptr, pointerApply_ = nullptr;
    ActionCard battery_, poll_, dpi_, pointer_;
    TextCard info_;
    RazerMouse mouse_;
    RazerState state_;
    std::vector<int> pollChoices_;
    bool busy_ = false;
    unsigned ticks_ = 0;
    std::jthread worker_;  // last: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeDevicesPage() { return std::make_unique<DevicesPage>(); }

}  // namespace mtm
