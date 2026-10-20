# Kernova

A personal Windows 10/11 task manager in plain C++20 + Win32. Every feature is unlocked;
there is no licensing or network activation code.

## Pages

| Tab | What it does | Main Windows APIs |
|---|---|---|
| Summary | One-page overview: Windows version, processor, graphics, storage, network and exposure (listening ports), signed-in users, up time; CPU and GPU temperatures | Registry, PDH, DXGI, `D3DKMTQueryAdapterInfo`, `GetExtendedTcpTable` |
| Processes | CPU, memory, GPU, I/O rate, threads, handles; search, run new task; end, suspend/resume, efficiency mode, priority, open location | `NtQuerySystemInformation`, `NtSuspendProcess`, `SetProcessInformation(ProcessPowerThrottling)` |
| Performance | Live CPU / GPU / memory / disk / network graphs and per-core tiles | `GetSystemTimes`, PDH (incl. GPU Engine counters), `GetIfTable2`, DXGI |
| Connections | Every TCP/UDP socket with owning process and exposure (all interfaces / loopback) | `GetExtendedTcpTable`, `GetExtendedUdpTable` |
| Users | Signed-in sessions with their state; disconnect, sign out, copy user name | WTS (`WTSEnumerateSessionsW`) |
| Power & Freq | Live per-core clocks, battery, power plan | `CallNtPowerInformation`, PDH `% Processor Performance` |
| Game tweaks | GPU interrupt mode and priority, scheduler time slices, USB selective suspend; can make a System Restore point first (on by default) and restore the original values | Device registry keys, `Win32PrioritySeparation`, power settings, System Restore |
| Startup apps | Run keys + Startup folders; enable/disable like Task Manager | Registry `StartupApproved` |
| Services | Status, PID, start type; start/stop/restart | Service Control Manager |
| Installed apps | Registry (4 views), MSI and Store apps; search, uninstall | Registry, `MsiEnumProductsExW`, WinRT `PackageManager` |
| Disk space | Folder sizes, 1,000 largest files, size by file type; drill down | `FindFirstFileExW` (large fetch), long paths |
| Drive Health | SMART (SATA) and NVMe health, temperature, failure prediction; drive optimization with reminders | `IOCTL_ATA_PASS_THROUGH`, `IOCTL_STORAGE_QUERY_PROPERTY`, `defrag.exe` |
| Flight Recorder | Records system and top-process samples to a `.mtmtrace` file, or saves the in-memory buffer (about an hour at 1 Hz) | PDH, `NtQuerySystemInformation`, memory-mapped file I/O |
| Benchmarks | CPU (FP64, single + multi), GPU (D3D11 compute), Disk (sequential + 4K random) | `std::thread`, Direct3D 11, unbuffered file I/O |
| Input tester | Mouse polling rate over time, buttons and wheel, keyboard key test and rollover; key depth on Wooting analog keyboards. Only reads input while Kernova is the active window | Raw Input, USB hub descriptors, HID |
| Mouse settings | Razer mice (tested on the DeathAdder V3 Pro; others such as the Viper family use the same protocol but are untested): battery, DPI stages and custom DPI, polling rate up to 8000 Hz; Windows "enhance pointer precision" | HID feature reports, `SystemParametersInfo` |

Right-click rows for actions. F5 refreshes, Ctrl+Tab / Ctrl+Shift+Tab switch pages.

**Look:** dark, light or "match Windows" theme, five accent colours, update speed and always-on-top are in **Settings** (bottom of the sidebar); the sun/moon item toggles dark/light. Number columns are heat-shaded like Windows Task Manager, and the footer shows live CPU / memory / GPU / disk / network.

Some actions (other users' processes, services, all-users startup items) need
**Settings > Restart as administrator**. For actions that ask for administrator rights, run
Kernova from a folder only administrators can write to (for example `C:\Program Files\Kernova`);
see [Security](docs/security.md#where-to-put-the-program).

## Build

Requires Visual Studio 2022 Build Tools ("Desktop development with C++", Windows 11 SDK).

```bat
build.bat
```

Output: `build\Kernova.exe` (single file, static CRT, ~1.5 MB).

The same build also makes `build\kernova_tests.exe`, which checks the trace-file loader and the
validation the elevated helpers rely on:

```bat
ctest --test-dir build --output-on-failure
```

## Layout

- `src/main.cpp` — window, sidebar, header, footer, settings menu, timer (settings in `HKCU\Software\Kernova`)
- `src/theme.*`, `src/gdip.h` — palette, dark/light mode, themed buttons/inputs, GDI+ helpers
- `src/page_*.cpp` — one file per tab
- `src/table.*` — sortable virtual ListView used by every list
- `src/graph.*` — graph cards, per-core grid, progress bar
- `src/processes.*`, `src/sysinfo.*` — shared per-tick samplers
- `src/apps.cpp`, `src/msix.cpp` — installed-app collectors
- `src/benchmarks.*` — benchmark engines (run on a worker thread)
- `src/sensors.*` — CPU / GPU / disk temperatures
- `src/power.*`, `src/defrag.*`, `src/net.*`, `src/tweaks.*` — the actions that can need administrator rights (each has its own elevated `--xxx` helper)
- `src/inputdev.*`, `src/razer.*` — input-device and Razer mouse access
- `tests/` — checks for the parsers and validators

## Documentation

- [Architecture](docs/architecture.md) — threading rules, sampling and UI layers, how to add a page
- [Design notes](docs/design.md) — motivation, design decisions, known limitations
- [File and registry formats](docs/formats.md) — `.mtmtrace`, `benchmarks.dat`, settings
- [Security](docs/security.md) — elevated helpers, network activity, reporting
- [Changelog](docs/CHANGELOG.md)

## License

[MIT](LICENSE)
