#include "graph.h"
#include "page.h"
#include "theme.h"
#include "tweaks.h"

#include <shellapi.h>

#include <algorithm>
#include <memory>
#include <thread>

namespace mtm {
namespace {

std::wstring PriorityText(int p) {
    return p == 3 ? L"high" : p == 2 ? L"normal" : p == 1 ? L"low" : L"driver default";
}

// Longer explanations, shown when hovering a card's info icon.
std::vector<TipSection> GpuTip() {
    return {{L"What it does",
             L"Switches the graphics card to Message Signaled Interrupts (MSI) and can mark its interrupts as high "
             L"priority. MSI lets the card signal the CPU with a memory write instead of a shared interrupt line, "
             L"so it never waits behind other devices sharing that line."},
            {L"What to expect",
             L"Most modern NVIDIA and AMD drivers already use MSI; if the card says \"MSI in use\", that part changes "
             L"nothing. The priority hint has a small, hard-to-measure effect. Takes effect after a restart."},
            {L"Risk & undo",
             L"Low on cards from the last ten years. A driver update can reset it. \"Restore original\" puts the values "
             L"from before Kernova back; if the PC ever failed to show a picture, Safe Mode still starts normally."}};
}
std::vector<TipSection> QuantumTip() {
    return {{L"What it does",
             L"Sets Win32PrioritySeparation: how long each thread may run before Windows switches to another (its "
             L"quantum), and whether the window you're using gets longer slices. Applies instantly."},
            {L"The choices",
             L"Gaming (0x28): short, equal slices for every thread, for steady frame pacing. Windows default (0x02): "
             L"short slices with a 3× boost for the app in front. Background work (0x18): long slices, better for "
             L"rendering and compiling while you're away."},
            {L"What to expect", L"Differences are small and depend on the game; try it and compare."}};
}
std::vector<TipSection> UsbTip() {
    return {{L"What it does",
             L"USB selective suspend lets Windows put idle USB ports to sleep. Turning it off keeps them awake. "
             L"Stored in the active power plan."},
            {L"Why gamers turn it off",
             L"A sleeping port has to wake up first, which can cause the odd dropped input or a mouse, headset or "
             L"controller that briefly disconnects."},
            {L"Good to know",
             L"Off uses a little more power on laptops, so \"off when plugged in\" is a good middle ground. Switching "
             L"power plans brings back that plan's own setting."}};
}

class TweaksPage final : public Page {
public:
    const wchar_t* Title() const override { return L"Game tweaks"; }

    void Create() override {
        status_ = CreateLabel(L"", SS_CENTERIMAGE | SS_ENDELLIPSIS);
        applyAll_ = CreateButton(L"\xE945  Apply all recommended", true);
        undoAll_ = CreateButton(L"\xE777  Restore all originals");
        restoreFirst_ = ReadSetting(L"TweakRestorePoint", 1) != 0;
        restoreToggle_ = CreateButton(L"");
        makePoint_ = CreateButton(L"\xE81C  Create restore point");
        sysRestore_ = CreateButton(L"\xE777  System Restore…");
        ShowRestoreToggle();

        // shortcut: the card set is fixed at startup, so a GPU added while Kernova runs shows after a restart.
        for (const GpuInterrupts& g : ListGpuInterrupts()) AddCard(Kind::Gpu, g.instanceId, L"GPU interrupts", GpuTip());
        AddCard(Kind::Quantum, L"quantum", L"Thread scheduling", QuantumTip());
        AddCard(Kind::Usb, L"usb", L"USB power saving", UsbTip());
    }

    static int CardH() { return Scale(150); }
    static int Columns(int width) { return width >= Scale(1100) ? 2 : 1; }
    static bool ToolbarWraps(int width) { return width < Scale(1180); }
    int ToolbarH(int width) const { return ToolbarWraps(width) ? 2 * Scale(34) + Scale(8) : Scale(34); }
    int MinHeight(int width) const override {
        const int rows = int((cards_.size() + Columns(width) - 1) / Columns(width));
        return Scale(32) + ToolbarH(width) + Scale(12) + rows * (CardH() + Scale(12));
    }

    void Layout(RECT a) override {
        const int gap = Scale(12), h = Scale(34), w = a.right - a.left;
        Place(status_, a.left + Scale(2), a.top, w, Scale(26));
        int y = a.top + Scale(32);

        // Toolbar: everything that acts on all tweaks, then the restore-point safety net.
        int x = a.left;
        Place(applyAll_, x, y, Scale(220), h);
        Place(undoAll_, x += Scale(228), y, Scale(200), h);
        if (ToolbarWraps(w)) {
            x = a.left;
            y += h + Scale(8);
        } else {
            x += Scale(200) + Scale(24);
        }
        Place(restoreToggle_, x, y, Scale(210), h);
        Place(makePoint_, x += Scale(218), y, Scale(200), h);
        Place(sysRestore_, x += Scale(208), y, Scale(170), h);
        y += h + gap;

        // One card per tweak: its choices in a dropdown along the bottom edge, then Apply and the info icon.
        const int cols = Columns(w), cardW = (w - (cols - 1) * gap) / cols, pad = Scale(14);
        for (size_t i = 0; i < cards_.size(); ++i) {
            Card& c = *cards_[i];
            const int cx = a.left + int(i % cols) * (cardW + gap), cy = y + int(i / cols) * (CardH() + gap);
            Place(c.text.Handle(), cx, cy, cardW, CardH());
            const int by = cy + CardH() - pad - h;
            int bx = cx + cardW - pad - h;
            Place(c.info.Handle(), bx, by, h, h);
            Place(c.apply, bx -= Scale(8) + Scale(120), by, Scale(120), h);
            Place(c.combo, cx + pad, by, std::max<int>(Scale(160), bx - Scale(8) - cx - pad), Scale(300));
            // On top of the card (it paints with WS_CLIPSIBLINGS, so it leaves them alone).
            for (HWND top : {c.apply, c.combo, c.info.Handle()})
                SetWindowPos(top, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void Show(bool v) override {
        ShowAll({status_, applyAll_, undoAll_, restoreToggle_, makePoint_, sysRestore_}, v);
        for (auto& c : cards_) ShowAll({c->text.Handle(), c->info.Handle(), c->apply, c->combo}, v);
    }

    void OnActivate() override { Refresh(); }

    bool OnCommand(HWND control, WORD code) override {
        for (auto& c : cards_) {
            if (control == c->combo && code == CBN_SELCHANGE) {
                UpdateButtons();
                return true;
            }
            if (control == c->apply && code == BN_CLICKED) {
                const int sel = Selection(*c);
                if (sel < 0 || sel == c->current) return true;
                const Option& o = c->options[size_t(sel)];
                Execute({Request(*c, o)}, c->kind == Kind::Gpu, !o.undo && restoreFirst_);
                return true;
            }
        }
        if (code != BN_CLICKED) return false;
        if (control == applyAll_) RunAll(false);
        else if (control == undoAll_) RunAll(true);
        else if (control == makePoint_) Execute({}, false, true);
        else if (control == restoreToggle_) {
            restoreFirst_ = !restoreFirst_;
            WriteSetting(L"TweakRestorePoint", restoreFirst_ ? 1 : 0);
            ShowRestoreToggle();
        } else if (control == sysRestore_) {
            // System Properties > System Protection: roll back to a point, or turn protection on.
            ShellExecuteW(g_main, nullptr, L"SystemPropertiesProtection.exe", nullptr, nullptr, SW_SHOWNORMAL);
        } else return false;
        return true;
    }

private:
    enum class Kind { Gpu, Quantum, Usb };
    struct Option {
        std::wstring label;
        int v1 = -1, v2 = -1;  // see TweakRequest
        bool undo = false;     // "restore original" (values come from the backup)
    };
    struct Card {
        Kind kind;
        std::wstring key;  // GPU instance id, "quantum" or "usb"
        TextCard text;
        InfoBadge info;
        HWND apply = nullptr, combo = nullptr;
        std::vector<Option> options;  // [0] is the recommended choice
        int current = -1;             // option matching what's set now, -1 = none
        bool backup = false, known = false;  // known: the setting could be read
    };

    void AddCard(Kind kind, std::wstring key, const wchar_t* title, std::vector<TipSection> tip) {
        auto c = std::make_unique<Card>();
        c->kind = kind;
        c->key = std::move(key);
        c->text.Create(title);
        c->info.Create(title, std::move(tip));
        c->combo = CreateCombo({});
        c->apply = CreateButton(L"\xE73E  Apply", true);
        SetOnCard(c->combo);
        SetOnCard(c->apply);
        c->info.onCard = true;
        cards_.push_back(std::move(c));
    }

    void ShowRestoreToggle() {
        SetText(restoreToggle_, restoreFirst_ ? L"\xE73A  Restore point first" : L"\xE739  Restore point first");
    }

    static int Selection(const Card& c) { return static_cast<int>(SendMessageW(c.combo, CB_GETCURSEL, 0, 0)); }

    static TweakRequest Request(const Card& c, const Option& o) {
        const TweakKind k = c.kind == Kind::Gpu ? TweakKind::Gpu : c.kind == Kind::Quantum ? TweakKind::Quantum : TweakKind::Usb;
        return {k, c.key, o.undo, o.v1, o.v2};
    }

    // Rebuilds each card's choices from what's set right now and selects the current one.
    void Refresh() {
        const std::vector<GpuInterrupts> gpus = ListGpuInterrupts();
        const QuantumState q = ReadQuantum();
        const UsbSuspendState u = ReadUsbSuspend();
        for (auto& cp : cards_) {
            Card& c = *cp;
            std::wstring now, hint, device;
            int cur1 = -1, cur2 = -1;
            bool known = true;
            if (c.kind == Kind::Gpu) {
                c.options = {{L"MSI on · high priority (recommended)", 1, 3},
                             {L"MSI on · driver default priority", 1, -1},
                             {L"Driver default for both", -1, -1}};
                auto g = std::find_if(gpus.begin(), gpus.end(), [&](const GpuInterrupts& x) { return x.instanceId == c.key; });
                if (g == gpus.end()) {
                    known = false;
                    now = L"This graphics card is no longer present";
                } else {
                    device = g->name;
                    cur1 = g->msiSetting;
                    cur2 = g->priority;
                    now = (g->msiInUse == 1 ? L"MSI in use" : g->msiInUse == 0 ? L"Line-based interrupts" : L"Interrupt mode unknown") +
                          std::wstring(L" · priority ") + PriorityText(g->priority);
                    if (g->msiSetting == 1 && g->msiInUse == 0) now += L"  ·  restart needed";
                }
                hint = L"Changes take effect after a restart.";
            } else if (c.kind == Kind::Quantum) {
                c.options.clear();
                for (const QuantumPreset& p : QuantumPresets()) c.options.push_back({p.name, int(p.value)});
                c.options[0].label += L" (recommended)";
                cur1 = int(q.value);
                now = DescribeQuantum(q.value);
                hint = L"How Windows shares CPU time between programs. Takes effect immediately.";
            } else {
                c.options = {{L"Off · ports never sleep (recommended)", 0, 0},
                             {L"Off when plugged in · on for battery", 0, 1},
                             {L"On · Windows default", 1, 1}};
                known = u.known;
                cur1 = int(u.ac);
                cur2 = int(u.dc);
                now = !u.known ? L"Not available in this power plan"
                               : std::wstring(L"Plugged in: ") + (u.ac ? L"on" : L"off") + L" · On battery: " + (u.dc ? L"on" : L"off");
                hint = L"Whether Windows may put idle USB ports to sleep. Takes effect immediately.";
            }
            c.current = -1;
            for (size_t i = 0; i < c.options.size(); ++i)
                if (c.options[i].v1 == cur1 && c.options[i].v2 == cur2) c.current = int(i);
            if (c.current < 0 && known) {  // set to something none of the choices describe: show it as it is
                c.options.insert(c.options.begin() + 1, {L"Custom setting", cur1, cur2});
                c.current = 1;
            }
            c.backup = HasTweakBackup(c.key);
            if (c.backup) c.options.push_back({L"Restore original (before Kernova)", -1, -1, true});

            SendMessageW(c.combo, CB_RESETCONTENT, 0, 0);
            for (size_t i = 0; i < c.options.size(); ++i) {
                const std::wstring label = c.options[i].label + (int(i) == c.current ? L"   ✓ current" : L"");
                SendMessageW(c.combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
            SendMessageW(c.combo, CB_SETCURSEL, c.current >= 0 ? c.current : 0, 0);
            c.known = known;
            c.text.SetSections({{now, hint}}, device);
        }
        if (!busy_ && GetText(status_).empty())
            SetText(status_, L"Pick a setting from a card's list and press Apply. Kernova saves the original first, so you can always go back.");
        UpdateButtons();
    }

    void UpdateButtons() {
        bool anyBackup = false, anyToApply = false;
        for (auto& c : cards_) {
            const int sel = Selection(*c);
            EnableWindow(c->combo, !busy_ && c->known);
            EnableWindow(c->apply, !busy_ && c->known && sel >= 0 && sel != c->current);
            anyBackup |= c->backup;
            anyToApply |= c->known && c->current != 0;
        }
        EnableWindow(applyAll_, !busy_ && anyToApply);
        EnableWindow(undoAll_, !busy_ && anyBackup);
        EnableWindow(restoreToggle_, !busy_);
        EnableWindow(makePoint_, !busy_);
    }

    void RunAll(bool undo) {
        std::vector<TweakRequest> reqs;
        bool gpu = false;
        for (auto& c : cards_) {
            if (undo ? !c->backup : (!c->known || c->current == 0)) continue;
            reqs.push_back(undo ? Request(*c, c->options.back()) : Request(*c, c->options[0]));
            gpu |= c->kind == Kind::Gpu;
        }
        if (!reqs.empty()) Execute(std::move(reqs), gpu, !undo && restoreFirst_);
    }

    // Runs on a worker: one UAC prompt covers the restore point and every change.
    void Execute(std::vector<TweakRequest> reqs, bool gpu, bool restorePoint) {
        if (busy_) return;
        // Windows can't make restore points with System Protection off: say so up front
        // instead of failing (and changing nothing) after the admin prompt.
        if (restorePoint && !SystemProtectionOn()) {
            if (reqs.empty()) {
                if (Confirm(L"System Protection is turned off on this PC, so Windows can't create restore points.\n\n"
                            L"Open System Protection now? Select your system drive, press Configure and choose "
                            L"\"Turn on system protection\"."))
                    ShellExecuteW(g_main, nullptr, L"SystemPropertiesProtection.exe", nullptr, nullptr, SW_SHOWNORMAL);
                return;
            }
            if (!Confirm(L"System Protection is turned off on this PC, so Windows can't create a restore point first.\n\n"
                         L"Apply the change without one? Kernova still saves the original setting, so you can go back "
                         L"with \"Restore original\" in the list.\n\n"
                         L"(To get restore points, turn on System Protection under System Restore… → Configure.)"))
                return;
            restorePoint = false;
        }
        busy_ = true;
        UpdateButtons();
        const bool onlyPoint = reqs.empty();
        SetText(status_, restorePoint ? (onlyPoint ? L"Creating a restore point… this can take a minute."
                                                   : L"Creating a restore point, then applying… this can take a minute.")
                                      : L"Applying…");
        worker_ = std::jthread([this, reqs = std::move(reqs), gpu, restorePoint, onlyPoint](std::stop_token) {
            bool pointFailed = false;
            const DWORD r = RunTweaks(reqs, restorePoint, &pointFailed);
            PostToUi([this, r, gpu, restorePoint, onlyPoint, pointFailed] {
                busy_ = false;
                if (r == ERROR_SUCCESS)
                    SetText(status_, std::wstring(onlyPoint ? L"Restore point created."
                                                  : restorePoint ? L"Restore point created and changes applied." : L"Done.") +
                                         (gpu ? L"  Restart the PC for the graphics card change to take effect." : L""));
                else if (r == ERROR_CANCELLED) SetText(status_, L"Not changed — the administrator prompt was cancelled.");
                else if (r == kTweakLeftUnchanged)
                    SetText(status_, L"Left unchanged: a setting has an unusual current value that Kernova couldn't restore "
                                     L"safely later, so it wasn't touched. Any other changes were applied.");
                else if (pointFailed && r == ERROR_SERVICE_DISABLED)
                    SetText(status_, L"Nothing changed: System Protection is off for your system drive. Turn it on under "
                                     L"System Restore… → Configure, or switch off \"Restore point first\".");
                else if (pointFailed && r == ERROR_ALREADY_EXISTS)
                    SetText(status_, L"Nothing changed: a setting from an earlier run couldn't be put back yet. Run Kernova as "
                                     L"administrator and try again.");
                else if (pointFailed) SetText(status_, L"Couldn't create the restore point, so nothing was changed: " + ErrorText(r));
                else SetText(status_, L"Couldn't change it: " + ErrorText(r));
                Refresh();
                // Failures get a dialog too: the status line alone is easy to miss. A skipped setting isn't a failure.
                if (r != ERROR_SUCCESS && r != ERROR_CANCELLED && r != kTweakLeftUnchanged) ShowError(GetText(status_));
            });
        });
    }

    HWND status_ = nullptr, applyAll_ = nullptr, undoAll_ = nullptr, restoreToggle_ = nullptr, makePoint_ = nullptr,
         sysRestore_ = nullptr;
    bool restoreFirst_ = true, busy_ = false;
    std::vector<std::unique_ptr<Card>> cards_;
    std::jthread worker_;  // last: joined before the rest is destroyed
};

}  // namespace

std::unique_ptr<Page> MakeTweaksPage() { return std::make_unique<TweaksPage>(); }

}  // namespace mtm
