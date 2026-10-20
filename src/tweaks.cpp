#include "tweaks.h"

#include "power.h"

#include <cfgmgr32.h>
#include <objbase.h>
#include <powrprof.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <srrestoreptapi.h>

#include <algorithm>
#include <climits>

namespace mtm {

// ---- registry DWORDs (public so the tests can reach them) ------------------------------

// nullopt = not there (or not a DWORD). A DWORD stays a DWORD: squeezing it into an int made values
// from 0x80000000 up look "not set", and Undo would then have deleted the user's real value.
std::optional<DWORD> ReadDword(HKEY root, const wchar_t* sub, const wchar_t* name) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueW(root, sub, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS) return std::nullopt;
    return v;
}

// nullopt removes the value (back to "not set").
DWORD WriteDword(HKEY root, const wchar_t* sub, const wchar_t* name, std::optional<DWORD> v) {
    if (!v) {
        const LSTATUS r = RegDeleteKeyValueW(root, sub, name);
        return r == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : DWORD(r);
    }
    const DWORD d = *v;
    return DWORD(RegSetKeyValueW(root, sub, name, REG_DWORD, &d, sizeof d));
}

namespace {
std::wstring NumOpt(std::optional<DWORD> v) { return v ? std::to_wstring(*v) : L"-"; }
}  // namespace

// While a setting is changed temporarily, its original is parked as text ("-" = not set) in a
// sibling value. If the process dies before the setting is put back, UnparkDword (run at the
// start of the next elevated helper) repairs it. Refuses to park over an earlier parked value:
// that one is the real original.
DWORD ParkDword(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* parkedName) {
    DWORD size = 0;
    if (RegGetValueW(root, sub, parkedName, RRF_RT_ANY, nullptr, nullptr, &size) == ERROR_SUCCESS) return ERROR_ALREADY_EXISTS;
    const std::wstring text = NumOpt(ReadDword(root, sub, name));
    return DWORD(RegSetKeyValueW(root, sub, parkedName, REG_SZ, text.c_str(), DWORD((text.size() + 1) * sizeof(wchar_t))));
}

// Puts the parked original back and removes the parked value. Nothing parked is not an error.
// A parked value that isn't ours (garbled) is removed and reported as ERROR_INVALID_DATA, with the
// setting left as it is. If the setting can't be written the parked value stays, to retry later.
DWORD UnparkDword(HKEY root, const wchar_t* sub, const wchar_t* name, const wchar_t* parkedName) {
    wchar_t text[16] = {};
    DWORD size = sizeof text;
    const LSTATUS got = RegGetValueW(root, sub, parkedName, RRF_RT_REG_SZ, nullptr, text, &size);
    if (got == ERROR_FILE_NOT_FOUND || got == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    std::optional<DWORD> original;
    bool valid = got == ERROR_SUCCESS;
    if (valid && wcscmp(text, L"-") != 0) {
        const size_t len = wcslen(text);
        valid = len >= 1 && len <= 10 && std::all_of(text, text + len, [](wchar_t c) { return c >= L'0' && c <= L'9'; });
        const unsigned long long v = valid ? wcstoull(text, nullptr, 10) : 0;
        valid = valid && v <= 0xFFFFFFFFull;
        if (valid) original = static_cast<DWORD>(v);
    }
    if (valid)
        if (const DWORD r = WriteDword(root, sub, name, original); r != ERROR_SUCCESS) return r;
    RegDeleteKeyValueW(root, sub, parkedName);
    return valid ? ERROR_SUCCESS : ERROR_INVALID_DATA;
}

namespace {

constexpr GUID kDisplayClass = {0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
constexpr GUID kSubUsb = {0x2a737441, 0x1930, 0x4402, {0x8d, 0x77, 0xb2, 0xbe, 0xbb, 0xa3, 0x08, 0xa3}};
constexpr GUID kUsbSuspend = {0x48e6b7a6, 0x50f5, 0x4782, {0xa5, 0xd4, 0x53, 0xbb, 0x8f, 0x07, 0xe2, 0x26}};

constexpr wchar_t kMsiKey[] = L"Interrupt Management\\MessageSignaledInterruptProperties";
constexpr wchar_t kAffinityKey[] = L"Interrupt Management\\Affinity Policy";
constexpr wchar_t kPriorityControl[] = L"SYSTEM\\CurrentControlSet\\Control\\PriorityControl";
constexpr wchar_t kQuantumValue[] = L"Win32PrioritySeparation";

// ---- small helpers ------------------------------------------------------------------

// The UI and the requests use an int with -1 for "not set".
std::optional<DWORD> Opt(int v) { return v < 0 ? std::nullopt : std::optional<DWORD>(DWORD(v)); }
int AsInt(std::optional<DWORD> v) { return v && *v <= INT_MAX ? int(*v) : -1; }

std::wstring Num(int v) { return v < 0 ? L"-" : std::to_wstring(v); }
int ParseNum(const wchar_t* s) { return wcscmp(s, L"-") == 0 ? -1 : int(wcstoul(s, nullptr, 10)); }

// ---- backup file: one line per tweak, "key<TAB>field<TAB>field..." -------------------

using Entry = std::vector<std::wstring>;  // [0] = key

std::wstring BackupPath() {
    PWSTR base = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        path = std::wstring(base) + L"\\Kernova";
        CreateDirectoryW(path.c_str(), nullptr);
        path += L"\\tweaks-backup.txt";
    }
    CoTaskMemFree(base);
    return path;
}

std::vector<Entry> LoadBackup() {
    std::vector<Entry> out;
    HANDLE f = CreateFileW(BackupPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::wstring text(GetFileSize(f, nullptr) / sizeof(wchar_t), L'\0');
    DWORD read = 0;
    ReadFile(f, text.data(), DWORD(text.size() * sizeof(wchar_t)), &read, nullptr);
    CloseHandle(f);
    text.resize(read / sizeof(wchar_t));
    Entry e(1);
    for (wchar_t c : text) {
        if (c == L'\t') e.emplace_back();
        else if (c == L'\n') {
            if (!e[0].empty()) out.push_back(std::move(e));
            e = Entry(1);
        } else e.back() += c;
    }
    return out;
}

// Written to a temp file and moved over the real one, so a crash or power loss leaves either the old
// backup or the new one, never an empty file that would make today's tweaked values look original.
bool SaveBackup(const std::vector<Entry>& entries) {
    std::wstring text;
    for (const Entry& e : entries) {
        for (size_t i = 0; i < e.size(); ++i) text += (i ? L"\t" : L"") + e[i];
        text += L'\n';
    }
    const std::wstring path = BackupPath();
    if (path.empty()) return false;
    const std::wstring temp = path + L".tmp";
    HANDLE f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD bytes = DWORD(text.size() * sizeof(wchar_t));
    const bool ok = WriteFile(f, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    DeleteFileW(temp.c_str());
    return false;
}

const Entry* FindEntry(const std::vector<Entry>& all, const std::wstring& key) {
    for (const Entry& e : all)
        if (_wcsicmp(e[0].c_str(), key.c_str()) == 0) return &e;
    return nullptr;
}

// ---- GPU interrupts -----------------------------------------------------------------

// MSI vectors show up as "negative" IRQ numbers in the device's allocated resources
// (that's why Device Manager lists them as huge numbers like 4294967270).
int MsiInUse(DEVINST dev) {
    LOG_CONF conf = 0;
    if (CM_Get_First_Log_Conf(&conf, dev, ALLOC_LOG_CONF) != CR_SUCCESS) return -1;
    int result = -1;
    RES_DES rd = 0, prev = conf;
    while (CM_Get_Next_Res_Des(&rd, prev, ResType_IRQ, nullptr, 0) == CR_SUCCESS) {
        if (prev != conf) CM_Free_Res_Des_Handle(prev);
        prev = rd;
        ULONG size = 0;
        if (CM_Get_Res_Des_Data_Size(&size, rd, 0) != CR_SUCCESS || size < sizeof(IRQ_DES)) continue;
        std::vector<BYTE> buf(size);
        if (CM_Get_Res_Des_Data(rd, buf.data(), size, 0) != CR_SUCCESS) continue;
        const auto* irq = reinterpret_cast<const IRQ_DES*>(buf.data());
        result = std::max(result, LONG(irq->IRQD_Alloc_Num) < 0 ? 1 : 0);
    }
    if (prev != conf) CM_Free_Res_Des_Handle(prev);
    CM_Free_Log_Conf_Handle(conf);
    return result;
}

// Calls fn for every present PCI display adapter with its Device Parameters key.
void ForEachGpu(REGSAM access, const std::function<bool(const GpuInterrupts&, HKEY, DEVINST)>& fn) {
    HDEVINFO set = SetupDiGetClassDevsW(&kDisplayClass, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return;
    SP_DEVINFO_DATA d{sizeof d};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        GpuInterrupts g;
        wchar_t buf[512] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &d, buf, ARRAYSIZE(buf), nullptr)) continue;
        g.instanceId = buf;
        if (_wcsnicmp(buf, L"PCI\\", 4) != 0) continue;  // skip remote / virtual display adapters
        if (SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, nullptr, reinterpret_cast<BYTE*>(buf), sizeof buf, nullptr) ||
            SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_DEVICEDESC, nullptr, reinterpret_cast<BYTE*>(buf), sizeof buf, nullptr))
            g.name = buf;
        HKEY key = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, access);
        if (key == INVALID_HANDLE_VALUE) key = nullptr;
        const bool more = fn(g, key, d.DevInst);
        if (key) RegCloseKey(key);
        if (!more) break;
    }
    SetupDiDestroyDeviceInfoList(set);
}

GpuInterrupts ReadGpu(const GpuInterrupts& base, HKEY key, DEVINST dev) {
    GpuInterrupts g = base;
    if (key) {
        g.msiSetting = AsInt(ReadDword(key, kMsiKey, L"MSISupported"));
        g.priority = AsInt(ReadDword(key, kAffinityKey, L"DevicePriority"));
    }
    g.msiInUse = MsiInUse(dev);
    return g;
}

// Elevated part. The instance id must name a present PCI display adapter.
DWORD WriteGpu(const std::wstring& instanceId, int msi, int priority) {
    if (!IsElevated()) return ERROR_ACCESS_DENIED;
    DWORD r = ERROR_NOT_FOUND;
    ForEachGpu(KEY_READ | KEY_WRITE, [&](const GpuInterrupts& g, HKEY key, DEVINST) {
        if (_wcsicmp(g.instanceId.c_str(), instanceId.c_str()) != 0) return true;
        if (!key) r = ERROR_ACCESS_DENIED;
        else if ((r = WriteDword(key, kMsiKey, L"MSISupported", Opt(msi))) == ERROR_SUCCESS)
            r = WriteDword(key, kAffinityKey, L"DevicePriority", Opt(priority));
        return false;
    });
    return r;
}

// ---- quantum / USB ------------------------------------------------------------------

DWORD WriteQuantum(int value) { return WriteDword(HKEY_LOCAL_MACHINE, kPriorityControl, kQuantumValue, Opt(value)); }

DWORD WriteUsb(const GUID& scheme, DWORD ac, DWORD dc) {
    DWORD r = PowerWriteACValueIndex(nullptr, &scheme, &kSubUsb, &kUsbSuspend, ac);
    if (r == ERROR_SUCCESS) r = PowerWriteDCValueIndex(nullptr, &scheme, &kSubUsb, &kUsbSuspend, dc);
    GUID active{};
    if (r == ERROR_SUCCESS && ActiveScheme(active) && memcmp(&active, &scheme, sizeof active) == 0)
        r = PowerSetActiveScheme(nullptr, &scheme);  // re-apply so it takes effect now
    return r;
}

// ---- one tweak's state = one Entry ----------------------------------------------------
// The same shape is used for the backup, for what to write and for what is there now:
//   GPU:     {instance id, MSISupported, DevicePriority}
//   quantum: {"quantum", Win32PrioritySeparation}
//   USB:     {"usb", plan guid, AC value, DC value}

bool IsQuantum(const Entry& e) { return e[0] == L"quantum"; }
bool IsUsb(const Entry& e) { return e[0] == L"usb"; }

bool SameEntry(const Entry& a, const Entry& b) {
    if (a.empty() || a.size() != b.size() || _wcsicmp(a[0].c_str(), b[0].c_str()) != 0) return false;
    return std::equal(a.begin() + 1, a.end(), b.begin() + 1, [](const auto& x, const auto& y) { return _wcsicmp(x.c_str(), y.c_str()) == 0; });
}

bool ValidEntry(const Entry& e);

// What is set right now, in the shape of `like` (for USB: in the plan `like` names), with the full
// DWORD values. Empty = not found. A value outside what ValidEntry accepts can't be restored by the
// helper later, so it is reported as `unusual` (and returned empty) instead of being backed up wrongly.
Entry CurrentEntry(const Entry& like, bool* unusual = nullptr) {
    Entry raw;
    if (IsQuantum(like)) {
        raw = {like[0], NumOpt(ReadDword(HKEY_LOCAL_MACHINE, kPriorityControl, kQuantumValue))};
    } else if (IsUsb(like)) {
        GUID g{};
        DWORD ac = 0, dc = 0;
        if (like.size() != 4 || FAILED(IIDFromString(like[1].c_str(), &g)) ||
            PowerReadACValueIndex(nullptr, &g, &kSubUsb, &kUsbSuspend, &ac) != ERROR_SUCCESS ||
            PowerReadDCValueIndex(nullptr, &g, &kSubUsb, &kUsbSuspend, &dc) != ERROR_SUCCESS)
            return {};
        raw = {like[0], like[1], std::to_wstring(ac), std::to_wstring(dc)};
    } else {
        ForEachGpu(KEY_READ, [&](const GpuInterrupts& g, HKEY key, DEVINST) {
            if (_wcsicmp(g.instanceId.c_str(), like[0].c_str()) != 0) return true;
            raw = {g.instanceId, L"-", L"-"};
            if (key) {
                raw[1] = NumOpt(ReadDword(key, kMsiKey, L"MSISupported"));
                raw[2] = NumOpt(ReadDword(key, kAffinityKey, L"DevicePriority"));
            }
            return false;
        });
    }
    if (!raw.empty() && !ValidEntry(raw)) {
        if (unusual) *unusual = true;
        return {};
    }
    return raw;
}

// Range checks for anything the elevated helper is asked to write.
bool ValidEntry(const Entry& e) {
    auto in = [](const std::wstring& s, int lo, int hi) {
        if (s == L"-") return true;
        if (s.empty() || s.size() > 3 || !std::all_of(s.begin(), s.end(), iswdigit)) return false;
        const int v = ParseNum(s.c_str());
        return v >= lo && v <= hi;
    };
    if (IsQuantum(e)) return e.size() == 2 && in(e[1], 0, 0x3F);
    if (IsUsb(e)) return e.size() == 4 && e[2] != L"-" && e[3] != L"-" && in(e[2], 0, 1) && in(e[3], 0, 1);
    return e.size() == 3 && _wcsnicmp(e[0].c_str(), L"PCI\\", 4) == 0 && in(e[1], 0, 1) && in(e[2], 0, 3);
}

DWORD WriteEntry(const Entry& e) {
    if (!ValidEntry(e)) return ERROR_INVALID_PARAMETER;
    if (IsQuantum(e)) return WriteQuantum(ParseNum(e[1].c_str()));
    if (IsUsb(e)) {
        GUID g{};
        if (FAILED(IIDFromString(e[1].c_str(), &g))) return ERROR_INVALID_PARAMETER;
        return WriteUsb(g, DWORD(ParseNum(e[2].c_str())), DWORD(ParseNum(e[3].c_str())));
    }
    return WriteGpu(e[0], ParseNum(e[1].c_str()), ParseNum(e[2].c_str()));
}

constexpr wchar_t kSystemRestoreKey[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
constexpr wchar_t kRestoreFrequency[] = L"SystemRestorePointCreationFrequency";
constexpr wchar_t kParkedFrequency[] = L"KernovaSavedFrequency";

// Puts the restore-point frequency back if an earlier run was killed while it was lifted.
void RepairRestoreFrequency() {
    UnparkDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey, kRestoreFrequency, kParkedFrequency);
}

// Elevated. Windows quietly skips a new restore point when one was made in the last 24 hours,
// so that limit is lifted for this one call and put back straight after. The original is parked
// first, so if this process is killed in between, the next helper run repairs it.
DWORD MakeRestorePoint() {
    if (!IsElevated()) return ERROR_ACCESS_DENIED;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // System Restore needs the caller to allow impersonation; fails harmlessly if COM security is already set.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                         EOAC_NONE, nullptr);
    if (const DWORD parked = ParkDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey, kRestoreFrequency, kParkedFrequency);
        parked != ERROR_SUCCESS)
        return parked;  // nothing is changed: without a way back the limit isn't lifted
    WriteDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey, kRestoreFrequency, DWORD(0));
    RESTOREPOINTINFOW info{};
    info.dwEventType = BEGIN_SYSTEM_CHANGE;
    info.dwRestorePtType = MODIFY_SETTINGS;
    wcscpy_s(info.szDescription, L"Kernova: before game tweaks");
    STATEMGRSTATUS st{};
    DWORD r = ERROR_SUCCESS;
    if (!SRSetRestorePointW(&info, &st)) r = st.nStatus ? st.nStatus : ERROR_GEN_FAILURE;
    if (r == ERROR_SUCCESS) {
        info.dwEventType = END_SYSTEM_CHANGE;
        info.llSequenceNumber = st.llSequenceNumber;
        SRSetRestorePointW(&info, &st);
    }
    RepairRestoreFrequency();  // back to what it was (removed again if it wasn't set)
    return r;
}

// Marks "the restore point failed" in the result (bit 29 = application-defined, never a Win32 code).
constexpr DWORD kPointFailedBit = 0x20000000;

// Elevated. A failed restore point stops everything: no tweak is applied without its safety net.
DWORD WriteAll(bool restorePoint, const std::vector<Entry>& entries) {
    RepairRestoreFrequency();  // an earlier run may have been killed while the limit was lifted
    if (restorePoint)
        if (const DWORD r = MakeRestorePoint(); r != ERROR_SUCCESS) return r | kPointFailedBit;
    DWORD first = ERROR_SUCCESS;
    for (const Entry& e : entries)
        if (const DWORD r = WriteEntry(e); r != ERROR_SUCCESS && first == ERROR_SUCCESS) first = r;
    return first;
}

std::wstring JoinFields(const Entry& e) {
    std::wstring s;
    for (size_t i = 0; i < e.size(); ++i) s += (i ? L"|" : L"") + e[i];
    return s;
}
}  // namespace

std::vector<GpuInterrupts> ListGpuInterrupts() {
    std::vector<GpuInterrupts> out;
    ForEachGpu(KEY_READ, [&](const GpuInterrupts& g, HKEY key, DEVINST dev) {
        out.push_back(ReadGpu(g, key, dev));
        return true;
    });
    return out;
}

QuantumState ReadQuantum() {
    QuantumState s;
    const std::optional<DWORD> v = ReadDword(HKEY_LOCAL_MACHINE, kPriorityControl, kQuantumValue);
    s.exists = v.has_value();
    if (v) s.value = *v;
    return s;
}

// Bits: AABBCC. AA quantum length (1 long, 2 short, else the edition default: short on
// desktop Windows), BB variable/fixed (1 variable, 2 fixed, else default: variable on
// desktop), CC foreground boost (0 none, 1 2×, 2/3 3×; only applies to variable quanta).
std::wstring DescribeQuantum(DWORD v) {
    const DWORD len = (v >> 4) & 3, kind = (v >> 2) & 3, boost = v & 3;
    const bool fixed = kind == 2;
    std::wstring s = len == 1 ? L"Long" : L"Short";
    s += fixed ? L" · fixed" : L" · variable";
    if (!fixed) s += boost == 0 ? L" · no boost" : boost == 1 ? L" · 2× boost" : L" · 3× boost";
    return s + Format(L"  (0x%02lX)", v);
}

const std::vector<QuantumPreset>& QuantumPresets() {
    static const std::vector<QuantumPreset> list = {
        {L"Gaming: short, fixed time slices (0x28)", 0x28},
        {L"Windows default: short, variable, 3× boost (0x02)", 0x02},
        {L"Background work: long, fixed (0x18)", 0x18},
        // Every other combination of length x fixed/variable x foreground boost, so a value a guide or
        // forum suggests can be picked from the list. The boost is only used by variable slices.
        {L"Short · variable · high boost (0x26)", 0x26},
        {L"Short · variable · medium boost (0x25)", 0x25},
        {L"Short · variable · no boost (0x24)", 0x24},
        {L"Long · variable · high boost (0x16)", 0x16},
        {L"Long · variable · medium boost (0x15)", 0x15},
        {L"Long · variable · no boost (0x14)", 0x14},
        {L"Short · fixed · high boost (0x2A)", 0x2A},
        {L"Short · fixed · medium boost (0x29)", 0x29},
        {L"Long · fixed · high boost (0x1A)", 0x1A},
        {L"Long · fixed · medium boost (0x19)", 0x19},
    };
    return list;
}

UsbSuspendState ReadUsbSuspend() {
    UsbSuspendState s;
    GUID scheme{};
    if (!ActiveScheme(scheme)) return s;
    s.known = PowerReadACValueIndex(nullptr, &scheme, &kSubUsb, &kUsbSuspend, &s.ac) == ERROR_SUCCESS &&
              PowerReadDCValueIndex(nullptr, &scheme, &kSubUsb, &kUsbSuspend, &s.dc) == ERROR_SUCCESS;
    return s;
}

// RPSessionInterval is 0 when System Restore is off for every drive; missing = unknown, assume on
// (the restore point attempt itself still reports a real failure).
bool SystemProtectionOn() {
    return ReadDword(HKEY_LOCAL_MACHINE, kSystemRestoreKey, L"RPSessionInterval").value_or(1) != 0;
}

bool HasTweakBackup(const std::wstring& key) { return FindEntry(LoadBackup(), key) != nullptr; }

bool ValidTweakEntry(const std::vector<std::wstring>& entry) { return !entry.empty() && ValidEntry(entry); }

DWORD RunTweaks(const std::vector<TweakRequest>& requests, bool restorePointFirst, bool* pointFailed) {
    struct Op {
        Entry target, original;
        bool undo = false, added = false;
    };
    std::vector<Op> ops;
    int leftUnchanged = 0;  // settings skipped because their current value can't be restored later
    std::vector<Entry> backup = LoadBackup();
    GUID scheme{};
    const bool haveScheme = ActiveScheme(scheme);
    for (const TweakRequest& q : requests) {
        Op op;
        op.undo = q.undo;
        const std::wstring key = q.kind == TweakKind::Gpu ? q.key : q.kind == TweakKind::Quantum ? L"quantum" : L"usb";
        if (q.undo) {
            const Entry* e = FindEntry(backup, key);
            if (!e) continue;
            op.target = *e;
        } else {
            if (q.kind == TweakKind::Gpu) op.target = {key, Num(q.v1), Num(q.v2)};
            else if (q.kind == TweakKind::Quantum) op.target = {key, Num(q.v1)};
            else if (haveScheme) op.target = {key, GuidText(scheme), Num(q.v1), Num(q.v2)};
            else continue;
            if (!ValidEntry(op.target)) return ERROR_INVALID_PARAMETER;
            bool unusual = false;
            op.original = CurrentEntry(op.target, &unusual);
            if (unusual) {  // would be backed up wrongly, so it is left exactly as it is
                ++leftUnchanged;
                continue;
            }
            if (op.original.empty()) continue;
            // Only the first original is kept, so Undo always means "as it was before Kernova".
            if (!FindEntry(backup, key)) {
                backup.push_back(op.original);
                op.added = true;
            }
        }
        ops.push_back(std::move(op));
    }
    // Nothing to apply: no admin prompt, and no restore point for a change that isn't happening.
    // (Only the "make a restore point" button asks for one with no tweaks, and then leftUnchanged is 0.)
    if (ops.empty() && (!restorePointFirst || leftUnchanged)) return leftUnchanged ? kTweakLeftUnchanged : ERROR_SUCCESS;
    // Written before anything changes: without the safety net nothing is applied.
    if (!SaveBackup(backup)) return ERROR_WRITE_FAULT;

    std::vector<Entry> targets;
    for (const Op& op : ops) targets.push_back(op.target);
    DWORD r;
    if (IsElevated()) {
        r = WriteAll(restorePointFirst, targets);
    } else {  // everything in one helper run = one UAC prompt
        std::wstring params = restorePointFirst ? L"--tweak restore" : L"--tweak";
        for (const Entry& e : targets) params += L" \"" + JoinFields(e) + L"\"";
        r = RunSelfElevated(params);
    }
    if (pointFailed) *pointFailed = (r & kPointFailedBit) != 0;
    r &= ~kPointFailedBit;

    // Settle the backup by what actually happened, whatever the helper reported.
    backup = LoadBackup();
    for (const Op& op : ops) {
        const Entry now = CurrentEntry(op.target);
        if ((op.undo && SameEntry(now, op.target)) || (op.added && SameEntry(now, op.original)))
            std::erase_if(backup, [&](const Entry& e) { return _wcsicmp(e[0].c_str(), op.target[0].c_str()) == 0; });
    }
    SaveBackup(backup);
    return r == ERROR_SUCCESS && leftUnchanged ? kTweakLeftUnchanged : r;
}

bool HandleTweakCommandLine(int& exitCode) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    const bool match = argc >= 2 && wcscmp(argv[1], L"--tweak") == 0;
    if (match) {
        // --tweak [restore] "field|field|..." ...  Every entry is range-checked before anything runs.
        int i = 2;
        const bool restore = i < argc && wcscmp(argv[i], L"restore") == 0;
        if (restore) ++i;
        std::vector<Entry> entries;
        bool valid = true;
        for (; i < argc; ++i) {
            Entry e(1);
            for (const wchar_t* c = argv[i]; *c; ++c) {
                if (*c == L'|') e.emplace_back();
                else e.back() += *c;
            }
            valid &= ValidEntry(e);
            entries.push_back(std::move(e));
        }
        exitCode = valid ? int(WriteAll(restore, entries)) : ERROR_INVALID_PARAMETER;
    }
    LocalFree(argv);
    return match;
}

}  // namespace mtm
