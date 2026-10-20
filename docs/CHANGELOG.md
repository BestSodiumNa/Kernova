# Changelog

## 0.2.1

- Game tweaks: the thread-scheduling list now has every combination of slice
  length, fixed or variable, and foreground boost (13 values), and its help
  text explains how to read them and says plainly that there are no published
  measurements of which is best for which game.
- Closing the app no longer waits up to 5 s for a slow LibreHardwareMonitor or
  OpenHardwareMonitor: a sensor row now has 500 ms, and a slow round shows the
  ACPI reading instead of a half-read one.
- Game tweaks read registry values with their full range. A value from
  `0x80000000` up used to look "not set", so Undo could have deleted the real
  value. A setting whose current value Kernova can't restore later is now left
  as it is, and the page says so.
- If the helper is killed while the restore-point limit is lifted, the next
  game-tweaks run puts `SystemRestorePointCreationFrequency` back.
- `security.md` explains why to run Kernova from a folder only administrators
  can write to.

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
  8000 Hz) for Razer mice, plus Windows' "enhance pointer precision". Tested
  on the DeathAdder V3 Pro. The protocol's transaction id is detected per
  mouse (0x1F, 0x3F or 0xFF), so other models such as the Viper family should
  work too, but they are untested.
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
