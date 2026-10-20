# Changelog

## 0.2.0

- CPU, GPU and disk temperatures on the Summary and Performance pages. GPU
  uses the WDDM driver's adapter data (same source as Task Manager). CPU shows
  the real package temperature when LibreHardwareMonitor or
  OpenHardwareMonitor is running, otherwise the approximate ACPI thermal zone.
  Disks use the storage temperature query. No driver or elevation needed.
- Game tweaks page: GPU interrupt mode and priority, scheduler time slices and
  USB selective suspend. A System Restore point can be made first (on by
  default; if it can't be made, nothing is changed), and the original values
  are kept in `tweaks-backup.txt` for "Restore original".
- Input tester page: mouse polling rate over time, buttons and wheel, keyboard
  key test and rollover, and key depth on Wooting analog keyboards.
- Mouse settings page: battery, DPI stages, custom DPI and polling rate (up to
  8000 Hz) for Razer mice, plus Windows' "enhance pointer precision".
- The elevated `--optimize` helper only appends to an existing progress file
  named like the one the app creates, and never follows a link elsewhere.
- Tweak backups are written atomically, so a crash can't leave an empty
  backup; if the backup can't be saved, nothing is applied.
- Opening a trace is limited to 200 MB (about 40 hours at 1 Hz), because
  decoded samples need several times their size on disk.
- A tweak or DNS helper that is still running after its timeout now reports a
  timeout instead of "error 259".
- Tests (`ctest --test-dir build`) for the trace loader and the helper
  validators.

## 0.1.0

Initial public release.

- Summary, Performance, Processes, Services, Startup apps, Installed apps,
  Users, Power & frequency, Disk space, Drive Health, Connections, Flight
  Recorder, and Benchmarks pages.
- Light and dark themes matching the system preference; five accent colours.
- Per-monitor v2 DPI awareness.
- Optional elevation for drive health, DNS changes, power plan management,
  drive optimization, and session control. Elevation is per-action via a
  helper process; the UI runs unelevated by default.
- `.mtmtrace` trace format, version 1: CRC32C-checked records, memory-mapped
  read path, tolerant of a truncated final record.
