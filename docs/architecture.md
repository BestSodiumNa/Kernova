# Architecture

This document is for whoever touches the code next — likely you, six months from
now, having forgotten why `HealthPage::future_` is declared last. It explains the
shape of the program, the threading rules, the sampling layer, the UI layer, the
invariants that are easy to break, and how to add a page.

## Overview

One window, one UI thread, one shared sampler that runs on the UI thread, and
short-lived background workers per feature. There are two objects shared
between threads (`FlightRecorder` and the CPU temperature worker in
`sensors.cpp`), and their rules are documented in their own sections.

```text
wWinMain
│
├─ WinMain message loop (UI thread)
│  ├─ WM_TIMER (kTimerId) ─► Tick()
│  │    ├─ g_procs.Sample()           process list
│  │    ├─ ResolveProcessIcons()      bounded icon lookups
│  │    ├─ SampleSystem()             CPU, cores, GPU, disk, net, memory
│  │    ├─ g_recorder.Add(CaptureSample())
│  │    └─ for each page: page->OnTick(visible)
│  │
│  └─ WM_TIMER (kAnimTimerId) ─► Widget::AnimateFrame()
│
├─ FlightRecorder writer thread (only while recording)
│
└─ Per-page workers (std::jthread / std::future / std::async)
     ▲
     └─ they compute a result, then call PostToUi([...]{ ... })
        which posts WM_APP_RUN to the UI thread
```

Every page is a `Page` subclass. Controls (tables, buttons, labels, graph widgets)
are children of `g_pageHost`, a scrollable child of the main window. The host
forwards control notifications to the main window, so pages see `WM_NOTIFY`,
`WM_COMMAND`, `WM_HSCROLL`, and so on as if the controls were children of the
main window itself.

## Threading model

The rules, stated once, because violating them causes the worst bugs in this
codebase.

### The UI thread owns every HWND

All `CreateWindow*`, `ShowWindow`, `SetWindowTextW`, `MoveWindow`, `ListView_*`,
and `InvalidateRect` calls happen on the UI thread. Background workers never
touch widgets.

### The sampler runs on the UI thread inside `Tick()`

This is deliberate. `g_procs.Sample()`, `SampleSystem()`, and `CaptureSample()`
take a few milliseconds together, and keeping them on the UI thread means no
locks around `g_procs.Processes()` or `Sys()` anywhere in the UI. If you move
them to a worker, you must add synchronisation to every consumer — including
the graph pages, which read `Sys()` every tick to push a new sample into their
history.

### Background workers never touch widgets

They compute a result and then call `PostToUi`:

```cpp
worker_ = std::jthread([this, path, samples = std::move(samples)](std::stop_token) {
    std::wstring error;
    const bool ok = FlightRecorder::Save(path, samples, error);
    PostToUi([this, ok, error] {
        saving_ = false;
        UpdateButtonState();
        if (!ok) ShowError(L"Couldn't save: " + error);
    });
});
```

`PostToUi` heap-allocates a `std::function<void()>`, posts `WM_APP_RUN` with the
pointer, and the UI thread's handler runs it and deletes it. If `g_main` is
gone, `PostToUi` deletes the function itself and drops it.

### `std::jthread` members are declared last

In every class that has one, `std::jthread` members are the final members of the
class, so their destructors run before the other members they might touch.
This is the single most important convention in the codebase.

Examples worth checking when you change a page:

- `RecorderPage::saver_` — last member of `RecorderPage`.
- `ConnectionsPage::speedWorker_` and `dnsWorker_` — last members.
- `HealthPage::future_` — last member. Its destructor blocks until the query
  finishes; if it ran before `drives_` was destroyed, the pending result would
  write into freed memory.
- `BenchmarksPage::worker_` — last member.
- `OptimizePanel::refreshWorker_`, `worker_`, `scheduleWorker_` — declared last,
  in that order, so the refresh worker's destructor runs first (it's the one
  that might still be holding a reference to `this` when the panel is being
  destroyed).

Breaking this rule produces a use-after-free that only shows up on shutdown
under load, and only on some machines. It is not worth arguing about.

### `FlightRecorder` is the one shared object

It has a mutex around its state and an SPSC queue for the writer thread. The
public API is documented in `recorder.h`; the invariants are:

- `Add` is called from the UI thread (via `Tick`).
- `Start` / `Stop` / `Save` are called from the UI thread.
- The writer thread only touches `file_`, `writeQueue_`, `writeHead_`, and
  `written_`, all under `mu_`.
- The writer thread can stop itself on a write failure; the UI sees this through
  `Recording()` becoming false and `LastError()` becoming non-empty.

Any change to this class needs to be reasoned about against all four of those
points.

## Sampling layer

### `processes.cpp` — the process list

One `NtQuerySystemInformation(SystemProcessInformation)` call per tick.

**Why not `Process32Next` or `EnumProcesses`?**

- `Process32Next` gives PID, parent PID, thread count, and executable name, but
  not CPU time, working set, private working set, session, handle count, I/O
  counters, or base priority. Getting those means additional calls per process,
  which is slower and races with process exit.
- `EnumProcesses` + `OpenProcess` per PID has the same problem, and the open can
  fail for protected processes.

The single `NtQuerySystemInformation` call returns everything the UI needs in
one buffer. The cost is that the `SYSTEM_PROCESS_INFORMATION` struct is not in
the public SDK, so the layout is hand-declared in `processes.cpp`. It is
x64-specific: the field offsets depend on pointer size, and an ARM64 port needs
to verify the layout against a known build. The current layout matches Windows
10 1809 through Windows 11 24H2.

**The PID-reuse problem.** PIDs are recycled. If a process exits and another is
spawned with the same PID between two samples, the CPU-time delta computed from
the old sample would be garbage. The sampler keys its per-process CPU and I/O
history on `(pid, createTime)` instead of just `pid`, so a reused PID starts
from zero. This is a subtle bug that many tools have; if you refactor this file,
don't lose it.

**Buffer growth.** The process list can grow between the size probe and the
second call. The loop retries on `STATUS_INFO_LENGTH_MISMATCH` (`0xC0000004`),
doubling the buffer with headroom, up to six times. If it still doesn't fit,
the sample is skipped and the previous one is kept.

### `sysinfo.cpp` — CPU, cores, memory, disk, GPU, network

Per-core usage comes from `NtQuerySystemInformation` class 8
(`SystemProcessorPerformanceInformation`), which gives idle, kernel, and user
time per processor. Deltas between samples give the busy percentage. This is
the same source Windows Task Manager uses.

Per-core clock is more subtle. `CallNtPowerInformation(ProcessorInformation)`
reports the base clock on modern CPUs, not the live clock. The live clock is
computed from the PDH counter `\Processor Information(*)\% Processor Performance`
multiplied by the nominal clock that `CallNtPowerInformation` does report. When
the counter is unavailable, the reported clock is the nominal one and
performance is 100%. This is also what Task Manager does.

Core parking comes from `\Processor Information(*)\Parking Status`. A value
of 1 means the core is parked. The counter isn't available on all systems (some
AMD and Intel chips manage idle cores in firmware), in which case
`CoreInfo::parked` is -1 and the UI shows "—".

Disk and network come from PDH (`\PhysicalDisk(*)`) and `GetIfTable2`
respectively. The `PhysicalDisk` instance names are `"0 C:"`, `"1 D: E:"`, and
`"_Total"`; the parser skips `_Total` by checking the first character is a
digit.

GPU is the most fiddly. PDH exposes one counter instance per GPU engine per
process: `pid_1234_luid_0x00000000_0x0000D1F1_phys_0_eng_0_engtype_3D`. The
sampler:

1. Parses the PID and the LUID from each instance name.
2. Sums values by LUID and by (PID, LUID).
3. For each process, reports its busiest engine as the GPU percentage.
4. For each GPU, reports the busiest engine among all processes on that adapter.
5. Matches GPU memory usage to adapters by LUID.

This is what Task Manager does. It is not perfect — a process that is using two
engines heavily will look like the busiest of the two, not the sum — but it's
the same metric and it's stable.

The adapter list comes from `EnumerateGpus()` in `benchmarks.cpp`, which uses
`IDXGIFactory6::EnumAdapterByGpuPreference` to put the high-performance
(dedicated) GPU first. This matters on hybrid laptops: the integrated GPU is
adapter 0 because it drives the display, so the dedicated one would otherwise be
hidden.

### `sensors.cpp` — temperatures

GPU temperature comes from `D3DKMTQueryAdapterInfo(KMTQAITYPE_ADAPTERPERFDATA)`
on the adapter's LUID, called from `QueryGpuAdapter` on the UI thread (it is a
fast call). The driver reports tenths of a degree; adapters that don't support
it return 0 and show no temperature.

CPU temperature has no unprivileged source for the real die temperature, so
`CpuTempWorker` polls every 2 s on its own thread:

1. LibreHardwareMonitor, then OpenHardwareMonitor, over WMI
   (`ROOT\LibreHardwareMonitor` / `ROOT\OpenHardwareMonitor`, class `Sensor`).
   Prefers `CPU Package` / `Core (Tctl/Tdie)`, else the hottest CPU sensor.
2. Fallback: PDH `\Thermal Zone Information(*)\Temperature` (kelvin). This is
   the ACPI thermal zone, not the die, so the UI labels it approximate.

This is the **second object shared between threads** (after `FlightRecorder`).
It exists because WMI calls can block; the worker stores its result under a
mutex and `CpuTempNow()` returns a copy. The `std::jthread` is the last member,
per the convention above. Readings outside 1–125 °C are discarded.

The app joins this thread when it exits, so no wait on it may be long. A WMI
row gets `kWmiRowTimeoutMs` (500 ms); on a timeout the monitor is slow, not
gone, so the connection is kept and that round shows the ACPI value. The limit
does not cover `ConnectServer` or `ExecQuery` themselves.

### `net.cpp` — speed test, adapters, DNS

Speed test uses WinHTTP against `speed.cloudflare.com`, the same endpoints
Cloudflare's own web test uses. Twenty latency probes, a ten-second download on
six connections, an eight-second upload on four. Latency under load is measured
on a separate connection while the download runs. The "steady state" throughput
excludes the first second of each phase, so TCP slow-start doesn't drag the
number down.

Adapter enumeration uses `GetAdaptersAddresses` with
`GAA_FLAG_INCLUDE_GATEWAYS`. Only Ethernet and Wi-Fi are listed; VPNs, loopback,
and tunnel adapters are filtered out. The adapter's static vs DHCP DNS state is
read from the registry at
`HKLM\SYSTEM\CurrentControlSet\Services\Tcpip\Parameters\Interfaces\{GUID}`.

DNS changes use `SetInterfaceDnsSettings`, which is the modern API and
correctly handles both IPv4 and IPv6. The elevated helper pattern is described
below.

### `health.cpp` — drive health

Three-tier open per physical drive:

1. `GENERIC_READ | GENERIC_WRITE` — required for `IOCTL_ATA_PASS_THROUGH`.
2. `GENERIC_READ` — enough for `IOCTL_STORAGE_QUERY_PROPERTY` (identity,
   temperature, NVMe health log) and `IOCTL_STORAGE_PREDICT_FAILURE`.
3. Zero access — enough for identity and temperature on drives that refuse even
   a read open.

ATA SMART then uses `IOCTL_ATA_PASS_THROUGH` with the SMART signature
(`0x4F 0xC2 0xA0 0xB0`) and command bytes `0xD0` (read data) and `0xD1` (read
thresholds). The buffer is aligned to 8 bytes via a `ULONG` filler between the
`ATA_PASS_THROUGH_EX` header and the 512-byte data region.

NVMe health uses `IOCTL_STORAGE_QUERY_PROPERTY` with
`StorageDeviceProtocolSpecificProperty`, `ProtocolTypeNvme`,
`NVMeDataTypeLogPage`, and `NVME_LOG_PAGE_HEALTH_INFO` (log page `0x02`). The
returned descriptor carries the log payload at `ProtocolDataOffset` from the
start of `STORAGE_PROTOCOL_SPECIFIC_DATA`.

### `power.cpp`, `defrag.cpp`, `net.cpp`, `tweaks.cpp` — the elevated helper pattern

These files implement the same pattern:

1. The UI thread calls a "try it directly" function.
2. If it fails with `ERROR_ACCESS_DENIED` and the process isn't elevated, the
   function re-launches `Kernova.exe` elevated with a specific command-line flag
   (`--power`, `--optimize`, `--set-dns`, `--tweak`), waits for it, and returns
   its exit code. The launching code is `RunElevated` / `RunSelfElevated` in
   `power.cpp`; every helper uses it. A helper still running after its timeout
   is reported as `WAIT_TIMEOUT`, not as its (still pending) exit code.
3. The elevated instance parses its command line, performs the operation, and
   exits.

This gives one UAC prompt per action, avoids running the whole UI elevated, and
keeps the privileged code in a small, well-defined place.

The command-line parsers are a security boundary. An elevated helper that
accepts arbitrary arguments is a local privilege escalation vector. Each parser
is therefore strict:

- `--set-dns` accepts a GUID and two comma-separated IP lists (IPv4 and IPv6),
  each entry validated with `InetPton`. An empty list is `-`.
- `--optimize` accepts a single drive letter, the literal `analyze` or
  `optimize`, and the path of an existing progress file named
  `mtm-optimize-*.mtmopt`, which it opens for append only (no creating, no
  directories, no links). Everything else is rejected.
- `--power` accepts three fixed shapes: `parking on|off`, `preset <digit>`, or
  `delete <guid>`.
- `--tweak [restore]` accepts entries of three fixed shapes (`quantum`, `usb`,
  or a PCI display adapter id), each range-checked by `ValidEntry` before
  anything is written. `ValidTweakEntry` exposes it for the tests.

Two rules in `tweaks.cpp` protect the user's original values:

- Registry DWORDs are `std::optional<DWORD>` end to end (`ReadDword`,
  `WriteDword`); `nullopt` is the only "not set". Never squeeze one into an
  `int` with -1 as a sentinel: values from `0x80000000` up would read as "not
  set", and Undo would delete the real value. `CurrentEntry` also refuses to
  back up a value that `ValidEntry` would not let the helper write back, and
  `RunTweaks` then leaves that setting alone and returns `kTweakLeftUnchanged`.
- Anything changed temporarily is parked first (`ParkDword`) and restored with
  `UnparkDword`. The restore-point frequency is lifted this way. `WriteAll`
  calls `UnparkDword` before doing anything, so a helper that was killed
  mid-way is repaired by the next run.

If you add a new helper, follow the same pattern: strict argument validation, no
paths from untrusted sources, no shell metacharacters, and the operation should
be something you'd be comfortable granting to any process that can already
elevate.

## UI layer

### `common.cpp` — cross-cutting concerns

The palette, the fonts (recreated on DPI change), the settings registry under
`HKCU\Software\Kernova`, `PostToUi`, the shell file pickers, and the small
utility functions (`Format`, `FormatBytes`, `FormatCount`, `FormatFileTime`,
`ErrorText`, `ToLower`, `IsElevated`, `CopyToClipboard`, `ShowError`, `Confirm`,
`ShowInExplorer`).

If a piece of code is needed on more than one page and doesn't touch a specific
widget, it belongs here.

### `theme.cpp` — control subclasses

Four controls are subclassed to draw themselves entirely in the app theme:

- **Buttons** (`ButtonProc`) — track hover state via `TrackMouseEvent`, and paint
  in `WM_DRAWITEM` (see `DrawThemedItem` in the same file). The primary/secondary
  distinction is a window property (`mtm.primary`) so `DrawThemedItem` can pick
  the right colours.
- **Edits** (`EditProc`) — the frame, the leading icon, and the padding all live
  in the non-client area, drawn in `WM_NCPAINT`. `WM_NCCALCSIZE` shrinks the
  client rect vertically so single-line text is centred, and horizontally to
  make room for the icon and the padding. The edit control itself only draws the
  text, which is why the pill frame is pixel-perfect at any DPI.
- **Combos** (`ComboProc`) — the closed field is painted in `WM_PAINT` with a
  rounded box, the selected text, and a chevron from the icon font. The open list
  is drawn via `DrawThemedItem` (owner-draw), and the list window gets rounded
  corners and a dark scrollbar via `DwmSetWindowAttribute` and `SetWindowTheme`.
- **Trackbars** (`SliderProc`) — every part of the control is drawn by the app:
  the slim rounded track, the accent-coloured filled portion, and the round knob
  with a soft halo on hover. The stock control's own painting is suppressed in
  `WM_PAINT`, and after any message that can move the knob the whole control is
  redrawn at once. This avoids the smearing that the stock slider produces when
  dragging.

**The GDI / GDI+ split.** GDI+ draws shapes, gradients, and rounded rectangles.
GDI draws text, because GDI+ text rendering doesn't use ClearType and looks
noticeably worse at small sizes. When both are used on the same `HDC`, the GDI+
`Graphics` object must be destroyed (or `Flush`ed with `FlushIntentionSync`)
before the GDI text is drawn, or the text can appear at the wrong offset on some
drivers. See `Chart::Paint` and `Graph::PaintStatic` for the correct ordering.

### `graph.cpp` — the widget model

`Widget` is the base class for every custom-painted child. It handles:

- Double-buffered painting (`Surface` struct with a DIB section).
- DPI-aware invalidation and re-layout.
- A layered static/dynamic split for expensive animations.

The layered split is the interesting part. A widget that declares
`Layered() == true` overrides `PaintStatic` and `PaintDynamic`. The static layer
— card background, title, big value, grid lines — is rendered once into an
offscreen surface and cached. It's only re-rendered when `InvalidateStatic` is
called, when the widget is resized, or when `VisualGeneration()` changes (theme
or DPI). The dynamic layer — the scrolling series, the animating bars, the live
read-out — is drawn on top of the cached static surface every frame.

This is why the graphs can animate at 60 fps without burning CPU: the text and
the grid are not re-rendered 60 times a second.

`DrawSeries` is the core of every graph. It takes a history of values, the
visible capacity, the plot rect, a max, a colour, and a phase. The phase is how
far through the current sample interval the widget is; the whole series is
shifted right by `(1 - phase)` of a step, so between samples the line glides
left and the newest point slides in from the right edge instead of popping in.
Callers keep one extra sample beyond the capacity so the left edge stays covered
while the series is shifted.

`CoreGrid` uses a critically damped spring rather than exponential easing.
Exponential easing restarts at full speed every time a new sample lands, which
reads as a jerk once a second. The spring carries its velocity across samples,
so motion is continuous and settles without overshoot. Its speed scales with the
update interval, so it takes about one interval to arrive regardless of whether
the user has chosen 0.5 s, 1 s, or 2 s.

### `table.cpp` — the data table

A virtual (`LVS_OWNERDATA`) report ListView with click-to-sort columns, heat
shading, and a fully custom-painted header.

**Why owner-data?** With a few thousand rows (the process list, the
installed-apps list) the standard ListView allocates and stores a string per
cell per row. Owner-data stores the strings once in `Table::rows_` and hands
them to the ListView on demand. Sorting and selection are managed by the `Table`
class rather than by the ListView.

Sorting uses `Row::keys` when both rows have a numeric key for the sorting
column, and falls back to a locale-aware string comparison (`CompareStringEx`
with `SORT_DIGITSASNUMBERS`) otherwise. Columns are declared numeric or textual
at creation.

Heat shading is per-column, controlled by the `heat` field of `Column`. A
positive value is a fixed full-scale (a percentage column might use 100). A
negative value means "scale to the largest value in the column, with `|heat|` as
a floor." Cells with heat are tinted from the row background toward the accent
colour, and the tint is applied before the text is drawn, so the text stays
legible.

**Selection survival.** `Row::tag` is the stable identity of a row across
`SetRows` calls. When the table is refreshed, the previously selected tag is
looked up in the new row set; if it's still there, the same row stays selected
(possibly at a different index if sorting moved it). If the tag is gone, the
selection is cleared. This is why pages that rebuild their rows on every tick
don't lose the user's selection.

### `page.h` and the pages

`Page` is a small interface: `Title`, `Create`, `Layout`, `Show`, `OnActivate`,
`OnTick`, `OnNotify`, `OnCommand`, `OnContextMenu`, `OnScroll`,
`NeedsAttention`, `MinHeight`.

- `Create` builds the controls. Called once.
- `Layout` positions them for a given area. Called on resize, on scroll, and when
  the page becomes visible.
- `Show` toggles visibility for the whole page.
- `OnActivate` is called when the page becomes visible, and on F5. Use it for
  work that should only happen on demand (starting a query, refreshing a plan
  list).
- `OnTick(visible)` runs every update interval. It's called for every page, with
  `visible` telling the page whether it's the current one. Graph pages push
  samples regardless of visibility so their history is full when the user
  switches to them; expensive queries check `visible`.
- `NeedsAttention` returns true to draw a reminder dot next to the page's sidebar
  entry. Only the Drive Health page uses it, driven by the optimize panel's
  `due_` flag.
- `MinHeight(width)` returns the height the page needs at that width. The host
  window adds a vertical scrollbar when the current window is shorter. Return 0
  to always fit.

### `recorder.cpp` — the trace format

See `docs/formats.md` for the byte layout. What's worth knowing here:

- The writer runs on its own thread and is fed by an SPSC queue. `Add` never
  blocks on disk I/O.
- The queue is bounded at 256 samples; if the writer falls behind, the oldest
  entries are dropped rather than growing the queue without bound.
- A write failure closes the file and stops the writer thread; the UI sees
  `Recording()` become false and `LastError()` become non-empty.
- The reader memory-maps the file rather than copying it into a buffer first,
  but it still decodes every sample into memory, which takes several times the
  size on disk. That is why `Load` refuses files over 200 MiB (`kMaxFileBytes`,
  about 40 hours at 1 Hz). Raise it only together with a streaming reader.
  If the file is truncated by another process while it is being read, the
  mapped read can fault; the loader does not guard against that.

## Invariants worth writing down

These are the things that will bite you if you change them carelessly.

- Every custom-painted widget must set `SetBkMode(dc, TRANSPARENT)` before
  drawing text, and must select the right font back afterwards. `SelectObject`
  returns the previous object; restore it. A missed restore corrupts the DC for
  everything drawn after.
- Every `DrawTextW` in a themed widget must pass `DT_NOPREFIX`. Without it,
  `&` in a filename is eaten as an accelerator marker.
- A GDI+ `Graphics` object must be destroyed or `Flush`ed before the same `HDC`
  is used for GDI text. Otherwise the text renders at the wrong offset on some
  drivers. This is why `Chart::Paint` and `Graph::PaintStatic` end their GDI+
  blocks with a scoped `Graphics` before drawing text.
- `BrushFor` caches brushes for the process lifetime. The cache is not freed.
  Don't call it with an unbounded set of colours.
- The registry key is `HKCU\Software\Kernova`. Settings written under any
  other key will not be read.
- The migration from `BetterTaskManager` and `MyTaskManager` runs once per
  process, on the first call to `ReadSetting`. It will not overwrite an
  existing `Kernova` key.
- `Row::tag` must be stable across `SetRows` calls for the selection to
  survive. Pages that use vector indices must remap; pages that use PIDs,
  session IDs, or hashed names are fine.
- `IsElevated()` is cached on the first call. The process token doesn't
  change at runtime except on `RestartElevated`, which starts a new process.
- Every page is registered in `OnCreate` in a fixed order, and `kPageIcons`
  must match that order exactly. The build checks this at startup and exits
  with an error if the counts don't match.

## Adding a page

1. Add `MakeXxxPage()` to `page.h`.
2. Create `page_xxx.cpp` with a `Page` subclass. Controls are created as
   children of `g_pageHost` via `CreateChild` (from `common.cpp`) or one of the
   themed factories in `theme.h` (`CreateButton`, `CreateEdit`, `CreateCombo`,
   `CreateLabel`).
3. Register the page in `OnCreate` (in `main.cpp`) and add the same number of
   glyphs to `kPageIcons`, in the same positions.
4. Implement `Create`, `Layout`, `Show`. Add `OnActivate` for on-demand work,
   `OnTick(visible)` for anything periodic. If the page has a table, forward
   `OnNotify` to `table_.HandleNotify(nm, result)`. If it has a trackbar, handle
   `OnScroll`.
5. Override `MinHeight(width)` if the page needs a minimum height to look right.
   The host will add a scrollbar when the window is shorter.
6. If the page has a `std::jthread` or a `std::future` worker, declare it as the
   last member of the class.

## Tests

`CMakeLists.txt` builds everything except `main.cpp` as an object library
(`kernova_core`) that both `Kernova.exe` and `tests\tests.cpp` link. The tests
are a plain console program (no framework): they round-trip a trace file, check
that a corrupt, truncated, wrong-version or oversized trace is handled as
documented, and pin down what `ValidTweakEntry` accepts. Run them with
`ctest --test-dir build --output-on-failure`.

When you add parsing code or a new elevated helper, add its accept/reject cases
there. Anything the elevated helpers accept is a security boundary, so a change
to it should come with a test.

## What was deliberately not done

- **No kernel driver.** Everything is user-mode. This means some things are
  unreachable: full disk temperature on some USB bridges, some SMART attributes
  on NVMe drives behind certain RAID controllers, and per-core temperature.
  Real CPU die temperature is only shown when LibreHardwareMonitor or
  OpenHardwareMonitor is running alongside. That's an accepted limitation.
- **No always-elevated mode.** Elevation is opt-in per action via the helper
  pattern. The app never asks to run as administrator for its own sake.
- **No telemetry, no auto-update, no crash reporter.** If a user wants to report
  a bug, they can describe it. The app doesn't phone home.
- **No support for Windows 7 or 8.** The UI uses APIs and behaviours that only
  exist on Windows 10 1903 and later.
- **No theme skinning beyond the app's own palette.** The app respects the
  system light/dark preference and the system accent setting, but doesn't try to
  match arbitrary third-party themes.
