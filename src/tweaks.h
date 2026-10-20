#pragma once

#include "common.h"

#include <optional>

namespace mtm {

// Latency tweaks for gaming. Every change first saves the original value to a backup
// file, so Undo puts back exactly what was there before (including "not set").

// One PCI graphics adapter's interrupt settings (Device Parameters\Interrupt Management).
struct GpuInterrupts {
    std::wstring name, instanceId;
    int msiSetting = -1;  // MSISupported: -1 not set (driver default), 0 off, 1 on
    int priority = -1;    // Affinity Policy\DevicePriority: -1 not set, 1 low, 2 normal, 3 high
    int msiInUse = -1;    // what Windows actually assigned right now: -1 unknown, 0 line-based, 1 MSI
};
std::vector<GpuInterrupts> ListGpuInterrupts();

// Win32PrioritySeparation (thread quantum + foreground boost). nullopt-style: exists=false = not set.
struct QuantumState {
    bool exists = false;
    DWORD value = 2;  // Windows' default
};
QuantumState ReadQuantum();
std::wstring DescribeQuantum(DWORD value);
struct QuantumPreset {
    const wchar_t* name;
    DWORD value;
};
const std::vector<QuantumPreset>& QuantumPresets();

// USB selective suspend in the active power plan (1 = devices may sleep).
struct UsbSuspendState {
    bool known = false;
    DWORD ac = 1, dc = 1;
};
UsbSuspendState ReadUsbSuspend();

enum class TweakKind { Gpu, Quantum, Usb };
struct TweakRequest {
    TweakKind kind;
    std::wstring key;   // GPU instance id
    bool undo = false;  // restore the backed-up original instead of applying
    // Values to apply. GPU: MSISupported, DevicePriority (-1 = remove, back to the driver's
    // default). Quantum: the Win32PrioritySeparation value. USB: plugged-in, battery (0 off, 1 on).
    int v1 = -1, v2 = -1;
};
// Returned by RunTweaks when everything else succeeded but at least one setting was skipped:
// its current value is outside what the helper can write back (so Undo couldn't restore it),
// and Kernova leaves such a value exactly as it is. Application-defined, never a Win32 code.
constexpr DWORD kTweakLeftUnchanged = 0x40000001;

// Applies / undoes all requests, optionally creating a System Restore point first, with
// at most one UAC prompt. If the restore point fails nothing is changed, *pointFailed is set
// and the error is returned (ERROR_SERVICE_DISABLED = System Protection is off).
// GPU changes need a restart. Blocks; call from a worker thread.
DWORD RunTweaks(const std::vector<TweakRequest>& requests, bool restorePointFirst, bool* pointFailed = nullptr);

// False when System Protection (System Restore) is off, so no restore point can be made.
bool SystemProtectionOn();

// Whether Undo has something to restore. key = GPU instance id, or "quantum" / "usb".
bool HasTweakBackup(const std::wstring& key);

// Handles "--tweak ..." (the elevated helper).
bool HandleTweakCommandLine(int& exitCode);

// Whether the elevated helper would accept this entry ("field|field|..." split into fields).
// Exposed so the tests can pin down what the helper is willing to write.
bool ValidTweakEntry(const std::vector<std::wstring>& entry);

// Where Win32PrioritySeparation lives, and the sibling value that holds its original while the
// latency test (latency.cpp) steps through other values. Both are under the same key.
constexpr wchar_t kPriorityControlKey[] = L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl";
constexpr wchar_t kQuantumValueName[] = L"Win32PrioritySeparation";
constexpr wchar_t kParkedQuantum[] = L"KernovaSavedPrioritySeparation";

// Registry DWORD helpers, exposed for the tests. nullopt = not set / remove the value.
std::optional<DWORD> ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name);
DWORD WriteDword(HKEY root, const wchar_t* sub, const wchar_t* name, std::optional<DWORD> value);
// Keeps a setting's original in a sibling text value while the setting is changed temporarily,
// and puts it back afterwards (UnparkDword is also how a run that was killed in between is
// repaired). ParkDword refuses to overwrite an earlier parked value (ERROR_ALREADY_EXISTS).
DWORD ParkDword(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* parkedName);
DWORD UnparkDword(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* parkedName);
// True while something is parked: a run was cut short and its setting still needs putting back.
bool ParkedDwordExists(HKEY root, const wchar_t* sub, const wchar_t* parkedName);

}  // namespace mtm
